/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    cam_gen.h
 * @brief   Which band stream a stop is waiting for (issue #142).
 *
 * Every band start takes a fresh generation under cam_lock, and the teardown
 * records the generation it ended, under cam_lock too.  A holder that leaves
 * last asks for a stop of THE STREAM THAT WAS RUNNING THEN, by its
 * generation, and then waits -- without the lock, because the teardown takes
 * it -- for that generation to be over.
 *
 * [!] THE WAIT ASKS "IS MY STREAM OVER", NEVER "IS A STREAM RUNNING".  The
 * waiter runs below the consoles that can start a new stream, so by the time
 * it looks again the stream it stopped may be gone and another one running.
 * The old wait looked at "active" and, finding the new stream, posted a stop
 * request onto it -- a stream with holders that then stopped, after which
 * every start answered "already running".  A generation cannot be mistaken
 * for the next one, and the waiter writes nothing.
 *
 * Pure, for shell/test/test_cam_gen.c: the interleavings are two consoles at
 * two priorities and cannot be typed on purpose.
 *
 * Shared since issue #143: f746g-disco's camera counts its streams the same
 * way, so the shared `camera off` can wait for the stream it stopped and not
 * mistake a later start for it.  No storage and no board header (svc/).
 */
#ifndef CAM_GEN_H
#define CAM_GEN_H

#include <stdint.h>

/** Generation 0 is never handed out: it means "no stream to wait for". */
#define CAM_GEN_NONE 0u

/** The generation after @p g, skipping CAM_GEN_NONE on the wrap. */
static inline uint32_t cam_gen_next(uint32_t g)
{
	g++;
	return (g == CAM_GEN_NONE) ? 1u : g;
}

/**
 * Has stream @p want ended, given that @p done is the last generation a
 * teardown recorded?  Streams run one at a time and in generation order, so
 * a recorded end at or after @p want covers it.  Compared as a signed
 * distance, so the wrap does not read as "long ago".  CAM_GEN_NONE: nothing
 * to wait for, so yes.
 */
static inline int cam_gen_ended(uint32_t done, uint32_t want)
{
	if (want == CAM_GEN_NONE)
		return 1;
	return (int32_t)(done - want) >= 0;
}

#endif /* CAM_GEN_H */
