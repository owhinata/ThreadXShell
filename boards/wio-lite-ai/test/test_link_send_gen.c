/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Host test for the DATA channel's send generation (issue #218,
 * src/link_send_gen.h and src/link_data.c).
 *
 * WHY THIS EXISTS.  `nxn_stop()` relies on one fact: once the NetX driver's gate
 * closes (nx_link_driver_set_link(0) fences the generation), no frame decided
 * before that can be queued on the DATA channel -- even from a sender preempted
 * between its gate and its enqueue.  The windows that matter (a fence landing
 * just before a masked section, or during the unmasked copy) cannot be typed at
 * a console.  So this drives the real link_data.c with the HAL's PRIMASK
 * accessors replaced by a shim (link_data_shim/) that lets the test land a
 * "preemption" at the last unmasked instant before any masked section, and with
 * erpc_crc16() -- which runs unmasked between the two sections -- as the
 * mid-copy hook.
 *
 * Cases:
 *     the decisions alone (link_send_gen.h)
 *     same generation accepted
 *     read before attach / detach / reset / fence: refused, and each moves it
 *     no consumer: refused, even with the current generation
 *     a failed attach does not move it
 *     a fence just before the take section: refused there, nothing copied
 *     a fence just before the enqueue section, or during the copy: refused at
 *       the enqueue, nothing queued, the buffer given back exactly once
 *     a reset during the copy: the epoch and the generation share one exit
 *     a consumer attaching during a detach (issue #224): refused at the entry of
 *       its masked section, and there is no second section to land in front of
 *
 * [!] WHAT IT DOES NOT COVER: that every NetX send path holds nx_ip_protection,
 * and that nx_link_driver.c reads link_up and the generation in one masked step
 * (that file is NetX-bound; the diff review holds it).
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "link_data.h"
#include "link_send_gen.h"

/* ---- the shim's state and hooks ------------------------------------------ */

uint32_t ld_test_primask;
unsigned ld_test_masked_sections;

static void   (*preempt_action)(void);
static unsigned preempt_at;          /* fire at this masked entry (1-based), 0 = off */
static unsigned preempt_seen;

void ld_test_preempt_point(void)
{
	void (*a)(void);

	if (preempt_at == 0u || ++preempt_seen != preempt_at)
		return;
	a = preempt_action;
	preempt_at = 0u;                 /* one shot, and the action's own sections pass */
	a();
}

static void arm_preempt(unsigned at, void (*action)(void))
{
	preempt_action = action;
	preempt_seen   = 0u;
	preempt_at     = at;
}

static unsigned crc_calls;
static void   (*crc_action)(void);

/* Stands in for erpc.c's CRC: called by link_data_send() between its two masked
 * sections, i.e. during the copy. */
uint16_t erpc_crc16(const uint8_t *d, uint16_t n)
{
	void (*a)(void) = crc_action;

	(void)d;
	(void)n;
	crc_calls++;
	if (ld_test_primask != 0u)
		printf("  NOTE crc ran masked\n");
	crc_action = NULL;
	if (a != NULL)
		a();
	return 0x1234u;
}

static unsigned posted;
void erpc_data_posted(void)
{
	posted++;
}

/* ---- helpers --------------------------------------------------------------- */

static int fails;

#define EQ(got, want, what)                                                  \
	do {                                                                 \
		long g_ = (long)(got);                                       \
		long w_ = (long)(want);                                      \
		if (g_ != w_) {                                              \
			printf("  FAIL %s:%d %s: got %ld, wanted %ld\n",     \
			       __func__, __LINE__, what, g_, w_);            \
			fails++;                                             \
		}                                                            \
	} while (0)

static int rx_cb(void *ctx, uint8_t chan, uint8_t *p, uint16_t n)
{
	(void)ctx; (void)chan; (void)p; (void)n;
	return 0;
}

static const uint8_t frame[64];

static void act_fence(void)  { link_data_send_fence(); }
static void act_reset(void)  { link_data_reset(); }

static int send(uint32_t gen)
{
	return link_data_send(LINK_DATA_CHAN_ETH, frame, sizeof frame, gen);
}

static unsigned queued(void)
{
	struct link_data_stats s;

	link_data_stats(&s);
	return s.tx_queued;
}

static unsigned stale(void)
{
	struct link_data_stats s;

	link_data_stats(&s);
	return s.tx_stale;
}

static void drain(void)
{
	while (link_data_tx_pending())
		link_data_tx_pop();
}

/* A clean channel with one consumer attached. */
static void fresh(void)
{
	preempt_at = 0u;
	crc_action = NULL;
	link_data_detach();
	EQ(link_data_attach(rx_cb, NULL), 0, "attach");
	link_data_reset();               /* zero the counters for the case */
	EQ(ld_test_primask, 0u, "interrupts enabled between calls");
}

/* All LINK_DATA_TX_BUFS buffers are free: exactly that many sends queue and the
 * next is a pool-full drop.  A buffer given back twice would let one more in. */
static void expect_pool_whole(const char *what)
{
	uint32_t g = link_data_send_gen();
	unsigned i, ok = 0u;

	drain();
	for (i = 0u; i < LINK_DATA_TX_BUFS + 2u; i++)
		if (send(g) == 0)
			ok++;
	EQ(ok, LINK_DATA_TX_BUFS, what);
	drain();
}

/* ---- the decisions alone ---------------------------------------------------- */

static void test_pure(void)
{
	EQ(link_send_gen_admit(1, 7u, 7u), LINK_SEND_GEN_OK, "same generation");
	EQ(link_send_gen_admit(1, 8u, 7u), LINK_SEND_GEN_STALE, "moved");
	EQ(link_send_gen_admit(0, 7u, 7u), LINK_SEND_GEN_NO_CONSUMER, "no consumer");
	EQ(link_send_gen_admit(0, 8u, 7u), LINK_SEND_GEN_NO_CONSUMER, "no consumer first");
	EQ(link_send_gen_still(7u, 7u), LINK_SEND_GEN_OK, "still");
	EQ(link_send_gen_still(8u, 7u), LINK_SEND_GEN_STALE, "not still");
	EQ(link_send_gen_next(7u), 8u, "next");
	EQ(link_send_gen_next(0xFFFFFFFFu), 0u, "wraps");
	/* Equality only: a counter that moved "backwards" (wrap) is just as stale. */
	EQ(link_send_gen_still(0u, 0xFFFFFFFFu), LINK_SEND_GEN_STALE, "across the wrap");
}

/* ---- link_data.c ------------------------------------------------------------ */

static void test_same_generation(void)
{
	uint32_t g;

	fresh();
	g = link_data_send_gen();
	crc_calls = 0u;
	EQ(send(g), 0, "accepted");
	EQ(queued(), 1, "queued");
	EQ(crc_calls, 1, "copied once");
	EQ(stale(), 0, "not stale");
	EQ(ld_test_primask, 0u, "mask restored");
	drain();
}

static void test_before_attach(void)
{
	uint32_t g;

	fresh();
	link_data_detach();
	g = link_data_send_gen();
	EQ(link_data_attach(rx_cb, NULL), 0, "attach");
	EQ(link_data_send_gen() != g, 1, "attach moved the generation");
	crc_calls = 0u;
	EQ(send(g), -1, "read before attach: refused");
	EQ(queued(), 0, "nothing queued");
	EQ(crc_calls, 0, "refused before the copy");
	EQ(stale(), 1, "counted");
	/* The new owner's own read is good. */
	EQ(send(link_data_send_gen()), 0, "read after attach: accepted");
	drain();
}

static void test_failed_attach(void)
{
	uint32_t g;

	fresh();
	g = link_data_send_gen();
	EQ(link_data_attach(rx_cb, NULL), -1, "second consumer refused");
	EQ(link_data_send_gen(), g, "a refused attach does not move it");
	EQ(send(g), 0, "and the owner keeps sending");
	drain();
}

static void test_before_detach(void)
{
	uint32_t g;

	fresh();
	g = link_data_send_gen();
	link_data_detach();
	EQ(link_data_send_gen() != g, 1, "detach moved the generation");
	EQ(send(g), -1, "read before detach, sent with nobody attached");
	EQ(send(link_data_send_gen()), -1, "no consumer: refused even when current");
	EQ(link_data_attach(rx_cb, NULL), 0, "next consumer");
	EQ(send(g), -1, "read before detach, sent to the next consumer: refused");
	EQ(queued(), 0, "nothing queued");
	drain();
}

static void test_before_reset(void)
{
	uint32_t g;

	fresh();
	g = link_data_send_gen();
	link_data_reset();
	EQ(link_data_send_gen() != g, 1, "reset moved the generation");
	EQ(send(g), -1, "read before reset: refused");
	EQ(link_data_attached(), 1, "reset keeps the consumer");
	drain();
}

static void test_before_fence(void)
{
	uint32_t g;

	fresh();
	g = link_data_send_gen();
	link_data_send_fence();
	EQ(link_data_send_gen() != g, 1, "fence moved the generation");
	crc_calls = 0u;
	EQ(send(g), -1, "read before fence: refused");
	EQ(crc_calls, 0, "refused before the copy");
	EQ(queued(), 0, "nothing queued");
	EQ(stale(), 1, "counted");
	EQ(link_data_attached(), 1, "a fence is not a detach");
	EQ(send(link_data_send_gen()), 0, "read after the fence: accepted");
	drain();
}

/* The sender read its generation at its gate; the fence lands at the last unmasked
 * instant before the section that TAKES the buffer.  The comparison in that section
 * must see it: refused there, before any copy. */
static void test_fence_before_take(void)
{
	uint32_t g;

	fresh();
	g = link_data_send_gen();
	crc_calls = 0u;
	arm_preempt(1u, act_fence);
	EQ(send(g), -1, "fence before the take section: refused");
	EQ(preempt_at, 0u, "the fence did land");
	EQ(crc_calls, 0, "refused in the take section (nothing copied)");
	EQ(queued(), 0, "nothing queued");
	EQ(stale(), 1, "counted once");
	expect_pool_whole("no buffer kept");
}

/* The fence lands just before the section that QUEUES the filled buffer. */
static void test_fence_before_enqueue(void)
{
	uint32_t g;

	fresh();
	g = link_data_send_gen();
	crc_calls = 0u;
	arm_preempt(2u, act_fence);
	EQ(send(g), -1, "fence before the enqueue section: refused");
	EQ(preempt_at, 0u, "the fence did land");
	EQ(crc_calls, 1, "it had been copied");
	EQ(queued(), 0, "nothing queued");
	EQ(stale(), 1, "counted once");
	expect_pool_whole("buffer given back exactly once");
}

/* The fence lands during the copy (the CRC runs unmasked). */
static void test_fence_mid_copy(void)
{
	uint32_t g;

	fresh();
	g = link_data_send_gen();
	posted = 0u;
	crc_action = act_fence;
	EQ(send(g), -1, "fence during the copy: refused at the enqueue");
	EQ(queued(), 0, "nothing queued");
	EQ(posted, 0, "the service thread was not woken for it");
	EQ(stale(), 1, "counted once");
	expect_pool_whole("buffer given back exactly once");
}

/* A reset during the copy moves both the epoch and the generation: one exit, one
 * give-back, counted as a drop (the epoch's own counter, as before #218). */
static void test_reset_mid_copy(void)
{
	struct link_data_stats s;
	uint32_t g;

	fresh();
	g = link_data_send_gen();
	crc_action = act_reset;
	EQ(send(g), -1, "reset during the copy: refused");
	link_data_stats(&s);
	EQ(s.tx_queued, 0, "nothing queued");
	EQ(s.tx_drops, 1, "counted as a drop");
	EQ(s.tx_stale, 0, "and not twice");
	expect_pool_whole("buffer given back exactly once");
}

/* ---- a consumer attaching during a detach (issue #224) ------------------------ */

static int rx_cb2(void *ctx, uint8_t chan, uint8_t *p, uint16_t n)
{
	(void)ctx; (void)chan; (void)p; (void)n;
	return 0;
}

/* What the next owner does when it lands inside the detach: attach, read its send
 * generation, queue a frame.  Every result is recorded; none is acted on. */
static unsigned intr_ran;
static int      intr_attach_rc, intr_send_rc;
static uint32_t intr_gen;

static void act_attach_and_send(void)
{
	intr_ran++;
	intr_attach_rc = link_data_attach(rx_cb2, NULL);
	intr_gen       = link_data_send_gen();
	intr_send_rc   = send(intr_gen);
}

static void intr_clear(void)
{
	intr_ran = 0u;
	intr_attach_rc = intr_send_rc = 99;
	intr_gen = 0u;
}

/* (a) At the entry of the detach's masked section the old owner is still attached, so
 * the next owner's attach is refused; the frame it queued under the old owner's
 * generation is reclaimed by the detach, and nobody is left attached. */
static void test_attach_at_detach_entry(void)
{
	uint32_t g0;

	fresh();
	drain();
	intr_clear();
	g0 = link_data_send_gen();
	arm_preempt(1u, act_attach_and_send);
	link_data_detach();
	EQ(intr_ran, 1, "the attach did land at the entry");
	EQ(intr_attach_rc, -1, "attach at the entry: refused (old owner still there)");
	EQ(intr_gen, g0, "the generation it read was still the old owner's");
	EQ(intr_send_rc, 0, "its send went out under the old owner's generation");
	EQ(link_data_attached(), 0, "nobody attached after the detach");
	EQ(queued(), 0, "the detach reclaimed what was queued before it");
	EQ(link_data_send_gen() != g0, 1, "the detach moved the generation");
	EQ(ld_test_primask, 0u, "mask restored");
	EQ(link_data_attach(rx_cb, NULL), 0, "the next owner attaches after it");
	expect_pool_whole("no buffer kept across the detach");
}

/* (b) The detach has no second masked section: one entry, and an attach armed for a
 * second entry never runs.  Not "if it ran, then ...": a second section that the
 * attach slipped in front of is the bug, so its running at all is the failure. */
static void test_detach_one_section(void)
{
	unsigned before, after;

	fresh();
	drain();
	intr_clear();
	arm_preempt(2u, act_attach_and_send);
	before = ld_test_masked_sections;
	link_data_detach();
	after = ld_test_masked_sections;
	preempt_at = 0u;                 /* disarm before anything else masks */
	EQ(after - before, 1, "the detach is one masked section");
	EQ(intr_ran, 0, "no second entry for an attach to land at");
	EQ(link_data_attached(), 0, "the channel is left detached");
	EQ(ld_test_primask, 0u, "mask restored");
	/* The next owner, attaching after the detach, keeps its generation and frame. */
	EQ(link_data_attach(rx_cb2, NULL), 0, "attach after the detach");
	intr_gen = link_data_send_gen();
	EQ(send(intr_gen), 0, "its send is accepted");
	EQ(queued(), 1, "and stays queued");
	EQ(link_data_send_gen(), intr_gen, "and its generation stays");
	drain();
}

int main(void)
{
	test_pure();
	test_same_generation();
	test_before_attach();
	test_failed_attach();
	test_before_detach();
	test_before_reset();
	test_before_fence();
	test_fence_before_take();
	test_fence_before_enqueue();
	test_fence_mid_copy();
	test_reset_mid_copy();
	test_attach_at_detach_entry();
	test_detach_one_section();

	if (fails) {
		printf("test_link_send_gen: %d failure(s)\n", fails);
		return 1;
	}
	printf("test_link_send_gen: OK\n");
	return 0;
}
