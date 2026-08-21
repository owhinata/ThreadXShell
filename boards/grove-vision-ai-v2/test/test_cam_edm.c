/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Host test for the EDM observer's bookkeeping (issue #68,
 * port/camera/cam_edm.c).
 *
 * WHY THIS EXISTS.  Nothing a console can type makes EDM fire.  The event this
 * observer was written for has been seen twice in a month, both times inside a
 * hang, and there is no way to ask for another one -- so without this file the
 * accumulation and the logging policy would be code that has never run, added
 * in order to explain a failure it would then fail to record.  That is the
 * failure mode this repository has already been bitten by twice: a check nobody
 * has seen work (issues #66, #42).
 *
 * [!] THE CASE THAT CARRIES THE POINT is the storm.  If the count wraps, the
 * "first, then every power of two" policy walks back through 1, 2, 4 ... and
 * starts logging again -- turning the bounded trail into a flood, in exactly
 * the situation where the log ring is the only channel left (no thread runs, so
 * `camera stats` cannot be asked) and where overwriting it destroys the boot
 * history that says what led up to the event.  Saturation is what stops that,
 * and this file is what holds saturation down.
 *
 * [!] WHAT IT DOES NOT COVER.  This compiles cam_edm.c and nothing else.  The
 * register reads, the log record's formatting and length, the registration
 * ordering against the interrupt accounting, and the deregistration before the
 * vendor stop are all in camera.c / cam_dp.c and are NOT exercised here.  What
 * holds those down is that each has exactly one call site and the reasons are
 * written at them -- not this test.
 */
#include <stdio.h>

#include "cam_edm.h"

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

/* One event, with values that are distinguishable from each other. */
static int note(struct cam_edm_state *st, uint32_t status, uint32_t tick,
                uint32_t gen)
{
	struct cam_edm_event ev;

	ev.status     = status;
	ev.mask       = 0x0003FFFFu;   /* what the datapath configuration sets */
	ev.wdt[0]     = 1u;
	ev.wdt[1]     = 2u;
	ev.wdt[2]     = 3u;
	ev.tick       = tick;
	ev.generation = gen;
	return cam_edm_note(st, &ev);
}

/*
 * A zeroed state is "nothing seen", and the first event latches what only it
 * can say.  Static storage gives this for free, which is the point: there is no
 * init call to forget, and no second flag that could disagree with the counter.
 */
static void test_first_event(void)
{
	struct cam_edm_state st = { 0 };
	struct cam_edm_state snap;

	CHECK(st.events == 0u, "a zeroed state must read as no events");

	CHECK(note(&st, 0x00000000u, 111u, 7u) == 1,
	      "the first event must be logged");
	CHECK(st.events == 1u, "events %lu, expected 1",
	      (unsigned long)st.events);
	/* Zero is the status actually observed on hardware, so it must be
	 * latched as a value and not confused with "nothing recorded". */
	CHECK(st.first_status == 0x00000000u, "first status not latched");
	CHECK(st.last_status == 0x00000000u, "last status not latched");
	CHECK(st.first_tick == 111u, "first tick %lu, expected 111",
	      (unsigned long)st.first_tick);
	CHECK(st.first_gen == 7u, "first generation %lu, expected 7",
	      (unsigned long)st.first_gen);

	/* The second event moves `last` and leaves every `first` alone. */
	(void)note(&st, 0x00040000u, 222u, 9u);
	CHECK(st.events == 2u, "events %lu, expected 2",
	      (unsigned long)st.events);
	CHECK(st.first_status == 0x00000000u, "first status was overwritten");
	CHECK(st.last_status == 0x00040000u, "last status did not move");
	CHECK(st.first_tick == 111u, "first tick was overwritten");
	CHECK(st.first_gen == 7u, "first generation was overwritten");

	cam_edm_snapshot(&st, &snap);
	CHECK(snap.events == st.events && snap.first_status == st.first_status &&
	      snap.last_status == st.last_status &&
	      snap.first_tick == st.first_tick && snap.first_gen == st.first_gen,
	      "the snapshot is not a faithful copy");
}

/*
 * Exactly which occurrences the policy selects.  Stated as a list rather than
 * recomputed from the same expression the implementation uses, so that changing
 * the expression fails here instead of agreeing with itself.
 */
static void test_selection(void)
{
	static const uint32_t logged[] = {
		1u, 2u, 4u, 8u, 16u, 32u, 64u, 128u, 256u, 512u, 1024u,
		0x10000u, 0x40000000u, 0x80000000u,
	};
	static const uint32_t quiet[] = {
		0u, 3u, 5u, 6u, 7u, 9u, 15u, 17u, 100u, 1023u, 1025u,
		0xC0000000u, 0xFFFFFFFFu,
	};
	size_t i;

	for (i = 0u; i < sizeof logged / sizeof logged[0]; i++)
		CHECK(cam_edm_should_log(logged[i]) != 0,
		      "occurrence %lu should be logged",
		      (unsigned long)logged[i]);
	for (i = 0u; i < sizeof quiet / sizeof quiet[0]; i++)
		CHECK(cam_edm_should_log(quiet[i]) == 0,
		      "occurrence %lu should NOT be logged",
		      (unsigned long)quiet[i]);
}

/* A run of events logs the powers of two and nothing else, and the number of
 * records over a long run stays logarithmic -- which is the property the ring
 * depends on. */
static void test_trail_is_bounded(void)
{
	struct cam_edm_state st = { 0 };
	uint32_t i;
	uint32_t records = 0u;

	for (i = 0u; i < 100000u; i++)
		records += (uint32_t)note(&st, 0u, i, 1u);

	CHECK(st.events == 100000u, "events %lu, expected 100000",
	      (unsigned long)st.events);
	/* 1,2,4,...,65536 is seventeen records for a hundred thousand events. */
	CHECK(records == 17u, "%lu records for 100000 events, expected 17",
	      (unsigned long)records);
}

/*
 * [!] Saturation.  Driven by writing the counter to the values around the top
 * rather than by counting there, because reaching 2^32 an event at a time is
 * not a test anyone will run.
 */
static void test_saturation(void)
{
	struct cam_edm_state st = { 0 };

	/* The last power of two still selects. */
	st.events = 0x7FFFFFFFu;
	CHECK(note(&st, 0u, 0u, 0u) == 1,
	      "the 0x80000000th event must be logged");
	CHECK(st.events == 0x80000000u, "events %lu, expected 0x80000000",
	      (unsigned long)st.events);

	/* Above it, nothing selects again. */
	st.events = 0xFFFFFFFDu;
	CHECK(note(&st, 0u, 0u, 0u) == 0, "0xFFFFFFFE must not be logged");
	CHECK(st.events == 0xFFFFFFFEu, "events %lu, expected 0xFFFFFFFE",
	      (unsigned long)st.events);

	/* And the counter STOPS rather than wrapping.  If it wrapped, the next
	 * event would be occurrence 0 and the one after it occurrence 1 -- which
	 * the policy logs, restarting the whole geometric trail. */
	CHECK(note(&st, 0u, 0u, 0u) == 0, "0xFFFFFFFF must not be logged");
	CHECK(st.events == 0xFFFFFFFFu, "events %lu, expected saturation at "
	      "0xFFFFFFFF", (unsigned long)st.events);
	CHECK(note(&st, 0u, 0u, 0u) == 0,
	      "a saturated counter must keep selecting nothing");
	CHECK(st.events == 0xFFFFFFFFu,
	      "the counter wrapped past saturation: events %lu",
	      (unsigned long)st.events);
	/* Saturated, the first-event fields are still whatever the first event
	 * put there -- the "is this the first?" test must not come back to life
	 * when the counter stops moving. */
	st.first_status = 0xDEADBEEFu;
	(void)note(&st, 0x12345678u, 999u, 3u);
	CHECK(st.first_status == 0xDEADBEEFu,
	      "a saturated counter re-latched the first event");
	CHECK(st.last_status == 0x12345678u,
	      "a saturated counter stopped tracking the last status");
}

/* Null arguments are refused rather than dereferenced: this runs in interrupt
 * context, where a fault has no diagnosis. */
static void test_null_safe(void)
{
	struct cam_edm_state st = { 0 };
	struct cam_edm_event ev = { 0 };

	CHECK(cam_edm_note(NULL, &ev) == 0, "a null state must be refused");
	CHECK(cam_edm_note(&st, NULL) == 0, "a null event must be refused");
	CHECK(st.events == 0u, "a refused call must not count");
	cam_edm_snapshot(NULL, &st);
	cam_edm_snapshot(&st, NULL);
}

int main(void)
{
	test_first_event();
	test_selection();
	test_trail_is_bounded();
	test_saturation();
	test_null_safe();

	if (failures != 0) {
		printf("test_cam_edm: %d failure(s)\n", failures);
		return 1;
	}
	printf("test_cam_edm: ok\n");
	return 0;
}
