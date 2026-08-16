/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    cam_imx219.h
 * @brief   OV5647 sensor + HX6538 datapath glue, board-owned (issue #35).
 *
 * A port of the SDK's `tflm_yolov8_od/cis_sensor/cis_imx219` glue, narrowed to
 * the one configuration this firmware uses and given return values.
 *
 * [!] THE FILE NAME NO LONGER MATCHES ITS CONTENTS (issue #54).  The IMX219 was
 * removed and the OV5647 is the only part this port drives; splitting the
 * datapath out and renaming what remains is issue #36.
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
 * THE FIXED CONFIGURATION.  640x480 RAW10 over 2 MIPI lanes -- the sensor does
 * its own first reduction with on-chip binning -> no INP crop -> 4:2 binning ->
 * 320x240 -> HW5x5 demosaic (BGGR, the part's native order with the mirror left
 * off) -> WDMA3.  That is the donor's shipping OV5647 configuration rather than
 * one invented here.
 *
 * THREADING.  Everything in this header is thread context only.  Nothing here
 * may be called from the datapath callback: these functions do I2C, spin on
 * SCU read-backs and take milliseconds.
 */
#ifndef CAM_IMX219_H
#define CAM_IMX219_H

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
 * cam_imx219_needs_rev_c_bounce()).  The SDK does not export this constant --
 * every donor app re-defines it locally, and so does this port.
 */
#define CAM_CHIP_VERSION_C 0x8538000Cu

/**
 * @brief  Ask each known part, at its own I2C address, who it is.
 *
 * With one part in the table this is an identity CHECK rather than a choice,
 * and it stays a loop over descriptors because that is the shape a second part
 * would arrive in -- and because asking is still the only way to tell a module
 * that is absent from one that is wired wrong.  Called from the power-up path;
 * the answer selects the register tables, the geometry, the link rate and the
 * default Bayer phase.
 */
int cam_imx219_detect(void);

/** @return the detected sensor's name ("ov5647"). */
const char *cam_imx219_sensor_name(void);

/** @return the model ID the DETECTED sensor is expected to answer with. */
uint16_t cam_imx219_sensor_id(void);

/** @return the WDMA3 landing buffer: CAM_RAW_BYTES of SRAM, 32-byte aligned. */
uint8_t *cam_imx219_raw_buffer(void);

/**
 * @brief  Power the module and open the sensor's I2C channel.
 *
 * Drives PA1/AON_GPIO1 high -- on this board that is the IMX219's enable, not
 * the SDK's default xSleep path -- and initialises the CIS layer.  Idempotent.
 */
int cam_imx219_power_on(void);

/** Drop the module's enable line.  Safe to call when it is already down. */
void cam_imx219_power_off(void);

/**
 * @brief  Read the sensor's 16-bit model ID over I2C.
 *
 * The cheapest end-to-end proof that the module is powered, strapped to the
 * expected I2C address and talking.  Reads whichever part cam_imx219_detect()
 * selected -- the OV5647 answers 0x5647 at 0x300A/0x300B.
 */
int cam_imx219_read_id(uint16_t *id);

/** Push the init tables: mode, binning, exposure, gains, mirror. */
int cam_imx219_sensor_init(void);

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
void cam_imx219_set_bayer(uint8_t pattern);
uint8_t cam_imx219_bayer(void);
const char *cam_imx219_bayer_name(uint8_t pattern);

/**
 * @brief  Set the sensor's exposure and gains at RUNTIME.
 *
 * Being able to change them from the console is what keeps finding the right
 * values from costing a flash cycle each, on a board whose external NOR is
 * rated ~100k of those.
 *
 * [!] On this part BOTH also switch its on-chip AEC/AGC to manual (0x3503), or
 * the sensor's own loop writes over the value on its next frame and the command
 * appears to do nothing.
 *
 * @param lines  integration time, in lines (0x3500..0x3502, sixteenths)
 * @param again  analogue gain on the console's 0..232 curve (0x350A/0x350B)
 * @param dgain  digital gain, 0x0100 == 1.0; no OV5647 equivalent, ignored
 */
int cam_imx219_set_exposure(uint16_t lines);
int cam_imx219_set_gains(uint8_t again, uint16_t dgain);

/**
 * @brief  Hand exposure back to (or take it from) the sensor's own AEC.
 *
 * The OV5647 has an on-chip AEC/AGC and the IMX219 does not, so setting an
 * exposure by hand on the OV5647 must also switch that loop off -- otherwise it
 * overwrites the value on its next frame and the command appears to do nothing.
 *
 * @return 0 on success, including on a part with no on-chip loop -- there both
 *         directions are already true and there is nothing to write.
 */
int cam_imx219_set_sensor_auto(int on);

/**
 * @brief  Re-read the sensor's exposure and gain into the reported values.
 *
 * A no-op for a part this port drives itself -- there the shadow copies ARE the
 * truth.  For a part running its own AEC they are the only truth available, and
 * without this the console reports whatever was last written by hand while the
 * real exposure moves underneath, which makes a working auto-exposure look
 * broken.  I2C: producer thread only while a stream runs.
 */
int cam_imx219_refresh_exposure_gains(void);

/** The values currently programmed (what this driver last wrote, or read). */
void cam_imx219_get_exposure_gains(uint16_t *lines, uint8_t *again,
                                   uint16_t *dgain);

/** Sensor stream on/off.  Checked, unlike the donor's. */
int cam_imx219_stream_on(void);
int cam_imx219_stream_off(void);

/**
 * @brief  Bring the MIPI receiver (and the transmitter leg the datapath needs)
 *         up for this link.
 *
 * Selects the PLL-derived MIPI clock, resets both PHYs, programs the HS counts
 * and the computed FIFO fill (cam_mipi_calc.h), and enables two lanes.
 */
int cam_imx219_csirx_enable(void);

/** Disable the MIPI receiver.  Pairs with cam_imx219_csirx_enable(). */
void cam_imx219_csirx_disable(void);

/**
 * @brief  Program the INP crop/bin/subsample chain and the HW5x5 -> WDMA3 leg.
 *
 * Must be re-run after every full stop: the stop performs a datapath software
 * reset, and nothing in the SDK documents which of these registers survive it.
 * Assuming they do is the kind of thing that works until it does not.
 */
int cam_imx219_datapath_config(void);

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
int cam_imx219_datapath_config_raw(void);

/** Start the sensor controller (the first frame follows). */
int cam_imx219_capture_start(void);

/** Arm the next frame.  Cheap, and the only per-frame call on the happy path. */
void cam_imx219_retrigger(void);

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
void cam_imx219_full_stop(void);

/** Latched CSI receiver error state, as read by cam_imx219_csirx_errors(). */
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
int cam_imx219_csirx_clear_errors(void);

/** Sample the latched receiver error state.  Never clears. */
void cam_imx219_csirx_errors(struct cam_csirx_errors *out);

/** @return the chip version word (SCU), or 0 if it could not be read. */
uint32_t cam_imx219_chip_version(void);

/**
 * @return non-zero when this chip needs the per-frame MIPI bounce.
 *
 * Revision C loses D-PHY lock across a frame boundary unless the receiver and
 * the sensor stream are cycled between frames; every donor application carries
 * the same workaround, gated on the same version word.  It is expensive (a full
 * receiver bring-up per frame) and it freezes the sensor's own exposure and
 * gain loops, so it is applied only where it is needed.
 */
int cam_imx219_needs_rev_c_bounce(void);

#ifdef __cplusplus
}
#endif

#endif /* CAM_IMX219_H */
