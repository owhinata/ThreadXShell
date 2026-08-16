/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Grey-world white balance (issue #35).  See cam_auto.h for why the HX6538
 * datapath has none of its own, and for why the auto-exposure loop that used to
 * sit beside this went with the IMX219 (issue #54).
 */
#include "cam_auto.h"

/* Move `cur` a quarter of the way to `want`, so a scene that is genuinely one
 * colour drifts rather than snapping -- and a wrong grey-world guess is visibly
 * a drift rather than a jump. */
static uint16_t damp_gain(uint16_t cur, uint32_t want)
{
	uint32_t next;

	if (want < CAM_AWB_GAIN_MIN)
		want = CAM_AWB_GAIN_MIN;
	if (want > CAM_AWB_GAIN_MAX)
		want = CAM_AWB_GAIN_MAX;

	next = ((uint32_t)cur * 3u + want) / 4u;
	if (next < CAM_AWB_GAIN_MIN)
		next = CAM_AWB_GAIN_MIN;
	if (next > CAM_AWB_GAIN_MAX)
		next = CAM_AWB_GAIN_MAX;
	return (uint16_t)next;
}

int cam_awb_step(uint32_t b_x100, uint32_t g_x100, uint32_t r_x100,
                 struct cam_wb *wb)
{
	uint16_t old_r, old_b;

	if (wb == 0)
		return 0;
	/* Any channel at zero makes the ratio meaningless -- and it is exactly
	 * what a dark frame gives, which is when a runaway would be least
	 * noticeable and most damaging. */
	if (b_x100 == 0u || g_x100 == 0u || r_x100 == 0u)
		return 0;

	old_r = wb->r;
	old_b = wb->b;

	/* Grey world: scale red and blue so their means meet green's.  Green
	 * stays at unity deliberately -- it is the reference, and letting all
	 * three move would let the whole frame drift in brightness as a side
	 * effect of balancing it, which is the exposure loop's job. */
	wb->g = CAM_WB_UNITY;
	wb->r = damp_gain(wb->r, (CAM_WB_UNITY * g_x100) / r_x100);
	wb->b = damp_gain(wb->b, (CAM_WB_UNITY * g_x100) / b_x100);

	return (wb->r != old_r || wb->b != old_b);
}
