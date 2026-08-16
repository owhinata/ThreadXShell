/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Host test for the white balance control law (issue #35,
 * port/camera/cam_auto.c).
 *
 * WHY THIS EXISTS.  A control loop has failure modes that a single frame never
 * shows: it oscillates, or it runs away, or it hunts for ever on noise.  All
 * three look fine in one capture and are miserable in a live preview, and all
 * three are cheap to catch here by running the loop against a simulated sensor
 * for a few hundred iterations -- which is exactly what a person staring at a
 * panel cannot do reliably.
 *
 * The auto-EXPOSURE cases went with the loop they tested (issue #54): the port
 * drove the exposure only for a sensor that had none of its own, and that part
 * was the IMX219.
 */
#include <stdint.h>
#include <stdio.h>

#include "cam_auto.h"

static int failures;

#define CHECK(cond, ...)                                                       \
	do {                                                                   \
		if (!(cond)) {                                                 \
			printf("FAIL %s:%d: ", __FILE__, __LINE__);            \
			printf(__VA_ARGS__);                                   \
			printf("\n");                                          \
			failures++;                                            \
		}                                                              \
	} while (0)

/* ---- white balance converges and stays bounded --------------------------- */

static void test_awb(void)
{
	struct cam_wb wb = { CAM_WB_UNITY, CAM_WB_UNITY, CAM_WB_UNITY, 0u, 0u,
	                     CAM_SAT_UNITY };
	int i;

	/* The green-heavy frame this sensor actually produces: R 58 / G 66 /
	 * B 54.  After settling, the corrected means should be close to equal,
	 * which is what grey world is for. */
	for (i = 0; i < 100; i++)
		if (!cam_awb_step(5400u, 6600u, 5800u, &wb))
			break;

	CHECK(i < 100, "the white balance never settled");
	{
		uint32_t r_corr = (5800u * wb.r) / CAM_WB_UNITY;
		uint32_t b_corr = (5400u * wb.b) / CAM_WB_UNITY;

		CHECK(r_corr > 6000u && r_corr < 7200u,
		      "corrected red is %lu, green is 6600",
		      (unsigned long)r_corr);
		CHECK(b_corr > 6000u && b_corr < 7200u,
		      "corrected blue is %lu, green is 6600",
		      (unsigned long)b_corr);
	}
	CHECK(wb.g == CAM_WB_UNITY, "green did not stay the reference");

	/* A scene that IS one colour must not drive a channel to absurdity --
	 * grey world is wrong there, and the clamp is what keeps being wrong
	 * from being catastrophic. */
	wb.r = CAM_WB_UNITY;
	wb.b = CAM_WB_UNITY;
	for (i = 0; i < 200; i++)
		(void)cam_awb_step(100u, 6600u, 100u, &wb);
	CHECK(wb.r <= CAM_AWB_GAIN_MAX && wb.b <= CAM_AWB_GAIN_MAX,
	      "a monochrome scene drove the gains to %u / %u", wb.r, wb.b);

	/* Degenerate input is refused rather than divided by. */
	CHECK(cam_awb_step(0u, 6600u, 5800u, &wb) == 0,
	      "a zero channel mean was accepted");
	CHECK(cam_awb_step(5400u, 6600u, 5800u, NULL) == 0,
	      "a NULL white balance was accepted");
}

int main(void)
{
	test_awb();

	if (failures != 0) {
		printf("test_cam_auto: %d failure(s)\n", failures);
		return 1;
	}
	printf("test_cam_auto: OK\n");
	return 0;
}
