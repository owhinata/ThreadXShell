/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * RGB565 frame statistics and the seam (issue #237).  See cam_rgb565.h.  Pure:
 * this TU owns no storage (svc/, audited per board).
 */
#include "cam_rgb565.h"

void cam_rgb565_begin(struct cam_rgb565_walk *w, struct cam_rgb565_stats *st,
                      uint32_t width)
{
	for (int k = 0; k < 3; k++) {
		st->min[k] = 0xFFFFFFFFu;
		st->max[k] = 0u;
		st->sum[k] = 0u;
		st->seam_d[k] = 0u;
		w->row_start[k] = 0u;
		w->prev_x100[k] = 0u;
	}
	st->npx = 0u;
	st->seam_row = 0u;
	st->seam_total = 0u;
	w->st = st;
	w->width = width;
	w->col = 0u;
	w->row = 0u;
}

/* The current row is complete (w->col pixels, its channel sums @p row_sum):
   rank its step to the one above.  Once per row, so it may live in memory. */
static void cam_rgb565_row_done(struct cam_rgb565_walk *w,
                                const uint32_t row_sum[3])
{
	struct cam_rgb565_stats *st = w->st;
	uint32_t mean[3], d[3], total = 0u;

	for (int k = 0; k < 3; k++) {
		mean[k] = (row_sum[k] * 100u) / w->col;
		d[k] = (w->prev_x100[k] > mean[k]) ? w->prev_x100[k] - mean[k]
		                                   : mean[k] - w->prev_x100[k];
		total += d[k];
	}
	if (w->row != 0u && total > st->seam_total) {
		st->seam_total = total;
		st->seam_row = w->row;
		for (int k = 0; k < 3; k++)
			st->seam_d[k] = d[k];
	}
	for (int k = 0; k < 3; k++)
		w->prev_x100[k] = mean[k];
	w->col = 0u;
	w->row++;
}

/*
 * [!] THE PER-PIXEL LOOP WORKS ON LOCALS.  The minimum, maximum and sum of each
 * channel are copied out of the caller's answer on entry and back on exit, and
 * the loop runs a row's worth of pixels at a time, so the row bookkeeping is
 * once per row and the nine values can stay in registers (issue #237: kept in
 * the caller's structure through a pointer, they were loaded and stored on
 * every pixel, about twice the instructions of the loop f746g-disco had).
 * A row's own sums are the running sums less those at the row's start.
 */
void cam_rgb565_add(struct cam_rgb565_walk *w, const uint8_t *buf, uint32_t n)
{
	struct cam_rgb565_stats *st = w->st;
	uint32_t rmin = st->min[0], gmin = st->min[1], bmin = st->min[2];
	uint32_t rmax = st->max[0], gmax = st->max[1], bmax = st->max[2];
	uint32_t rsum = st->sum[0], gsum = st->sum[1], bsum = st->sum[2];
	uint32_t px = n / 2u;   /* a trailing odd byte is not a pixel */

	st->npx += px;
	while (px != 0u) {
		uint32_t seg = px;
		const uint8_t *end;

		if (w->width != 0u && seg > w->width - w->col)
			seg = w->width - w->col;
		end = buf + 2u * seg;
		for (; buf != end; buf += 2) {
			uint32_t p = (uint32_t)buf[0] | ((uint32_t)buf[1] << 8);
			uint32_t r = (p >> 11) & 0x1Fu;
			uint32_t g = (p >> 5) & 0x3Fu;
			uint32_t b = p & 0x1Fu;

			if (r < rmin) rmin = r;
			if (r > rmax) rmax = r;
			rsum += r;
			if (g < gmin) gmin = g;
			if (g > gmax) gmax = g;
			gsum += g;
			if (b < bmin) bmin = b;
			if (b > bmax) bmax = b;
			bsum += b;
		}
		px -= seg;
		w->col += seg;
		if (w->col == w->width) {
			uint32_t rs[3] = { rsum - w->row_start[0], gsum - w->row_start[1],
			                   bsum - w->row_start[2] };

			cam_rgb565_row_done(w, rs);
			w->row_start[0] = rsum;
			w->row_start[1] = gsum;
			w->row_start[2] = bsum;
		}
	}
	st->min[0] = rmin; st->min[1] = gmin; st->min[2] = bmin;
	st->max[0] = rmax; st->max[1] = gmax; st->max[2] = bmax;
	st->sum[0] = rsum; st->sum[1] = gsum; st->sum[2] = bsum;
}

void cam_rgb565_end(struct cam_rgb565_walk *w)
{
	const struct cam_rgb565_stats *st = w->st;

	if (w->col != 0u) {
		uint32_t rs[3] = { st->sum[0] - w->row_start[0],
		                   st->sum[1] - w->row_start[1],
		                   st->sum[2] - w->row_start[2] };

		cam_rgb565_row_done(w, rs);
	}
}
