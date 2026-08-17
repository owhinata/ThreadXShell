/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    cam_wdma3.h
 * @brief   The WDMA3 landing-buffer transition machine (issue #59).
 *
 * Two landing buffers, alternated by explicit re-arm, so that the capture of
 * frame N+1 overlaps the CPU's work on frame N.  This module owns which buffer
 * the write-DMA is armed at and which one the CPU may read, and it is the ONLY
 * code that programs the WDMA3 channel address registers after configuration.
 *
 * WHY THIS IS ITS OWN FILE rather than a corner of cam_dp.c: the transition
 * machine is the part of issue #59 that can be WRONG SILENTLY.  A flip that is
 * a no-op -- both arms landing on buffer 0 -- produces a working picture at the
 * old frame rate, which no runtime check can tell from a working one.  So the
 * machine is separated behind the narrowest seam it can have (hx_drv_xdma.h
 * plus one retrigger hook) and compiled UNCHANGED into a host test that drives
 * it through configure -> complete -> arm -> complete against mock registers;
 * see boards/grove-vision-ai-v2/test/test_cam_wdma3.c.
 *
 * THE HARDWARE RULES it encodes, all established by disassembling the prebuilt
 * archives (there is no public TRM; the SVD names these registers but carries
 * no field descriptions):
 *
 *  - The channel addresses are programmed ONLY while xDMA is disabled.  The
 *    vendor never writes them in the enabled state -- bring-up is
 *    configure-then-enable, and hx_drv_xdma_set_WDMA3allCfg() is documented as
 *    configuring "without enable ... because after enable it will start".
 *    Whether STARTADDR latches at the enable edge or is sampled live is
 *    undocumented, so writing it live cannot be justified from anything
 *    available.
 *  - The disable is hx_drv_xdma_set_enable(0,0,0,0), the identical call the
 *    vendor's own setup_dp_disable_xDMA_TP() makes.  Not the single-bit
 *    hx_drv_xdma_set_wdma3_enable(): the wide call writes the enable register
 *    twenty times in a hardware loop -- an idiom that looks like a clock-domain
 *    crossing -- and the vendor never uses the narrow variant on this path.
 *  - The WDMA3 interrupt is MASKED across the disable, because a disable can
 *    raise XDMA_WDMA3STATUS_ERR_DIS_BEFORE_FINISH, which the camera's closed
 *    status classification makes terminal -- unmasked, the arm could fault-stop
 *    the stream every frame.  Save/restore goes through the WDMA3-specific
 *    hx_drv_xdma_get/set_WDMA3INTMask pair, which read-modify-write only the
 *    low 16 bits of the shared mask register; the vendor's own
 *    hx_drv_xdma_set_mask() writes BOTH mask registers wholesale and would
 *    destroy the mask configuration installed at set_WDMA3allCfg().
 *  - The mask is restored on EVERY exit, fault paths included.  A fault that
 *    left WDMA3 masked would make the next stream's frame-ready never arrive,
 *    presenting as a timeout somewhere unrelated.
 *
 * THREADING.  Producer thread only, except the read-side accessors, which are
 * safe wherever camera_raw_frame()'s contract already applies.
 */
#ifndef CAM_WDMA3_H
#define CAM_WDMA3_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Landing buffers.  Two is the design: one being written, one being read. */
#define CAM_WDMA3_BUFFERS 2u

/**
 * @brief  Adopt the layout the vendor's configuration just programmed.
 *
 * Call IMMEDIATELY after sensordplib_set_hw5x5_wdma3(), which derives the
 * three channel addresses from its own global and writes them for buffer 0.
 * This reads the six address/size registers BACK and derives the per-channel
 * offsets from what was actually programmed -- deliberately not a
 * re-implementation of the vendor's arithmetic, so the two cannot disagree.
 * It refuses (fail-closed) unless channel 1 starts at base[0], the channels
 * are contiguous, and the sizes sum to @p bytes.
 *
 * Also resets the indices: buffer 0 is armed and readable, the completion
 * counts restart.  The premature-disable count is cumulative and survives.
 *
 * @param  base   the two landing buffers' addresses; they must not overlap
 * @param  bytes  one landing buffer's size
 * @return 0, or -1 with the reason latched for cam_wdma3_fault()
 */
int cam_wdma3_capture_layout(const uint32_t base[CAM_WDMA3_BUFFERS],
                             uint32_t bytes);

/**
 * @brief  Reset to "buffer 0 readable, no layout" for the one-shot RAW leg.
 *
 * The RAW leg lands through WDMA2, so there is no WDMA3 layout to verify and
 * nothing may later call the streaming transitions -- they refuse without a
 * layout.  What this provides is the read index the completed-buffer accessor
 * answers from.
 */
void cam_wdma3_reset(void);

/**
 * @brief  A frame-ready arrived: verify, then commit the armed buffer as
 *         readable.
 *
 * The verify reads the six channel registers back and compares them with the
 * buffer that was armed.  [!] THAT PROVES "NOTHING MOVED THE CONFIGURATION",
 * NOT "the hardware wrote here" -- the getters echo the programmed values (a
 * disassembled fact, not an assumption).  What proves the alternation actually
 * happens is the host test plus the per-buffer completion counts.
 *
 * A mismatch means the frame is somewhere this port cannot name, so it is a
 * refusal, not a counted anomaly; the read index is left where it was.
 */
int cam_wdma3_frame_complete(void);

/**
 * @brief  Arm the capture of the next frame into the OTHER buffer.
 *
 * One routine, single exit: save mask -> mask WDMA3 -> disable xDMA ->
 * program the three channels -> read them back -> audit the interrupt status
 * -> retrigger (which re-enables) -> restore mask.
 *
 * The status audit is fail-closed.  The normal-finish bit is DISCARDED, not
 * asserted clear -- the vendor's ISR acknowledges it before frame-ready, but
 * whether the status readback then returns zero is undocumented, and a gate
 * that fires on every frame is as useless as one that never can.  Of what
 * remains: nothing -> proceed; exactly the premature-disable bit -> count it
 * (from the PRE-acknowledge sample, so the count cannot read zero while the
 * condition occurs), acknowledge, re-read and require clear; anything else ->
 * refuse without re-arming.
 *
 * On failure xDMA is left disabled, nothing is armed, and the mask has been
 * restored; the caller's fault path owns what happens next.
 */
int cam_wdma3_arm_next(void);

/** @return which landing buffer the CPU may read, 0 or 1. */
uint32_t cam_wdma3_read_index(void);

/** @return frames committed readable from buffer @p idx since the layout was
 *  captured.  Equal counts under a stream is the on-hardware evidence that the
 *  alternation is real. */
uint32_t cam_wdma3_completions(uint32_t idx);

/** @return premature-disable statuses seen at the arm, CUMULATIVE.  The mask
 *  around the disable exists to keep this zero; acceptance requires it. */
uint32_t cam_wdma3_premature_disables(void);

/** @return the first failure reason latched since the layout was captured,
 *  or NULL.  A string literal, ISR-safe to have latched. */
const char *cam_wdma3_fault(void);

/** @return the raw status word behind a fault, when one was involved. */
uint32_t cam_wdma3_fault_status(void);

/**
 * @brief  Board-owned hook: fire the vendor's re-arm.
 *
 * Implemented in cam_dp.c over sensordplib_retrigger_capture(); the host test
 * supplies a mock.  A one-function seam so this module's include surface stays
 * hx_drv_xdma.h alone -- the retrigger belongs to the sensordp layer, and
 * dragging sensor_dp_lib.h in here would drag the device tree behind it.
 */
void cam_wdma3_hw_retrigger(void);

#ifdef __cplusplus
}
#endif

#endif /* CAM_WDMA3_H */
