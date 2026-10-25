/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Host test for the band stream's generations (issue #142 review,
 * svc/cam_gen.h; shared with f746g-disco since issue #143).
 *
 * WHY THIS EXISTS.  `camera stream stop &` runs below the console that types
 * the next `camera stream start`, so the stop's wait can wake up to find its
 * stream gone and ANOTHER one running.  The wait used to look at "is a stream
 * active" and post a stop onto whatever it found -- stopping the new stream
 * under its holders.  That interleaving needs two consoles at two priorities
 * and cannot be typed on purpose; here it is a sequence of the same updates
 * camera.c makes under cam_lock, and the question the wait asks.
 */
#include <stdint.h>
#include <stdio.h>

#include "cam_gen.h"

static int fails;

#define EQ(got, want, what)                                                  \
	do {                                                                 \
		unsigned long g_ = (unsigned long)(got);                     \
		unsigned long w_ = (unsigned long)(want);                    \
		if (g_ != w_) {                                              \
			printf("  FAIL %s:%d %s: got 0x%lx, wanted 0x%lx\n", \
			       __func__, __LINE__, what, g_, w_);            \
			fails++;                                             \
		}                                                            \
	} while (0)

/* camera.c's state, as cam_band_start_locked() / the teardown move it. */
struct cam {
	uint32_t gen, done;
	int active;
};

static void start(struct cam *c)
{
	c->gen = cam_gen_next(c->gen);
	c->active = 1;
}

static void teardown(struct cam *c)
{
	c->done = c->gen;
	c->active = 0;
}

/* The stop's request: the generation running now, or none. */
static uint32_t stop_req(const struct cam *c)
{
	return c->active ? c->gen : CAM_GEN_NONE;
}

/* The race the review found: A stops, the producer tears down, B starts, and
 * only then does A look.  A's stream is over; B's must not look like it. */
static void test_stop_then_restart(void)
{
	struct cam c = { 0, 0, 0 };
	uint32_t a;

	start(&c);
	a = stop_req(&c);
	EQ(a != CAM_GEN_NONE, 1, "a running stream names itself");
	EQ(cam_gen_ended(c.done, a), 0, "not over before the teardown");
	teardown(&c);
	start(&c);                         /* B, before A wakes */
	EQ(c.active, 1, "B streams");
	EQ(cam_gen_ended(c.done, a), 1, "A's stream is over with B running");
	EQ(cam_gen_ended(c.done, c.gen), 0, "B's stream is not over");
}

/* The other order: A wakes while its own stream still runs -- it waits. */
static void test_wait_while_running(void)
{
	struct cam c = { 0, 0, 0 };
	uint32_t a;

	start(&c);
	teardown(&c);
	start(&c);
	a = stop_req(&c);
	EQ(cam_gen_ended(c.done, a), 0, "the previous end does not cover it");
	teardown(&c);
	EQ(cam_gen_ended(c.done, a), 1, "its own end does");
}

static void test_nothing_to_wait_for(void)
{
	struct cam c = { 0, 0, 0 };

	EQ(stop_req(&c), CAM_GEN_NONE, "idle names nothing");
	EQ(cam_gen_ended(0u, CAM_GEN_NONE), 1, "nothing is over at once");
	EQ(cam_gen_ended(0x12345678u, CAM_GEN_NONE), 1, "whatever done says");
	EQ(cam_gen_ended(0x80000001u, CAM_GEN_NONE), 1, "even half a wrap away");
}

/* The counter wraps after 2^32 starts; 0 is never handed out, and a stream
 * after the wrap is not read as long over. */
static void test_wrap(void)
{
	struct cam c = { 0xFFFFFFFEu, 0xFFFFFFFEu, 0 };
	uint32_t a;

	start(&c);
	EQ(c.gen, 0xFFFFFFFFu, "last before the wrap");
	teardown(&c);
	start(&c);
	EQ(c.gen, 1u, "the wrap skips 0");
	a = stop_req(&c);
	EQ(cam_gen_ended(c.done, a), 0, "after the wrap: not over");
	teardown(&c);
	EQ(cam_gen_ended(c.done, a), 1, "after the wrap: over");
	EQ(cam_gen_next(0u), 1u, "first generation");
}

int main(void)
{
	test_stop_then_restart();
	test_wait_while_running();
	test_nothing_to_wait_for();
	test_wrap();
	if (fails) {
		printf("test_cam_gen: %d FAILED\n", fails);
		return 1;
	}
	printf("test_cam_gen: all passed\n");
	return 0;
}
