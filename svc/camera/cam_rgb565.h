/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    cam_rgb565.h
 * @brief   The statistics `camera capture` reports for an RGB565 frame: the
 *          per-channel minimum, maximum and sum, and the seam -- the largest
 *          step between two adjacent rows' channel means (issue #237).
 *
 * f746g-disco and wio-lite-ai each used to compute these themselves (f746g-disco
 * the minimum / maximum / sum over 512-byte reads, wio-lite-ai the same plus the
 * seam over one row per read).  This is the one implementation both feed.
 *
 * [!] SHARED, SO IT OWNS NO MUTABLE STORAGE AND KNOWS NO BOARD.  The answer
 * (struct cam_rgb565_stats) and the state in between reads (struct
 * cam_rgb565_walk) are the caller's: on its stack, or -- the answer only -- in
 * the board's own record (wio-lite-ai keeps it with the frame generation it
 * describes).  No HAL, board header or tx_api.h.
 *
 * HOW TO FEED IT.  cam_rgb565_begin() with the frame's width in pixels, then
 * cam_rgb565_add() with the frame's bytes in order, in pieces of any EVEN
 * length (a pixel is two bytes and is never split across two calls; a trailing
 * odd byte of a piece is ignored, as both boards' loops did), then
 * cam_rgb565_end().  The pieces need not line up with the rows: the walk counts
 * the row boundary itself from the width (f746g-disco reads 512 bytes at a
 * time, which a WVGA row of 1600 bytes does not divide).  A last row shorter
 * than the width (a frame that is not a whole number of rows) counts as a row
 * of its own length, as wio-lite-ai's one-row reads did.  Inside a piece the
 * pixels are walked a row's worth at a time on local copies of the minimum,
 * maximum and sum (cam_rgb565_add()), so the row bookkeeping costs once per
 * row and not once per pixel.
 *
 * THE PIXEL.  Little-endian 16-bit: R = bits 15..11 (0..31), G = bits 10..5
 * (0..63), B = bits 4..0 (0..31).  Index 0 / 1 / 2 below is R / G / B.
 *
 * THE SEAM (wio-lite-ai's definition, owhinata/wio-lite-ai#44).  Each row's
 * channel mean is taken x100 and truncated (sum * 100 / pixels in that row).
 * For every row r >= 1 the step to row r - 1 is |mean(r - 1) - mean(r)| per
 * channel, ranked on the sum of the three; the largest is kept with r.  An
 * equal sum does not replace an earlier one (the first row wins), and a frame
 * whose steps are all zero answers row 0 with zero steps.
 *
 * RANGES.  Every sum stays in 32 bits for any frame up to 2^26 pixels (63 x
 * 2^26 < 2^32); the largest the boards capture is WVGA, 384000.  A row mean x100
 * needs width x 63 x 100 < 2^32, which holds up to 681,000 pixels per row.
 */
#ifndef CAM_RGB565_H
#define CAM_RGB565_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** The answer.  Valid after cam_rgb565_end(). */
struct cam_rgb565_stats {
	uint32_t min[3];      /**< per channel; 0xFFFFFFFF while no pixel is seen */
	uint32_t max[3];
	uint32_t sum[3];
	uint32_t npx;         /**< pixels seen                                     */
	uint32_t seam_row;    /**< the later row of the largest step; 0 if none    */
	uint32_t seam_d[3];   /**< that step per channel, x100                     */
	uint32_t seam_total;  /**< seam_d[0] + seam_d[1] + seam_d[2], ranked on    */
};

/** The state between two cam_rgb565_add() calls.  The caller's; moved only by
 *  the functions below. */
struct cam_rgb565_walk {
	struct cam_rgb565_stats *st;  /**< where the answer is accumulated        */
	uint32_t width;               /**< pixels per row                         */
	uint32_t col;                 /**< pixels so far in the current row       */
	uint32_t row;                 /**< the current row's index                */
	uint32_t row_start[3];        /**< the channel sums when the current row
	                               *   began (its own sums are the difference) */
	uint32_t prev_x100[3];        /**< the previous row's channel means x100  */
};

/** Start a frame @p width pixels wide; @p st is cleared and filled from here
 *  on.  A width of 0 makes the whole frame one row. */
void cam_rgb565_begin(struct cam_rgb565_walk *w, struct cam_rgb565_stats *st,
                      uint32_t width);

/** The next @p n bytes of the frame (@p n even; see "HOW TO FEED IT"). */
void cam_rgb565_add(struct cam_rgb565_walk *w, const uint8_t *buf, uint32_t n);

/** The frame is complete: a last, shorter row is counted. */
void cam_rgb565_end(struct cam_rgb565_walk *w);

#ifdef __cplusplus
}
#endif

#endif /* CAM_RGB565_H */
