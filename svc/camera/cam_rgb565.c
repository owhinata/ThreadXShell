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
		w->row_sum[k] = 0u;
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

/* The current row is complete (w->col pixels): rank its step to the one above. */
static void cam_rgb565_row_done(struct cam_rgb565_walk *w)
{
	struct cam_rgb565_stats *st = w->st;
	uint32_t mean[3], d[3], total = 0u;

	for (int k = 0; k < 3; k++) {
		mean[k] = (w->row_sum[k] * 100u) / w->col;
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
	for (int k = 0; k < 3; k++) {
		w->prev_x100[k] = mean[k];
		w->row_sum[k] = 0u;
	}
	w->col = 0u;
	w->row++;
}

void cam_rgb565_add(struct cam_rgb565_walk *w, const uint8_t *buf, uint32_t n)
{
	struct cam_rgb565_stats *st = w->st;

	for (uint32_t i = 0; i + 1u < n; i += 2u) {
		uint32_t p = (uint32_t)buf[i] | ((uint32_t)buf[i + 1u] << 8);
		uint32_t ch[3] = { (p >> 11) & 0x1Fu, (p >> 5) & 0x3Fu, p & 0x1Fu };

		for (int k = 0; k < 3; k++) {
			if (ch[k] < st->min[k]) st->min[k] = ch[k];
			if (ch[k] > st->max[k]) st->max[k] = ch[k];
			st->sum[k] += ch[k];
			w->row_sum[k] += ch[k];
		}
		st->npx++;
		if (++w->col == w->width)
			cam_rgb565_row_done(w);
	}
}

void cam_rgb565_end(struct cam_rgb565_walk *w)
{
	if (w->col != 0u)
		cam_rgb565_row_done(w);
}
