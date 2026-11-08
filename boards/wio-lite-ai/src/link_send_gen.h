/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/*
 * Wio Lite AI -- the DATA channel's send generation, as pure functions (issue #218).
 *
 * link_data.c owns one 32-bit send generation.  It moves on three events:
 *
 *   - a consumer attaches          (the frames from before belong to nobody now)
 *   - detach / link_data_reset()   (the byte stream restarts)
 *   - link_data_send_fence()       (the NetX driver's gate closing, nx_link_driver.c)
 *
 * A sender reads the generation where it decides to send (the NetX driver: in the
 * same interrupt-masked step as its link_up test) and hands it to link_data_send(),
 * which compares it TWICE, each time inside a masked section: when it takes a pool
 * buffer, and again when it puts the filled buffer on the queue (the copy and the
 * CRC run unmasked between the two, so a fence can land mid-copy).  A mismatch at
 * either point drops the frame.  The effect is that a send which passed its gate and
 * was then preempted cannot reach the wire after any of the three events, whatever
 * thread it runs on and however long it stalled -- no wait and no in-flight count.
 *
 * These are the decisions only; the state, the masking and the buffer bookkeeping
 * stay in link_data.c.  They are separated so the host test can pin them on their
 * own (boards/wio-lite-ai/test/test_link_send_gen.c), and it drives link_data.c too.
 *
 * WRAP.  The generation is compared for equality, so a stale sender is accepted only
 * if the counter moved an exact multiple of 2^32 times between its read and its
 * last comparison.  That span is one driver call (gate -> copy -> enqueue), while
 * the counter moves a few times per bridge session or bench run; 2^32 moves inside
 * one stalled send cannot happen.  Nothing here relies on ordering (no "newer than").
 */
#ifndef APP_LINK_SEND_GEN_H
#define APP_LINK_SEND_GEN_H

#include <stdint.h>

enum {
	LINK_SEND_GEN_OK          = 0,
	LINK_SEND_GEN_NO_CONSUMER = -1,  /* nobody is attached: nothing to send for   */
	LINK_SEND_GEN_STALE       = -2   /* the generation moved since the sender read it */
};

/* The generation after one of the three events above. */
static inline uint32_t link_send_gen_next(uint32_t cur)
{
	return cur + 1u;                 /* wraps by definition; see WRAP above */
}

/* At buffer take: may a sender holding @mine take a buffer now? */
static inline int link_send_gen_admit(int attached, uint32_t cur, uint32_t mine)
{
	if (!attached)
		return LINK_SEND_GEN_NO_CONSUMER;
	return (cur == mine) ? LINK_SEND_GEN_OK : LINK_SEND_GEN_STALE;
}

/* At enqueue: is the frame filled under @mine still for the current generation?
 * (A detach also moves the generation, so the consumer test is not repeated.) */
static inline int link_send_gen_still(uint32_t cur, uint32_t mine)
{
	return (cur == mine) ? LINK_SEND_GEN_OK : LINK_SEND_GEN_STALE;
}

#endif /* APP_LINK_SEND_GEN_H */
