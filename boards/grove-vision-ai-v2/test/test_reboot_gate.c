/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Host test for the reboot's stop table (issue #68, cmds/reboot_gate.c).
 *
 * WHY THIS EXISTS.  `reboot` resets only when camera_stream_stop() confirms
 * the stop, and the refusals cannot be typed: CAM_ERR_LOCKED needs another
 * stop to hold the API mutex for the whole bounded wait, CAM_ERR_TIMEOUT a
 * producer that never acknowledges, and an undocumented code a camera.h that
 * has grown one.  The one that matters most is the last: a table written as
 * "refuse the known failures" would reset on a code nobody documented, which
 * is exactly the soft reset over a running camera that hangs the next boot.
 *
 * [!] WHAT IT DOES NOT COVER: that cmd_system.c asks the stop at all, and
 * resets only on REBOOT_RESET.  That is one call site, read by hand.
 */
#include <stdio.h>
#include <string.h>

#include "reboot_gate.h"

static int fails;

static void check(const char *what, int rc, enum reboot_verdict want)
{
	enum reboot_verdict got = reboot_verdict_of(rc);

	if (got != want) {
		printf("  FAIL %-40s (%d) -> %d, wanted %d\n", what, rc,
		       (int)got, (int)want);
		fails++;
	} else {
		printf("  ok   %-40s (%d) %d\n", what, rc, (int)got);
	}
}

/* Every refusal has a sentence, ASCII, with no line break of its own. */
static void check_text(enum reboot_verdict v)
{
	const char *t = reboot_refusal_text(v);
	const char *p;

	if (v == REBOOT_RESET) {
		if (t != NULL) {
			printf("  FAIL text(RESET) is not NULL\n");
			fails++;
		}
		return;
	}
	if (t == NULL || t[0] == '\0') {
		printf("  FAIL text(%d) is empty\n", (int)v);
		fails++;
		return;
	}
	for (p = t; *p; p++) {
		if ((unsigned char)*p < 0x20 || (unsigned char)*p > 0x7e) {
			printf("  FAIL text(%d) has a non-printable byte\n",
			       (int)v);
			fails++;
			return;
		}
	}
	printf("  ok   text(%d) \"%s\"\n", (int)v, t);
}

int main(void)
{
	int rc, swept = 0;

	printf("reboot_verdict_of: the documented codes\n");
	check("CAM_OK (joined, or nothing streaming)", REBOOT_CAM_OK,
	      REBOOT_RESET);
	check("CAM_ERR_LOCKED (never asked)", REBOOT_CAM_LOCKED,
	      REBOOT_REFUSE_LOCKED);
	check("CAM_ERR_TIMEOUT (now CAM_ST_LOST)", REBOOT_CAM_TIMEOUT,
	      REBOOT_REFUSE_LOST);
	check("CAM_ERR_STATE (poisoned / never created)", REBOOT_CAM_STATE,
	      REBOOT_REFUSE_STATE);

	printf("reboot_verdict_of: [!] undocumented codes are refused\n");
	check("CAM_ERR_PARAM (not a stop answer)", -1, REBOOT_REFUSE_UNKNOWN);
	check("CAM_ERR_HAL (not a stop answer)", -2, REBOOT_REFUSE_UNKNOWN);
	check("CAM_ERR_BUSY (not a stop answer)", -7, REBOOT_REFUSE_UNKNOWN);
	check("unknown negative", -99, REBOOT_REFUSE_UNKNOWN);
	check("positive", 1, REBOOT_REFUSE_UNKNOWN);

	/* And the sweep: exactly one value in a wide range resets. */
	for (rc = -1000; rc <= 1000; rc++) {
		enum reboot_verdict v = reboot_verdict_of(rc);

		if (v == REBOOT_RESET && rc != REBOOT_CAM_OK) {
			printf("  FAIL sweep: %d resets\n", rc);
			fails++;
		}
		if (v == REBOOT_RESET)
			swept++;
	}
	if (swept != 1) {
		printf("  FAIL sweep: %d values reset, wanted 1\n", swept);
		fails++;
	} else {
		printf("  ok   sweep -1000..1000: only CAM_OK resets\n");
	}

	printf("reboot_refusal_text\n");
	check_text(REBOOT_RESET);
	check_text(REBOOT_REFUSE_UNKNOWN);
	check_text(REBOOT_REFUSE_LOCKED);
	check_text(REBOOT_REFUSE_LOST);
	check_text(REBOOT_REFUSE_STATE);
	check_text((enum reboot_verdict)77);   /* outside the enum */

	if (fails) {
		printf("test_reboot_gate: %d failure(s)\n", fails);
		return 1;
	}
	printf("test_reboot_gate: all passed\n");
	return 0;
}
