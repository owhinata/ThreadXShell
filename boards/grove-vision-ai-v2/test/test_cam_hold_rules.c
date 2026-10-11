/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Host test for the holder-table decisions (issue #142,
 * port/camera/cam_hold_rules.c).
 *
 * WHY THIS EXISTS.  The holder code takes three decisions that no console can
 * exercise: whether a stop may unlink the panel sink, whether a stop that came
 * back short is retried or terminal, and whether a sink that is linked to a
 * camera that stopped streaming is a stop in progress or a stream that ended by
 * itself.  The first two are opposite mistakes -- "retry" for a producer that
 * never came back, "reboot" for a microsecond of lock contention -- and the
 * third decides whether the holders are marked lost.
 *
 * [!] WHAT IT DOES NOT COVER: that cam_lcd_sink.c asks these under the holder
 * lock and acts in the same locked region.  That is held down by there being
 * one call site per question, not by this file.
 */
#include <stdio.h>

#include "cam_hold_rules.h"

static int fails;

static const int codes[] = {
	CAM_HOLD_CAM_OK, CAM_HOLD_CAM_TIMEOUT, CAM_HOLD_CAM_STATE,
	CAM_HOLD_CAM_BUSY, CAM_HOLD_CAM_LOCKED, -99, 1,
};
#define NCODES (sizeof codes / sizeof codes[0])

static void check_end(const char *what, int cam_rc, int attempted,
                      int detach_rc, enum cam_hold_end want)
{
	enum cam_hold_end got = cam_hold_end_of(cam_rc, attempted, detach_rc);

	if (got != want) {
		printf("  FAIL %-58s -> %d, wanted %d\n", what, (int)got,
		       (int)want);
		fails++;
	} else {
		printf("  ok   %-58s %d\n", what, (int)got);
	}
}

/* An independent statement of the end table, for the sweep: retry only for
 * the two answers that say nothing was lost, done only for both halves
 * confirmed, terminal for everything else. */
static enum cam_hold_end spec_end(int cam_rc, int attempted, int detach_rc)
{
	if (cam_rc == CAM_HOLD_CAM_LOCKED)
		return CAM_HOLD_END_RETRY;
	if (cam_rc != CAM_HOLD_CAM_OK || !attempted)
		return CAM_HOLD_END_TERMINAL;
	if (detach_rc == CAM_HOLD_CAM_OK)
		return CAM_HOLD_END_DONE;
	if (detach_rc == CAM_HOLD_CAM_BUSY)
		return CAM_HOLD_END_RETRY;
	return CAM_HOLD_END_TERMINAL;
}

int main(void)
{
	unsigned c, a, d, n = 0u, bad = 0u;

	printf("cam_hold_may_detach (camera.h's rule)\n");
	for (c = 0u; c < NCODES; c++) {
		int got  = cam_hold_may_detach(codes[c]);
		int want = (codes[c] == CAM_HOLD_CAM_OK);

		if (!got != !want) {
			printf("  FAIL may_detach(%d) -> %d\n", codes[c], got);
			fails++;
		} else {
			printf("  ok   may_detach(%d) %s\n", codes[c],
			       got ? "unlink" : "leave it linked");
		}
	}

	printf("cam_hold_end_of\n");
	check_end("both halves confirmed", CAM_HOLD_CAM_OK, 1, CAM_HOLD_CAM_OK,
	          CAM_HOLD_END_DONE);
	/* [!] The two retryable ones.  Fold either into terminal and an operator
	 * reboots for something a second stop finishes. */
	check_end("the camera API stayed locked: retry, nothing was asked",
	          CAM_HOLD_CAM_LOCKED, 0, 0, CAM_HOLD_END_RETRY);
	check_end("the unlink found a transition in flight: retry",
	          CAM_HOLD_CAM_OK, 1, CAM_HOLD_CAM_BUSY, CAM_HOLD_END_RETRY);
	/* The terminal ones: a thread may still be out there. */
	check_end("the producer never acknowledged",
	          CAM_HOLD_CAM_TIMEOUT, 0, 0, CAM_HOLD_END_TERMINAL);
	check_end("the camera refused the stop",
	          CAM_HOLD_CAM_STATE, 0, 0, CAM_HOLD_END_TERMINAL);
	check_end("the panel thread did not come back",
	          CAM_HOLD_CAM_OK, 1, CAM_HOLD_CAM_TIMEOUT, CAM_HOLD_END_TERMINAL);
	check_end("the unlink was refused for good",
	          CAM_HOLD_CAM_OK, 1, CAM_HOLD_CAM_STATE, CAM_HOLD_END_TERMINAL);
	/* [!] Fail closed on what nobody documented, on both halves. */
	check_end("an unknown camera code", -99, 0, 0, CAM_HOLD_END_TERMINAL);
	check_end("an unknown unlink code", CAM_HOLD_CAM_OK, 1, -99,
	          CAM_HOLD_END_TERMINAL);
	/* [!] A confirmed stop with the unlink skipped is a caller bug: the sink
	 * is still linked, so the table may not be emptied. */
	check_end("the camera stopped but the unlink was never run",
	          CAM_HOLD_CAM_OK, 0, CAM_HOLD_CAM_OK, CAM_HOLD_END_TERMINAL);
	/* A leftover unlink code does not leak into a refused stop. */
	check_end("a locked stop ignores a leftover unlink code",
	          CAM_HOLD_CAM_LOCKED, 0, CAM_HOLD_CAM_TIMEOUT,
	          CAM_HOLD_END_RETRY);

	for (c = 0u; c < NCODES; c++)
	for (a = 0u; a < 2u; a++)
	for (d = 0u; d < NCODES; d++) {
		enum cam_hold_end got  = cam_hold_end_of(codes[c], (int)a, codes[d]);
		enum cam_hold_end want = spec_end(codes[c], (int)a, codes[d]);

		n++;
		if (got != want) {
			if (bad < 8u)
				printf("  FAIL cam %d attempted %u unlink %d -> %d, "
				       "wanted %d\n", codes[c], a, codes[d],
				       (int)got, (int)want);
			bad++;
		}
		/* And the unlink is only ever DONE behind a confirmed stop. */
		if (got == CAM_HOLD_END_DONE && !cam_hold_may_detach(codes[c])) {
			printf("  FAIL done without a confirmed stop (cam %d)\n",
			       codes[c]);
			bad++;
		}
	}
	if (bad) {
		printf("  FAIL %u of %u combinations\n", bad, n);
		fails++;
	} else {
		printf("  ok   all %u combinations of (camera, unlink run, "
		       "unlink)\n", n);
	}

	printf("cam_hold_settle_of\n");
	{
		unsigned l, s, p;

		for (l = 0u; l < 2u; l++)
		for (s = 0u; s < 2u; s++)
		for (p = 0u; p < 2u; p++) {
			enum cam_hold_settle got =
				cam_hold_settle_of((int)l, (int)s, (int)p);
			/* A stop in progress is finished, never re-read as an
			 * error end (that would mark the operator's own stop
			 * as a loss); otherwise only a linked sink on a camera
			 * that stopped is one. */
			enum cam_hold_settle want =
				p ? CAM_HOLD_SETTLE_STOP
				  : (l && !s) ? CAM_HOLD_SETTLE_ERROR
				              : CAM_HOLD_SETTLE_NONE;

			if (got != want) {
				printf("  FAIL settle(linked %u, streaming %u, "
				       "stopping %u) -> %d, wanted %d\n",
				       l, s, p, (int)got, (int)want);
				fails++;
			}
		}
		if (!fails)
			printf("  ok   all 8 combinations of (linked, streaming, "
			       "stopping)\n");
	}

	if (fails) {
		printf("FAILED (%d)\n", fails);
		return 1;
	}
	printf("test_cam_hold_rules: all cases pass\n");
	return 0;
}
