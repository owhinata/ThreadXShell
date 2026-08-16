/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    cam_convert.h
 * @brief   Planar B/G/R -> RGB565 packing and per-plane frame statistics
 *          (issue #35).
 *
 * The HX6538's HW5x5 demosaic block writes RGB as three SEPARATE 8-bit planes
 * through WDMA3 -- B first, then G, then R, each width*height bytes.  Nothing
 * downstream of it wants that: the frame pipeline carries RGB565 and the panel
 * wants RGB565, so somebody has to interleave.  There is no hardware packer for
 * this path (the HXCSC block unpacks already-packed input; it is not a Bayer or
 * planar packer, and the SDK contains no use of it), so the interleave is
 * software, and this is it.
 *
 * It lives in its own translation unit for two reasons.  It is the one piece of
 * the camera port that is pure arithmetic over memory -- no SDK, no ThreadX, no
 * MMIO -- so it can be unit-tested on the build host, which is where the
 * channel-order and endianness mistakes actually get caught.  And it is the one
 * piece that is a hot loop over 230 KB, so it is the one the board.cmake build
 * has to compile with -fno-tree-vectorize: the ThreadX Cortex-M55 port does not
 * preserve VPR across a context switch, and the post-link scan
 * (cmake/check_mve_predication.py) fails the build on any predicated MVE the
 * compiler emits.
 */
#ifndef CAM_CONVERT_H
#define CAM_CONVERT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Per-channel gain, in 1/256ths.  CAM_WB_UNITY (256) leaves a channel alone.
 *
 * This is a WHITE BALANCE, and it is in software because there is nowhere else
 * for it: this datapath has no automatic white balance and the sensor exposes
 * no per-channel gain registers -- only a global analogue and a global digital
 * one, which move all three together and so cannot correct a cast.
 *
 * A cast is the expected state of an uncorrected Bayer sensor rather than a
 * fault: the array has twice as many green photosites as red or blue and green
 * filters pass more light, so raw frames come out green-heavy.  Measured on
 * this module in room light, the channel means sat at roughly R 58 / G 66 /
 * B 54 -- about 1.2x green, which is what the eye reads as a green cast.
 */
#define CAM_WB_UNITY 256u

struct cam_wb {
	uint16_t r;
	uint16_t g;
	uint16_t b;
	/**
	 * Black level subtracted from every channel BEFORE the gain, clamped at
	 * zero.
	 *
	 * The sensor carries a fixed black pedestal: the OV5647's BLC target
	 * (0x4009) powers up at 16 and the mode table never writes it.  It is
	 * not noise and it does not go away with exposure -- it is a constant
	 * added to every pixel, so it lifts black off zero and costs contrast
	 * directly.  Measured here on a dark frame with `camera auto off`, the
	 * floor spans 8..16 rather than sitting near zero, which is why the
	 * default subtracts 16 and not the ~13 mean (see camera.c; an earlier
	 * version of this note argued the number from the IMX219's datasheet,
	 * a part this port has not had since issue #54 -- issue #61).
	 *
	 * Subtracting it before the gain is what makes the gain do what it
	 * looks like it does; scaling first would multiply the pedestal too.
	 */
	uint8_t black;
	/**
	 * Non-zero to sRGB-encode the result (see cam_convert.c).
	 *
	 * The sensor measures light, so its samples are LINEAR; the panel
	 * expects display-encoded ones.  Sending linear data to it is not a
	 * taste choice that comes out flat, it is the wrong units -- an
	 * ordinary midtone of 49 reads as nearly black.  Applied LAST, after
	 * the black level and the gain, both of which are corrections to a
	 * linear measurement.
	 */
	uint8_t gamma;
	/**
	 * Saturation, in 1/256ths.  256 leaves the colours alone.
	 *
	 * [!] WHAT THIS IS STANDING IN FOR.  A real imaging pipeline runs a
	 * COLOUR CORRECTION MATRIX between the demosaic and the display
	 * encode.  It is not decoration: a sensor's colour filters have broad,
	 * heavily OVERLAPPING spectral responses, so raw sensor RGB is
	 * inherently desaturated -- every channel sees a good deal of the light
	 * meant for its neighbours.  The matrix subtracts that crosstalk, and
	 * without it the picture comes out pale and flat no matter how well
	 * exposed and white-balanced it is.  Which is exactly what both sensors
	 * on this board produce, and why swapping them changed nothing about
	 * the colour.
	 *
	 * A correct matrix is per-sensor measured data this project does not
	 * have, so this is the honest approximation: pull each channel away
	 * from the pixel's own luma.  It restores the LOOK of saturation
	 * without claiming to be colorimetric, and it is one control instead of
	 * nine numbers nobody can source.
	 *
	 * Applied after the white balance and BEFORE the gamma -- saturation is
	 * a linear-light operation, and doing it on encoded values distorts the
	 * hue as well as the amount.
	 */
	uint16_t sat;
};

/** Saturation that leaves the image alone. */
#define CAM_SAT_UNITY 256u

/**
 * @brief  Interleave a planar 8-bit B/G/R frame into RGB565.
 *
 * @param bgr     three contiguous planes of @p pixels bytes: B, then G, then R
 *                (the WDMA3 layout -- app_get_raw_addr() in the vendor glue
 *                returns the B plane and calls it "the raw address")
 * @param out     @p pixels RGB565 samples, LITTLE-endian in memory: the
 *                pipeline's FRAME_FMT_RGB565 contract, R5<<11 | G6<<5 | B5 in a
 *                uint16_t.  This is NOT the ST7789 wire order -- lcd_blit_le()
 *                owns that swap, so that the byte order a slot claims and the
 *                byte order it holds never disagree.
 * @param pixels  width * height
 *
 * Channels are truncated, not dithered or rounded: the panel is 16-bit and the
 * error is below its own quantisation.
 */
void cam_bgr_planar_to_rgb565(const uint8_t *bgr, uint16_t *out,
                              uint32_t pixels);

/**
 * @brief  As cam_bgr_planar_to_rgb565(), with a per-channel gain applied.
 *
 * @param wb  gains in 1/256ths; NULL means unity, i.e. identical to the plain
 *            entry point above
 *
 * Gains are applied to the 8-bit samples and SATURATE at 255 before the 5/6-bit
 * truncation -- clipping a highlight is visible and wrapping it is a
 * kaleidoscope, so a gain that overflows must not wrap.
 */
void cam_bgr_planar_to_rgb565_wb(const uint8_t *bgr, uint16_t *out,
                                 uint32_t pixels, const struct cam_wb *wb);

/** Min / max / mean*100 of one 8-bit plane. */
struct cam_plane_stats {
	uint8_t  min;
	uint8_t  max;
	uint32_t mean_x100;
};

/**
 * @brief  Summarise one 8-bit plane.
 *
 * What this is for: a dead MIPI data lane, a sensor that never streamed, or a
 * demosaic fed the wrong Bayer pattern all produce a frame that LOOKS like a
 * frame -- right size, right time, every layer returning success.  The channel
 * balance is what tells them apart, which is why `camera capture` prints it
 * instead of just "ok".
 *
 * @p pixels of 0 yields min 255 / max 0 / mean 0, which reads as "empty" rather
 * than as a plausible dark frame.
 */
void cam_plane_stats(const uint8_t *plane, uint32_t pixels,
                     struct cam_plane_stats *out);

/**
 * @brief  How much 2x2 MOSAIC structure a plane still carries, times 100.
 *
 * THE QUESTION THIS ANSWERS: is this plane demosaiced colour, or is it still
 * raw Bayer?  On the glass the two are hard to tell apart -- an undemosaiced
 * frame is recognisable, just flat and oddly coloured, which is also what an
 * underexposed unbalanced frame looks like.  Arguing about it from a
 * photograph costs a flash cycle per hypothesis.
 *
 * A Bayer array carries one colour filter per photosite in a 2x2 tile, so a
 * plane that is still mosaiced has FOUR different populations interleaved: the
 * means of the four 2x2 phases differ by the difference between the colour
 * channels, which on any coloured scene is tens of counts.  A demosaiced plane
 * is one channel interpolated everywhere, so its four phase means agree to
 * within noise.
 *
 * [!] IT CANNOT TELL A RIGHT PHASE FROM A WRONG ONE.  A demosaic given the
 * wrong Bayer phase still interpolates on the 2x2 grid, so its output is just
 * as phase-flat as a correct one -- and the picture it produces (desaturated,
 * green-cast) looks like no demosaic at all, which is exactly the conclusion
 * this figure would wrongly seem to rule out.  cam_frame_colour_x100() is the
 * one that answers that question.
 *
 * @return max minus min of the four phase means, times 100.  Single digits
 *         means A demosaic ran; hundreds or thousands means the plane is still
 *         a mosaic.  0 when @p w or @p h is below 2.
 */
uint32_t cam_plane_mosaic_x100(const uint8_t *plane, uint32_t w, uint32_t h);

/**
 * @brief  Mean of each plane, x100, over a subsampled grid.
 *
 * For the white balance loop, which runs once per frame on the producer thread
 * and must not cost a frame's worth of time to do it.
 *
 * @param step  sample every @p step'th pixel (1 = all of them).  16 reads 4,800
 *              pixels of a 320x240 frame, which is far more than the loop
 *              needs -- it is steering a damped control law, not measuring
 *              anything that has to be exact.
 * @param out   three means: B, G, R, in the buffer's own plane order
 */
void cam_frame_means_x100(const uint8_t *bgr, uint32_t pixels, uint32_t step,
                          uint32_t out[3]);

/**
 * @brief  Mean of each of the four 2x2 positions in a RAW Bayer plane, x100.
 *
 * On raw mosaic data this identifies the phase without any theory about the
 * sensor's mirror setting: a Bayer tile has TWO green photosites, they sit on a
 * diagonal, and green is both the most sensitive channel and the most abundant
 * light in almost any scene -- so the two greens come out highest and nearly
 * equal to each other, and WHICH diagonal they occupy names the phase family.
 *
 * @param out  four means, indexed (y & 1) * 2 + (x & 1)
 */
void cam_bayer_phase_means_x100(const uint8_t *plane, uint32_t w, uint32_t h,
                                uint32_t out[4]);

/**
 * @brief  Mean colour separation across the three planes, times 100.
 *
 * THE QUESTION THIS ANSWERS: is the demosaic using the RIGHT Bayer phase?
 *
 * The mosaic figure above cannot tell -- a demosaic given the wrong phase still
 * interpolates on the 2x2 grid, so its output is just as phase-flat.  What a
 * wrong phase destroys is COLOUR: red and blue get interpolated from positions
 * where they do not exist, both are dragged toward the green level, and the
 * frame comes out desaturated.  So the discriminator is how far apart the three
 * channels are, pixel by pixel.
 *
 * Point the camera at something strongly coloured and try the four phases: the
 * right one is the one that maximises this.  A flat grey scene says nothing --
 * every phase scores low on it, which is worth knowing before drawing a
 * conclusion from one.
 *
 * @return mean of (max(R,G,B) - min(R,G,B)) over the frame, times 100.
 */
uint32_t cam_frame_colour_x100(const uint8_t *bgr, uint32_t pixels);

/**
 * @brief  Largest step between the means of two adjacent rows, times 100.
 *
 * Borrowed from the wio camera command, where it earned its keep: a settled
 * frame is single digits, and a gain or exposure change that landed part-way
 * through the readout shows up as hundreds.  It also catches a stride that is
 * off by a pixel, which otherwise looks like a slightly soft image.
 *
 * @param row_out  if non-NULL, receives the row index of that largest step
 * @return the step, times 100; 0 when @p h < 2
 */
uint32_t cam_plane_row_seam_x100(const uint8_t *plane, uint32_t w, uint32_t h,
                                 uint32_t *row_out);

#ifdef __cplusplus
}
#endif

#endif /* CAM_CONVERT_H */
