/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    cam_dp.h
 * @brief   HX6538 DATAPATH: MIPI receiver, INP, HW5x5 demosaic, WDMA3 (#36).
 *
 * The chip side of the camera seam.  cam_sensor.h is the part side; what the
 * datapath needs to know about whichever part is fitted (geometry, link rate,
 * Bayer phase) it reads from that descriptor.
 *
 * A port of the SDK's `tflm_yolov8_od/cis_sensor/cis_imx219` glue, narrowed to
 * the one configuration this firmware uses and given return values.
 *
 * WHY IT IS A PORT AND NOT A COMPILE OF THE SDK FILE.  The SDK tree is
 * read-only here (cmake/himax_sdk.cmake refuses a dirty checkout), and the
 * donor file cannot be used as it stands anyway:
 *
 *  - it allocates its DMA buffers from `mm_reserve`, a bump allocator over a
 *    hard-coded SRAM window, which puts the frame buffer somewhere no linker
 *    script knows about and no placement gate can pin;
 *  - most of its functions return void, including the ones whose failure is
 *    the difference between a picture and a hang;
 *  - it carries the JPEG encoder leg, WDMA1/WDMA2, five resolutions and both
 *    colour spaces, none of which this port uses;
 *  - it drives the vendor's `event_handler` scheduler, which this port replaces
 *    with a direct callback into a ThreadX thread.
 *
 * What IS reused verbatim is the part that is genuinely vendor knowledge: the
 * register tables (included from the SDK tree, so they stay in sync with the
 * pin) and the exact order of the bring-up calls.
 *
 * THREADING.  Thread context, except cam_dp_retrigger().
 */
#ifndef CAM_DP_H
#define CAM_DP_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Frame geometry after the fixed INP crop/bin/subsample chain. */
#define CAM_FRAME_WIDTH   320u
#define CAM_FRAME_HEIGHT  240u
#define CAM_FRAME_PIXELS  (CAM_FRAME_WIDTH * CAM_FRAME_HEIGHT)

/** WDMA3 payload: three 8-bit planes, B then G then R. */
#define CAM_RAW_PLANES    3u
#define CAM_RAW_BYTES     (CAM_FRAME_PIXELS * CAM_RAW_PLANES)

/**
 * Chip revision C, which needs the per-frame MIPI bounce (see
 * cam_dp_needs_rev_c_bounce()).  The SDK does not export this constant --
 * every donor app re-defines it locally, and so does this port.
 */
#define CAM_CHIP_VERSION_C 0x8538000Cu

/** @return the WDMA3 landing buffer: CAM_RAW_BYTES of SRAM, 32-byte aligned. */
uint8_t *cam_dp_raw_buffer(void);

/**
 * @brief  The demosaic's Bayer phase, settable at RUNTIME.
 *
 * [!] WHY THIS IS NOT A COMPILE-TIME CONSTANT.  The donor derives it from the
 * sensor's mirror setting with a chain of #if -- but that mapping is not the
 * whole story here, for two reasons.  The donor's shipping build selects the
 * OV5647, not the IMX219, so this particular pairing is less exercised than it
 * looks; and the INP does 10:2 binning and 4:2 subsampling BEFORE the demosaic,
 * either of which can move the effective phase.
 *
 * Getting it wrong does not produce an obvious failure.  It produces a picture:
 * red and blue interpolated from positions where they do not exist, so both are
 * dragged toward the green level and the frame comes out desaturated and
 * green-cast -- which looks exactly like an under-exposed frame, and exactly
 * like no demosaic at all.  There are only four possibilities, so they are
 * reachable from the console instead of costing a flash cycle each.
 *
 * Values match DEMOS_PATTENMODE_E: 0 BGGR, 1 GBRG, 2 GRBG, 3 RGGB.  Takes
 * effect at the next datapath configuration, i.e. the next capture or preview.
 */
void cam_dp_set_bayer(uint8_t pattern);
uint8_t cam_dp_bayer(void);
const char *cam_dp_bayer_name(uint8_t pattern);

/**
 * @brief  Adopt @p pattern as the phase, unless the console has already chosen.
 *
 * Called from the sensor's power-up path with the fitted part's default.  The
 * phase is the DATAPATH's setting -- it configures the HW5x5 -- but which one
 * is right depends on the part, so the seam hands it over rather than either
 * side reaching across.
 */
void cam_dp_seed_bayer(uint8_t pattern);

/**
 * @brief  Bring the MIPI receiver (and the transmitter leg the datapath needs)
 *         up for this link.
 *
 * Selects the PLL-derived MIPI clock, resets both PHYs, programs the HS counts
 * and the computed FIFO fill (cam_mipi_calc.h), and enables two lanes.
 */
int cam_dp_csirx_enable(void);

/** Disable the MIPI receiver.  Pairs with cam_dp_csirx_enable(). */
void cam_dp_csirx_disable(void);

/**
 * @brief  Program the INP crop/bin/subsample chain and the HW5x5 -> WDMA3 leg.
 *
 * Must be re-run after every full stop: the stop performs a datapath software
 * reset, and nothing in the SDK documents which of these registers survive it.
 * Assuming they do is the kind of thing that works until it does not.
 */
int cam_dp_config(void);

/**
 * @brief  Configure the RAW leg instead: INP -> WDMA2, no demosaic.
 *
 * The buffer then holds the Bayer MOSAIC the demosaic would have been fed, at
 * 320x240 8-bit.  That is the only way to answer two questions from the board
 * rather than from theory: what the true Bayer phase is (read it off the four
 * 2x2 position means -- the two greens are highest and nearly equal), and what
 * the 8-bit samples the MIPI receiver made of the sensor's RAW10 actually look
 * like.
 */
int cam_dp_config_raw(void);

/** Start the sensor controller (the first frame follows). */
int cam_dp_capture_start(void);

/** Arm the next frame.  Cheap, and the only per-frame call on the happy path. */
void cam_dp_retrigger(void);

/**
 * @brief  Stop everything, in the vendor's order, unconditionally.
 *
 * Capture stop -> datapath software reset -> release the reset (leaving the
 * sensor controller out of it, as the vendor does) -> sensor stream off ->
 * MIPI receiver off.  This is the ONLY stop path: normal stop, timeout,
 * terminal datapath error and a failed bring-up all funnel through it, so
 * there is exactly one sequence to get right and exactly one to reason about
 * when a restart has to prove the hardware is quiet.
 */
void cam_dp_full_stop(void);

/** Latched CSI receiver error state, as read by cam_dp_csirx_errors(). */
struct cam_csirx_errors {
	uint32_t err;      /**< error IRQ status                        */
	uint32_t dphyerr;  /**< D-PHY error IRQ status                  */
	int      readable; /**< 0 if a getter failed: treat as terminal */
};

/**
 * @brief  Clear the latched CSI receiver error state.
 *
 * The status registers latch, so a read taken without clearing first reports
 * whatever went wrong before the link was last brought up.  Call this while the
 * receiver is disabled, before re-enabling it.
 *
 * @return 0 if both clears reported success, -1 otherwise.  A failed clear is
 *         not cosmetic: it means the next poll cannot be believed.
 */
int cam_dp_csirx_clear_errors(void);

/** Sample the latched receiver error state.  Never clears. */
void cam_dp_csirx_errors(struct cam_csirx_errors *out);

/** @return the chip version word (SCU), or 0 if it could not be read. */
uint32_t cam_dp_chip_version(void);

/**
 * @return non-zero when this chip needs the per-frame MIPI bounce.
 *
 * Revision C loses D-PHY lock across a frame boundary unless the receiver and
 * the sensor stream are cycled between frames; every donor application carries
 * the same workaround, gated on the same version word.  It is expensive (a full
 * receiver bring-up per frame) and it freezes the sensor's own exposure and
 * gain loops, so it is applied only where it is needed.
 */
int cam_dp_needs_rev_c_bounce(void);

#ifdef __cplusplus
}
#endif

#endif /* CAM_DP_H */
