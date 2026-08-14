/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Host test for the auto exposure and white balance control laws (issue #35,
 * port/camera/cam_auto.c).
 *
 * WHY THIS EXISTS.  A control loop has failure modes that a single frame never
 * shows: it oscillates, or it runs away, or it hunts for ever on noise.  All
 * three look fine in one capture and are miserable in a live preview, and all
 * three are cheap to catch here by running the loop against a simulated sensor
 * for a few hundred iterations -- which is exactly what a person staring at a
 * panel cannot do reliably.
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

/*
 * A simulated sensor: the measured mean is proportional to scene light times
 * exposure times analogue gain, saturating at full scale.  Crude, but it has
 * the one property the loop's stability depends on -- the response is
 * monotonic in both controls and the loop closes on itself.
 */
static uint32_t simulate(uint32_t light, const struct cam_ae *ae)
{
	uint64_t gain_x256 = (ae->again >= 255u)
	                   ? 256u * 256u
	                   : (256u * 256u) / (256u - ae->again);
	uint64_t v = (uint64_t)light * ae->exposure * gain_x256;

	v /= (1000u * 256u);
	if (v > 25500u)
		v = 25500u;          /* 255.00, saturated */
	return (uint32_t)v;
}

/* ---- 1. it converges, from both directions ------------------------------- */

static void test_ae_converges(void)
{
	/* Chosen so the set straddles the base settings in BOTH directions:
	 * the dim end needs gain and exposure added, the bright end needs them
	 * taken away.  A set that only ever needs more light would never
	 * exercise the branch that backs the exposure off, and would sit at the
	 * ceiling looking like convergence. */
	static const uint32_t lights[] = { 500u, 2000u, 5000u, 12000u,
	                                   30000u, 80000u };
	size_t l;

	for (l = 0; l < sizeof lights / sizeof lights[0]; l++) {
		struct cam_ae ae = { 1000u, 64u };
		uint32_t mean = 0u;
		int i;

		for (i = 0; i < 200; i++) {
			mean = simulate(lights[l], &ae);
			if (!cam_ae_step(mean, CAM_AE_TARGET_ENCODED_X100, &ae))
				break;          /* settled */
		}
		mean = simulate(lights[l], &ae);

		CHECK(i < 200, "light %lu: the loop never settled",
		      (unsigned long)lights[l]);
		/* Not every scene is reachable -- a pitch-dark one runs out of
		 * gain and exposure -- but it must get as far as its limits
		 * allow and stop, not sit oscillating. */
		if (ae.again < CAM_AE_AGAIN_MAX &&
		    ae.exposure < CAM_AE_EXPOSURE_MAX)
			CHECK(mean > CAM_AE_TARGET_ENCODED_X100 / 2u &&
			      mean < CAM_AE_TARGET_ENCODED_X100 * 2u,
			      "light %lu settled at mean %lu, target %u",
			      (unsigned long)lights[l], (unsigned long)mean,
			      CAM_AE_TARGET_ENCODED_X100);
	}
}

/* ---- 1b. the two targets really are different aims ----------------------- */

/*
 * The target is not a constant: it depends on whether the output is encoded,
 * because the loop is aiming at what the PANEL shows.  Wiring the loop to one
 * target and the converter to the other setting is a silent mistake -- the
 * picture is simply dark, or simply blown, and the loop reports success either
 * way.  So the two aims must actually differ, and each must be reached.
 */
static void test_ae_target_follows_encoding(void)
{
	/* A light both targets can actually reach -- at the ceiling the two
	 * settle in the same place and the test would pass or fail for reasons
	 * that have nothing to do with the targets. */
	const uint32_t light = 5000u;
	struct cam_ae enc = { 1000u, 64u };
	struct cam_ae lin = { 1000u, 64u };
	int i;

	CHECK(CAM_AE_TARGET_LINEAR_X100 > CAM_AE_TARGET_ENCODED_X100,
	      "the un-encoded target must be the brighter of the two");

	for (i = 0; i < 200; i++)
		if (!cam_ae_step(simulate(light, &enc),
		                 CAM_AE_TARGET_ENCODED_X100, &enc))
			break;
	for (i = 0; i < 200; i++)
		if (!cam_ae_step(simulate(light, &lin),
		                 CAM_AE_TARGET_LINEAR_X100, &lin))
			break;

	CHECK(simulate(light, &lin) > simulate(light, &enc),
	      "both targets settled at the same brightness (%lu vs %lu)",
	      (unsigned long)simulate(light, &lin),
	      (unsigned long)simulate(light, &enc));
}

/* ---- 2. it does not oscillate once settled ------------------------------- */

static void test_ae_stable(void)
{
	const uint32_t light = 5000u;
	struct cam_ae ae = { 1000u, 64u };
	struct cam_ae settled;
	uint32_t mean;
	int i, moves = 0;

	for (i = 0; i < 200; i++) {
		mean = simulate(light, &ae);
		if (!cam_ae_step(mean, CAM_AE_TARGET_ENCODED_X100, &ae))
			break;
	}
	settled = ae;

	/* Now hold the light steady and keep running the loop.  A damped loop
	 * with a deadband stops moving; an undamped one pumps for ever, which
	 * on the panel is a preview that breathes light and dark. */
	for (i = 0; i < 100; i++) {
		mean = simulate(light, &ae);
		if (cam_ae_step(mean, CAM_AE_TARGET_ENCODED_X100, &ae))
			moves++;
	}
	CHECK(moves == 0, "the loop kept adjusting %d times on a steady scene",
	      moves);
	CHECK(ae.again == settled.again && ae.exposure == settled.exposure,
	      "the settled point drifted");
}

/* ---- 3. it refuses the cases that would run it away ---------------------- */

static void test_ae_guards(void)
{
	struct cam_ae ae = { 1000u, 64u };
	struct cam_ae before = ae;

	/* Total darkness: a lens cap, or a frame that never arrived.  Ramping
	 * to maximum gain against it produces amplified noise and a long climb
	 * back down when the cap comes off. */
	CHECK(cam_ae_step(0u, CAM_AE_TARGET_ENCODED_X100, &ae) == 0, "a black frame moved the exposure");
	CHECK(ae.again == before.again && ae.exposure == before.exposure,
	      "a black frame changed the controls");

	CHECK(cam_ae_step(5000u, CAM_AE_TARGET_ENCODED_X100, NULL) == 0, "a NULL state was accepted");

	/* And the limits hold under a scene it can never reach. */
	{
		int i;

		for (i = 0; i < 300; i++)
			(void)cam_ae_step(1u, CAM_AE_TARGET_ENCODED_X100, &ae);
		CHECK(ae.again <= CAM_AE_AGAIN_MAX,
		      "gain ran past its ceiling: %u", ae.again);
		CHECK(ae.exposure <= CAM_AE_EXPOSURE_MAX,
		      "exposure ran past its ceiling: %u", ae.exposure);
		CHECK(ae.exposure >= CAM_AE_EXPOSURE_MIN,
		      "exposure fell below its floor: %u", ae.exposure);
	}
}

/* ---- 4. white balance converges and stays bounded ------------------------ */

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
	test_ae_converges();
	test_ae_target_follows_encoding();
	test_ae_stable();
	test_ae_guards();
	test_awb();

	if (failures != 0) {
		printf("test_cam_auto: %d failure(s)\n", failures);
		return 1;
	}
	printf("test_cam_auto: OK\n");
	return 0;
}
