/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Host test for the plugin lease's miss accounting (issue #127,
 * svc/plugin_lease_miss.c since issue #130).
 *
 * WHY THIS EXISTS.  A miss is a frame that reached the panel without an
 * overlay because the plugin was busy, and a frame can be lost at
 * two places -- the producer's decode and the panel's draw.  Two of the rules
 * cannot be told apart on hardware by eye: a run that the producer's success
 * resets reads 1 for a panel that never draws, and a frame counted at both
 * places reads as twice the contention there was.  Both look like a working
 * stats line.
 *
 * [!] WHAT IT DOES NOT COVER: that the board's plugin_lease.c runs every one of
 * these with interrupts disabled, and that a producer's refusal really does keep
 * the panel from asking for that frame.  The first is the board lease's, the
 * second is the callers' (issue #127 stage 2); frame() below is the order they
 * must keep.
 */
#include <stdio.h>

#include "plugin_lease_miss.h"

static int fails;

#define CHECK(cond, text)                                                     \
	do {                                                                  \
		if (!(cond)) {                                                \
			printf("  FAIL %s (line %d)\n", text, __LINE__);      \
			fails++;                                              \
		}                                                             \
	} while (0)

static void prod(struct plugin_lease_miss *m, int held)
{
	plugin_lease_miss_note(m, PLUGIN_LEASE_PRODUCER, held);
}

static void panel(struct plugin_lease_miss *m, int held)
{
	plugin_lease_miss_note(m, PLUGIN_LEASE_PANEL, held);
}

/* One frame, in the order the callers take it: the producer asks before the
 * decode, and only a frame it decoded reaches the panel's ask.  Returns 1 when
 * the frame was shown bare because of the lease. */
static int frame(struct plugin_lease_miss *m, int prod_held, int panel_held)
{
	prod(m, prod_held);
	if (!prod_held)
		return 1;
	panel(m, panel_held);
	return !panel_held;
}

static int is(const struct plugin_lease_miss *m, uint32_t total,
              uint32_t run, uint32_t worst)
{
	uint32_t t = 0xDEADu, w = 0xDEADu;

	plugin_lease_miss_read(m, &t, &w);
	return t == total && w == worst && m->run == run;
}

int main(void)
{
	struct plugin_lease_miss m;
	uint32_t lost;
	unsigned i;

	printf("test_plugin_lease_miss\n");

	plugin_lease_miss_reset(&m);
	CHECK(is(&m, 0u, 0u, 0u), "a reset period starts at zero");

	/* [!] The producer's success does not end a run.  A panel that misses
	 * every frame of a decoded stream follows each refusal with a producer
	 * success; resetting there would cap the run at 1. */
	for (i = 0u; i < 3u; i++)
		CHECK(frame(&m, 1, 0) == 1, "decoded, not drawn");
	CHECK(is(&m, 3u, 3u, 3u), "panel misses run across producer successes");

	/* An annotated frame -- the panel got the lease -- ends it. */
	CHECK(frame(&m, 1, 1) == 0, "annotated");
	CHECK(is(&m, 3u, 0u, 3u), "the panel's success ends the run, keeps worst");

	/* A producer refusal is a miss of its own and extends a run. */
	CHECK(frame(&m, 0, 1) == 1, "not decoded");
	CHECK(frame(&m, 1, 0) == 1, "decoded, not drawn");
	CHECK(is(&m, 5u, 2u, 3u), "the two places add to one run");
	CHECK(frame(&m, 0, 0) == 1 && frame(&m, 0, 0) == 1, "two more refusals");
	CHECK(is(&m, 7u, 4u, 4u), "a longer run raises worst");

	/* [!] Each lost frame is counted exactly once, wherever it was lost.  The
	 * sequence below mixes all four outcomes; the total must be the number of
	 * frames shown bare, not the number of asks. */
	plugin_lease_miss_reset(&m);
	lost = 0u;
	for (i = 0u; i < 40u; i++)
		lost += (uint32_t)frame(&m, (i % 3u) != 0u, (i % 5u) != 0u);
	{
		uint32_t t = 0u;

		plugin_lease_miss_read(&m, &t, NULL);
		CHECK(t == lost, "one count per frame shown bare");
		CHECK(lost == 14u + 5u, "the sequence is the one intended");
	}

	/* A reset is a fresh period: no run carries over. */
	plugin_lease_miss_reset(&m);
	panel(&m, 0);
	panel(&m, 0);
	plugin_lease_miss_reset(&m);
	panel(&m, 0);
	CHECK(is(&m, 1u, 1u, 1u), "a run before the reset does not continue");

	/* An asker this file does not know never ends a run. */
	plugin_lease_miss_note(&m, (enum plugin_lease_who)7, 1);
	CHECK(is(&m, 1u, 1u, 1u), "an unknown asker's success is not annotation");
	plugin_lease_miss_note(&m, (enum plugin_lease_who)7, 0);
	CHECK(is(&m, 2u, 2u, 2u), "an unknown asker's refusal still counts");

	/* The reader takes NULL for the number it does not want. */
	plugin_lease_miss_read(&m, NULL, NULL);

	if (fails) {
		printf("test_plugin_lease_miss: %d FAILED\n", fails);
		return 1;
	}
	printf("test_plugin_lease_miss: all passed\n");
	return 0;
}
