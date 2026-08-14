/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Auto exposure and grey-world white balance (issue #35).  See cam_auto.h for
 * why the HX6538 datapath has neither of its own.
 */
#include "cam_auto.h"

/* Deadband, as a fraction of the target: inside this, do nothing.  Sensor noise
 * alone moves the measured mean by a percent or two frame to frame, and a loop
 * without a deadband spends its life correcting that. */
#define AE_DEADBAND_NUM 12u
#define AE_DEADBAND_DEN 100u

/*
 * The IMX219's analogue gain is 256 / (256 - again) -- strongly non-linear, so
 * a step in `again` near the top is worth many times one near the bottom.  The
 * loop therefore works in GAIN, not in register units, and converts back.
 */
static uint32_t again_to_gain_x256(uint8_t again)
{
	if (again >= 255u)
		return 256u * 256u;
	return (256u * 256u) / (256u - again);
}

static uint8_t gain_x256_to_again(uint32_t gain_x256)
{
	uint32_t d;

	if (gain_x256 <= 256u)
		return 0u;
	d = (256u * 256u) / gain_x256;      /* 256 - again */
	if (d >= 256u)
		return 0u;
	return (uint8_t)(256u - d);
}

int cam_ae_step(uint32_t green_x100, uint32_t target_x100, struct cam_ae *ae)
{
	uint32_t target = target_x100;
	uint32_t band;
	uint32_t gain_x256, want_x256;
	uint8_t new_again;
	uint16_t new_exposure;

	if (ae == 0 || target == 0u)
		return 0;
	band = (target * AE_DEADBAND_NUM) / AE_DEADBAND_DEN;
	/* A mean of zero means no light at all reached the sensor -- a lens cap,
	 * or a frame that never arrived.  Ramping the gain to maximum against
	 * that produces a screenful of amplified noise and then has to climb all
	 * the way back down when the cap comes off. */
	if (green_x100 == 0u)
		return 0;

	if (green_x100 + band >= target && green_x100 <= target + band)
		return 0;                        /* close enough */

	/*
	 * DAMPED: aim half way to the correction, not at it.  The sensor applies
	 * exposure and gain a frame or two after they are written, so a loop
	 * that closes the whole gap each time is reacting to a measurement that
	 * already reflects part of its own last move -- which is how an exposure
	 * loop turns into an oscillator that pumps light and dark for ever.
	 */
	gain_x256 = again_to_gain_x256(ae->again);
	want_x256 = (gain_x256 * target) / green_x100;
	want_x256 = (gain_x256 + want_x256) / 2u;

	new_again = ae->again;
	new_exposure = ae->exposure;

	if (want_x256 <= 256u) {
		/* Below unity gain: back the exposure off instead. */
		new_again = 0u;
		if (gain_x256 <= 256u + 8u) {
			uint32_t e = ((uint32_t)new_exposure * target) /
			             green_x100;

			e = ((uint32_t)new_exposure + e) / 2u;
			if (e < CAM_AE_EXPOSURE_MIN)
				e = CAM_AE_EXPOSURE_MIN;
			new_exposure = (uint16_t)e;
		}
	} else {
		uint32_t capped = again_to_gain_x256(CAM_AE_AGAIN_MAX);

		if (want_x256 <= capped) {
			new_again = gain_x256_to_again(want_x256);
		} else {
			/* Gain alone cannot get there: take it to the ceiling
			 * and put the remainder into exposure, which is the
			 * cheaper of the two in noise and the dearer in frame
			 * rate. */
			uint32_t e;

			new_again = (uint8_t)CAM_AE_AGAIN_MAX;
			e = ((uint32_t)new_exposure * want_x256) / capped;
			if (e > CAM_AE_EXPOSURE_MAX)
				e = CAM_AE_EXPOSURE_MAX;
			if (e < CAM_AE_EXPOSURE_MIN)
				e = CAM_AE_EXPOSURE_MIN;
			new_exposure = (uint16_t)e;
		}
	}

	if (new_again == ae->again && new_exposure == ae->exposure)
		return 0;
	ae->again = new_again;
	ae->exposure = new_exposure;
	return 1;
}

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
