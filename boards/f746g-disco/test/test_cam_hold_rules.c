/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Host test for this board's two decisions around the camera's holder table
 * (issue #142, port/camera/cam_hold_rules.c).
 *
 * WHY THIS EXISTS.  The stop here is asynchronous: the last holder's leaving
 * only asks the producer to tear down, and a holder can come back before the
 * producer gets there (`camera stream stop; nn stream start` on one line).  The
 * teardown then has to keep the stream -- and an ERROR teardown must never be
 * kept, whatever the table says.  The interleaving cannot be typed reliably,
 * so every input is walked here.  The nn join's format rule (D3) is walked
 * with it: the cases that matter -- a JPEG base, a non-streamable RGB565 mode,
 * a recovery pending -- are not the ones an operator reaches by accident.
 *
 * [!] WHAT IT DOES NOT COVER: that camera.c reads these under cam_lock and acts
 * in the same locked region.  Review and the hardware pass hold that.
 */
#include <stdio.h>

#include "cam_hold_rules.h"

static int fails;

#define EQ(got, want, what)                                                  \
	do {                                                                 \
		int g_ = (got), w_ = (want);                                 \
		if (g_ != w_) {                                              \
			printf("  FAIL %s:%d %s: got %d, wanted %d\n",       \
			       __func__, __LINE__, what, g_, w_);            \
			fails++;                                             \
		}                                                            \
	} while (0)

static void test_teardown(void)
{
	unsigned h;

	/* An error always tears down, whoever holds it and whoever asked. */
	for (h = 0; h <= 2; h++) {
		EQ(cam_teardown_proceeds(1, 0, h), 1, "error, no stop request");
		EQ(cam_teardown_proceeds(1, 1, h), 1, "error and a stop request");
	}
	/* The last holder left and nobody came back: tear down. */
	EQ(cam_teardown_proceeds(0, 1, 0), 1, "stop request, table empty");
	/* A holder came back before the producer got here: keep streaming. */
	EQ(cam_teardown_proceeds(0, 1, 1), 0, "stop request, a holder came back");
	EQ(cam_teardown_proceeds(0, 1, 2), 0, "stop request, two came back");
	/* Nothing asks any more (a start withdrew the request): keep streaming. */
	EQ(cam_teardown_proceeds(0, 0, 0), 0, "no reason at all");
	EQ(cam_teardown_proceeds(0, 0, 1), 0, "no reason, held");
}

static void test_join_format(void)
{
	/* RGB565 in a mode that streams: the only yes -- off (nn starts it),
	   running, or between an overrun and its recovery alike. */
	EQ(cam_join_format_ok(1, 1), 1, "RGB565 streamable");
	/* Capture-only RGB565 (VGA): refused even with a recovery pending, which
	   would only fail to re-arm and lose the holders (review F3). */
	EQ(cam_join_format_ok(1, 0), 0, "RGB565 capture-only (VGA)");
	/* JPEG / YUV: nn reads RGB565 only. */
	EQ(cam_join_format_ok(0, 1), 0, "JPEG / YUV streamable");
	EQ(cam_join_format_ok(0, 0), 0, "neither");
}

int main(void)
{
	test_teardown();
	test_join_format();
	if (fails) {
		printf("test_cam_hold_rules: %d FAILED\n", fails);
		return 1;
	}
	printf("test_cam_hold_rules: all passed\n");
	return 0;
}
