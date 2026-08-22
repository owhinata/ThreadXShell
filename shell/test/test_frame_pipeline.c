/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Host unit test for the camera frame pipeline core (svc/frame_pipeline.c,
 * owhinata/stm32f746g-disco#47 design / owhinata/stm32f746g-disco#46).  Pure: no
 * HAL/ThreadX/shell -- a no-op frame_os and mock sinks exercise the
 * ring/refcount/policy logic on the host. Asserts:  A. init + acquire returns
 * distinct free slots; acquire excludes the latest published slot (so a pull
 * reader is not recycled under).  B. publish stamps a monotonic generation.
 *   C. DROP policy: a frame published while the sink is busy is dropped.
 *   D. LATEST policy: coalesces to the newest pending; on completion put()
 *      delivers it; a superseded pending is dropped.
 *   E. get()/put() refcount; acquire() skips a pinned (held) slot.
 *   F. detach() reports in-flight pins and stops further consume().
 *   G. read_latest() returns the latest frame's bytes + generation (tear via
 *      gen across calls).
 *   H. a counting (auto-put) sink keeps an N=4 ring cycling like the producer.
 *   I. attach REFUSES an undrained or already-linked sink instead of erasing
 *      its bookkeeping, decides before open(), and normalises open()'s
 *      rejection to one core error (issue #72).
 */
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "frame.h"
#include "frame_pipeline.h"

/* ---- no-op injected lock (single-threaded host) -------------------------- */
static int lock_depth;
static void host_lock(void *ctx)   { (void)ctx; lock_depth++; }
static void host_unlock(void *ctx) { (void)ctx; lock_depth--; }
static const struct frame_os HOST_OS = { NULL, host_lock, host_unlock };

/* ---- mock sink ----------------------------------------------------------- */
struct mock {
	struct frame_sink        sink;
	struct frame_pipeline   *p;
	int                      auto_put;  /* 1 = put inside consume (counting)   */
	int                      open_rc;   /* open() return                       */
	int                      consume_calls;
	int                      open_calls;
	const struct frame_desc *last;      /* last consumed frame (for manual put) */
};

static int mock_open(void *ctx, enum frame_format fmt, uint16_t w, uint16_t h)
{
	struct mock *m = ctx;
	(void)fmt; (void)w; (void)h;
	m->open_calls++;
	return m->open_rc;
}

static int mock_consume(void *ctx, const struct frame_desc *f)
{
	struct mock *m = ctx;
	m->consume_calls++;
	m->last = f;
	if (m->auto_put)
		frame_pipeline_put(m->p, &m->sink, f);
	return 0;
}

static void mock_init(struct mock *m, struct frame_pipeline *p,
                      uint8_t policy, int auto_put)
{
	memset(m, 0, sizeof *m);
	m->p = p;
	m->auto_put = auto_put;
	m->sink.name    = "mock";
	m->sink.ctx     = m;
	m->sink.policy  = policy;
	m->sink.open    = mock_open;
	m->sink.consume = mock_consume;
	m->sink.close   = NULL;
}

/* ---- shared backing store ------------------------------------------------ */
#define SLOT_SZ 16u
#define NSLOTS  5u
static uint8_t mem[NSLOTS * SLOT_SZ];

static void fresh(struct frame_pipeline *p, uint32_t n)
{
	assert(frame_pipeline_init(p, &HOST_OS, mem, SLOT_SZ, n) == 0);
}

static struct frame_desc *pub(struct frame_pipeline *p)
{
	struct frame_desc *d = frame_pipeline_acquire(p);
	assert(d != NULL);
	frame_pipeline_publish(p, d, SLOT_SZ, FRAME_FMT_RGB565, 4, 2, 8);
	return d;
}

/* A. distinct slots + acquire excludes latest -------------------------------*/
static void test_acquire_latest(void)
{
	struct frame_pipeline p;
	fresh(&p, 4);

	struct frame_desc *a = frame_pipeline_acquire(&p);
	struct frame_desc *b = frame_pipeline_acquire(&p);
	assert(a && b && a != b);             /* distinct free slots */

	/* publish a -> it becomes latest; a later acquire must avoid it */
	frame_pipeline_publish(&p, a, SLOT_SZ, FRAME_FMT_RGB565, 4, 2, 8);
	struct frame_desc *c = frame_pipeline_acquire(&p);
	assert(c && c != a);                  /* not the latest */
}

/* B. monotonic generation ---------------------------------------------------*/
static void test_generation(void)
{
	struct frame_pipeline p;
	fresh(&p, 4);
	struct frame_desc *a = pub(&p);
	struct frame_desc *b = pub(&p);
	assert(a->gen == 1 && b->gen == 2);
	struct frame_stats st;
	frame_pipeline_stats(&p, &st);
	assert(st.published == 2);
}

/* C. DROP policy drops while busy ------------------------------------------ */
static void test_drop(void)
{
	struct frame_pipeline p;
	struct mock m;
	fresh(&p, 4);
	mock_init(&m, &p, FRAME_POLICY_DROP, 0 /* hold */);
	assert(frame_pipeline_attach(&p, &m.sink) == 0);
	assert(m.open_calls == 1);

	struct frame_desc *d1 = frame_pipeline_acquire(&p);
	frame_pipeline_publish(&p, d1, SLOT_SZ, FRAME_FMT_RGB565, 4, 2, 8);
	assert(m.consume_calls == 1);         /* delivered, now busy (held) */

	struct frame_desc *d2 = frame_pipeline_acquire(&p);
	frame_pipeline_publish(&p, d2, SLOT_SZ, FRAME_FMT_RGB565, 4, 2, 8);
	assert(m.consume_calls == 1);         /* dropped while busy */
	assert(m.sink.dropped == 1);
	assert(m.sink.delivered == 1);

	frame_pipeline_put(&p, &m.sink, d1);  /* release -> no pending */
	assert(m.sink._busy == 0);
}

/* D. LATEST coalesce + pending transfer ------------------------------------ */
static void test_latest(void)
{
	struct frame_pipeline p;
	struct mock m;
	fresh(&p, 5);
	mock_init(&m, &p, FRAME_POLICY_LATEST, 0 /* hold */);
	assert(frame_pipeline_attach(&p, &m.sink) == 0);

	struct frame_desc *d1 = frame_pipeline_acquire(&p);
	frame_pipeline_publish(&p, d1, SLOT_SZ, FRAME_FMT_RGB565, 4, 2, 8);
	assert(m.consume_calls == 1);         /* d1 delivered, busy */

	struct frame_desc *d2 = frame_pipeline_acquire(&p);
	frame_pipeline_publish(&p, d2, SLOT_SZ, FRAME_FMT_RGB565, 4, 2, 8);
	assert(m.consume_calls == 1);         /* d2 -> pending (not delivered) */

	struct frame_desc *d3 = frame_pipeline_acquire(&p);
	frame_pipeline_publish(&p, d3, SLOT_SZ, FRAME_FMT_RGB565, 4, 2, 8);
	assert(m.consume_calls == 1);         /* d3 supersedes pending d2 */
	assert(m.sink.dropped == 1);          /* d2 dropped */

	/* completing d1 transfers the pending (d3) into the next delivery */
	frame_pipeline_put(&p, &m.sink, d1);
	assert(m.consume_calls == 2);         /* d3 now delivered */
	assert(m.last == d3);
	frame_pipeline_put(&p, &m.sink, d3);
	assert(m.sink._busy == 0 && m.sink._pins == 0);
}

/* E. get/put refcount + acquire skips a held slot -------------------------- */
static void test_refcount(void)
{
	struct frame_pipeline p;
	struct mock m;
	int i, n = 0;
	fresh(&p, 4);
	mock_init(&m, &p, FRAME_POLICY_DROP, 0 /* hold */);
	assert(frame_pipeline_attach(&p, &m.sink) == 0);

	struct frame_desc *d1 = frame_pipeline_acquire(&p);
	frame_pipeline_publish(&p, d1, SLOT_SZ, FRAME_FMT_RGB565, 4, 2, 8);
	assert(m.sink._pins == 1);            /* d1 held by the sink */

	/* d1 is latest + pinned; every other acquire must avoid it (the 3 other
	 * slots become FILLING, then acquire returns NULL -> overrun). */
	for (i = 0; i < 5; i++) {
		struct frame_desc *d = frame_pipeline_acquire(&p);
		if (d) { assert(d != d1); n++; }
	}
	assert(n == 3);

	frame_pipeline_put(&p, &m.sink, d1);  /* release the pin */
	assert(m.sink._pins == 0);
}

/* F. detach reports in-flight pins, stops consume -------------------------- */
static void test_detach(void)
{
	struct frame_pipeline p;
	struct mock m;
	fresh(&p, 4);
	mock_init(&m, &p, FRAME_POLICY_DROP, 0 /* hold */);
	assert(frame_pipeline_attach(&p, &m.sink) == 0);

	struct frame_desc *d1 = frame_pipeline_acquire(&p);
	frame_pipeline_publish(&p, d1, SLOT_SZ, FRAME_FMT_RGB565, 4, 2, 8);
	assert(m.sink._pins == 1);

	int inflight = frame_pipeline_detach(&p, &m.sink);
	assert(inflight == 1);                /* one pin still held */

	/* after detach, a publish reaches no sink */
	int before = m.consume_calls;
	struct frame_desc *d2 = frame_pipeline_acquire(&p);
	frame_pipeline_publish(&p, d2, SLOT_SZ, FRAME_FMT_RGB565, 4, 2, 8);
	assert(m.consume_calls == before);
}

/* F1. the sink count is what a producer checks before starting (issue #63) --- */
static void test_sink_count(void)
{
	struct frame_pipeline p;
	struct mock m, m2;
	fresh(&p, 4);
	mock_init(&m, &p, FRAME_POLICY_DROP, 0 /* hold */);
	mock_init(&m2, &p, FRAME_POLICY_DROP, 0 /* hold */);

	assert(frame_pipeline_sink_count(&p) == 0);
	assert(frame_pipeline_attach(&p, &m.sink) == 0);
	assert(frame_pipeline_sink_count(&p) == 1);
	assert(frame_pipeline_attach(&p, &m2.sink) == 0);
	assert(frame_pipeline_sink_count(&p) == 2);

	/*
	 * The count follows the UNLINK, not the drain: detach reports a pin still
	 * in flight and the sink is gone from the registry all the same.  That is
	 * the property the camera's ownership rule rests on -- a sink whose owner
	 * is still draining it cannot be reached by a new producer.
	 */
	struct frame_desc *d1 = frame_pipeline_acquire(&p);
	frame_pipeline_publish(&p, d1, SLOT_SZ, FRAME_FMT_RGB565, 4, 2, 8);
	assert(frame_pipeline_detach(&p, &m.sink) == 1);
	assert(frame_pipeline_sink_count(&p) == 1);

	(void)frame_pipeline_detach(&p, &m2.sink);
	assert(frame_pipeline_sink_count(&p) == 0);
}

/* F2. detach drops an undelivered LATEST pending pin ------------------------*/
static void test_detach_pending(void)
{
	struct frame_pipeline p;
	struct mock m;
	int inflight, before;
	fresh(&p, 5);
	mock_init(&m, &p, FRAME_POLICY_LATEST, 0 /* hold */);
	assert(frame_pipeline_attach(&p, &m.sink) == 0);

	struct frame_desc *d1 = frame_pipeline_acquire(&p);
	frame_pipeline_publish(&p, d1, SLOT_SZ, FRAME_FMT_RGB565, 4, 2, 8);
	struct frame_desc *d2 = frame_pipeline_acquire(&p);
	frame_pipeline_publish(&p, d2, SLOT_SZ, FRAME_FMT_RGB565, 4, 2, 8);
	assert(m.sink._pins == 2);          /* active d1 + pending d2 */

	inflight = frame_pipeline_detach(&p, &m.sink);
	assert(inflight == 1);              /* pending dropped; only the active pin */
	assert(m.sink._pending == NULL);
	assert(m.sink._pins == 1);

	/* the sink completes its active delivery; no pending re-consume happens */
	before = m.consume_calls;
	frame_pipeline_put(&p, &m.sink, d1);
	assert(m.consume_calls == before);
	assert(m.sink._pins == 0);
}

/*
 * F3. the DRAIN CHECK: the pin count of a sink the owner is tearing down
 * (issue #72)
 *
 * The number frame_pipeline_detach() returns is the one at detach time, and a
 * non-zero value there is NORMAL -- the sink may be mid-delivery.  The number
 * that says whether the owner's drain finished is this one, taken afterwards,
 * and until issue #72 there was no way to ask for it: every owner that cared
 * kept a private counter beside the core's instead.
 *
 * [!] The case below is the one no existing test covered -- the query on a
 * sink that has ALREADY been unlinked.  That is the whole point: an attached
 * sink's count is a moving target, while a detached sink's zero is a decision
 * (see the header for why get() does not break that).
 */
static void test_sink_pins_drain(void)
{
	struct frame_pipeline p;
	struct mock m;
	int inflight;
	fresh(&p, 4);
	mock_init(&m, &p, FRAME_POLICY_DROP, 0 /* hold */);
	assert(frame_pipeline_attach(&p, &m.sink) == 0);

	/* Idle: nothing held. */
	assert(frame_pipeline_sink_pins(&p, &m.sink) == 0);

	struct frame_desc *d1 = frame_pipeline_acquire(&p);
	frame_pipeline_publish(&p, d1, SLOT_SZ, FRAME_FMT_RGB565, 4, 2, 8);
	/* Mid-delivery, still attached. */
	assert(frame_pipeline_sink_pins(&p, &m.sink) == 1);

	inflight = frame_pipeline_detach(&p, &m.sink);
	assert(inflight == 1);
	/* And the same answer through the accessor once unlinked -- this is what
	   an owner asks after its drain, and here the drain has NOT happened. */
	assert(frame_pipeline_sink_pins(&p, &m.sink) == 1);

	/* The owner's thread finishes: the drain is now complete. */
	frame_pipeline_put(&p, &m.sink, d1);
	assert(frame_pipeline_sink_pins(&p, &m.sink) == 0);

	/* A publish now reaches nobody, so the count stays put. */
	struct frame_desc *d2 = frame_pipeline_acquire(&p);
	frame_pipeline_publish(&p, d2, SLOT_SZ, FRAME_FMT_RGB565, 4, 2, 8);
	assert(frame_pipeline_sink_pins(&p, &m.sink) == 0);

	/*
	 * [!] AND THE INTERLEAVING THE "ZERO IS A DECISION" PROOF TURNS ON.
	 *
	 * get() has no attachment test, so a consumer CAN take an extra pin on a
	 * sink that is already unlinked -- which is why "nothing can add a pin"
	 * would be the wrong reason to trust a zero.  The real reason is the
	 * ordering below: get() is only reachable while the consumer still holds
	 * the delivery pin it was called with, so the count passes through 2 and 1
	 * on its way down and never rises from 0.
	 */
	struct frame_desc *d3 = frame_pipeline_acquire(&p);
	assert(frame_pipeline_attach(&p, &m.sink) == 0);
	frame_pipeline_publish(&p, d3, SLOT_SZ, FRAME_FMT_RGB565, 4, 2, 8);
	assert(frame_pipeline_sink_pins(&p, &m.sink) == 1);   /* the delivery pin */

	assert(frame_pipeline_detach(&p, &m.sink) == 1);
	frame_pipeline_get(&p, &m.sink, d3);                  /* re-queue: +1 */
	assert(frame_pipeline_sink_pins(&p, &m.sink) == 2);

	frame_pipeline_put(&p, &m.sink, d3);                  /* the extra pin */
	assert(frame_pipeline_sink_pins(&p, &m.sink) == 1);
	frame_pipeline_put(&p, &m.sink, d3);                  /* the delivery pin */
	assert(frame_pipeline_sink_pins(&p, &m.sink) == 0);   /* and now it is out */

	/*
	 * Null arguments answer zero rather than dereferencing.  NOT presented as
	 * a safety property: zero is what a caller reads as permission to tear
	 * down, so this is fail-OPEN and is only acceptable because no caller can
	 * reach it -- a sink's owner passes the address of its own static sink.
	 */
	assert(frame_pipeline_sink_pins(&p, NULL) == 0);
	assert(frame_pipeline_sink_pins(NULL, &m.sink) == 0);
}

/*
 * F4. a LATEST sink's pending pin is visible to the drain check too.
 *
 * detach() drops the pending pin (nothing will ever deliver it), so the count
 * an owner sees afterwards covers only what the sink can still put() back.  A
 * check that counted the dropped pending frame would never reach zero and would
 * strand the teardown forever.
 */
static void test_sink_pins_latest(void)
{
	struct frame_pipeline p;
	struct mock m;
	fresh(&p, 5);
	mock_init(&m, &p, FRAME_POLICY_LATEST, 0 /* hold */);
	assert(frame_pipeline_attach(&p, &m.sink) == 0);

	struct frame_desc *d1 = frame_pipeline_acquire(&p);
	frame_pipeline_publish(&p, d1, SLOT_SZ, FRAME_FMT_RGB565, 4, 2, 8);
	struct frame_desc *d2 = frame_pipeline_acquire(&p);
	frame_pipeline_publish(&p, d2, SLOT_SZ, FRAME_FMT_RGB565, 4, 2, 8);
	assert(frame_pipeline_sink_pins(&p, &m.sink) == 2);  /* active + pending */

	assert(frame_pipeline_detach(&p, &m.sink) == 1);     /* pending dropped */
	assert(frame_pipeline_sink_pins(&p, &m.sink) == 1);

	frame_pipeline_put(&p, &m.sink, d1);
	assert(frame_pipeline_sink_pins(&p, &m.sink) == 0);  /* drain complete */
}

/*
 * I. attach refuses instead of erasing (issue #72).
 *
 * attach() used to zero `_pins`, `_busy` and `_pending` unconditionally.  For a
 * sink whose owner skipped its drain that erased the only evidence there was:
 * the count the owner had been told to watch went to zero while the SLOT
 * refcounts the sink still held stayed up, so the ring was one slot short from
 * then on and nothing said so.
 *
 * [!] These are the LAST line of defence, not the first.  Every board's owner
 * refuses reuse before the core is consulted, so on a healthy system none of
 * this ever runs -- which is exactly why it has to be tested here.
 */
static void test_attach_refuses_undrained(void)
{
	struct frame_pipeline p;
	struct mock m;
	int opens;

	fresh(&p, 4);
	mock_init(&m, &p, FRAME_POLICY_DROP, 0 /* hold */);
	assert(frame_pipeline_attach(&p, &m.sink) == FRAME_PIPELINE_OK);

	struct frame_desc *d1 = pub(&p);
	assert(frame_pipeline_detach(&p, &m.sink) == 1);   /* undrained on purpose */
	assert(frame_pipeline_sink_pins(&p, &m.sink) == 1);

	/* The owner ignores its own contract and re-attaches. */
	opens = m.open_calls;
	m.sink.delivered = 7;                 /* something to notice being reset */
	assert(frame_pipeline_attach(&p, &m.sink) == FRAME_PIPELINE_ERR_UNDRAINED);

	/* Refused BEFORE open(), so nothing was touched: the sink's owner was not
	   told to reset the state its consume() reads, the pin is still countable,
	   and the sink is not in the registry. */
	assert(m.open_calls == opens);
	assert(m.sink.delivered == 7);
	assert(frame_pipeline_sink_pins(&p, &m.sink) == 1);
	assert(frame_pipeline_sink_count(&p) == 0);

	/* A pin that comes back late clears the refusal by itself. */
	frame_pipeline_put(&p, &m.sink, d1);
	assert(frame_pipeline_sink_pins(&p, &m.sink) == 0);
	assert(frame_pipeline_attach(&p, &m.sink) == FRAME_PIPELINE_OK);
	assert(m.open_calls == opens + 1);
	assert(m.sink.delivered == 0);        /* and NOW the reset is right */
}

/*
 * I1b. the three ways the undrained refusal can be got WRONG, each of which the
 * test above lets through.  Written out separately because every one of them is
 * a plausible implementation, not a typo.
 */
static void test_attach_undrained_precedence(void)
{
	struct frame_pipeline p;
	struct mock held, fill[FRAME_PIPELINE_MAX_SINKS];
	unsigned i;

	/*
	 * (a) UNDRAINED must outrank FULL.  A pipeline with no room AND a sink that
	 * never handed its frame back must name the sink, not the pipeline --
	 * otherwise the one fault that is invisible everywhere else gets reported
	 * as the one that is obvious from `camera info`.
	 */
	fresh(&p, 4);
	mock_init(&held, &p, FRAME_POLICY_DROP, 0 /* hold */);
	assert(frame_pipeline_attach(&p, &held.sink) == FRAME_PIPELINE_OK);
	struct frame_desc *d1 = pub(&p);
	assert(frame_pipeline_detach(&p, &held.sink) == 1);   /* undrained */

	for (i = 0; i < FRAME_PIPELINE_MAX_SINKS; i++) {
		mock_init(&fill[i], &p, FRAME_POLICY_DROP, 1 /* auto put */);
		assert(frame_pipeline_attach(&p, &fill[i].sink) == FRAME_PIPELINE_OK);
	}
	assert(frame_pipeline_sink_count(&p) == FRAME_PIPELINE_MAX_SINKS);
	assert(frame_pipeline_attach(&p, &held.sink) == FRAME_PIPELINE_ERR_UNDRAINED);
	frame_pipeline_put(&p, &held.sink, d1);

	/*
	 * (b) It is the PIN that refuses, not the busy flag.  put() clears `_busy`
	 * on the way out, so a sink that took an extra get() and returned only its
	 * delivery pin sits at busy = 0 with a slot still held -- the exact state a
	 * `_busy`-based check would wave through, and the exact state that leaves
	 * the ring one slot short.
	 */
	fresh(&p, 4);
	mock_init(&held, &p, FRAME_POLICY_DROP, 0 /* hold */);
	assert(frame_pipeline_attach(&p, &held.sink) == FRAME_PIPELINE_OK);
	struct frame_desc *d2 = pub(&p);
	frame_pipeline_get(&p, &held.sink, d2);               /* the sink re-queues */
	frame_pipeline_put(&p, &held.sink, d2);               /* delivery pin back  */
	assert(held.sink._busy == 0);
	assert(frame_pipeline_sink_pins(&p, &held.sink) == 1);
	assert(frame_pipeline_detach(&p, &held.sink) == 1);
	assert(frame_pipeline_attach(&p, &held.sink) == FRAME_PIPELINE_ERR_UNDRAINED);
	frame_pipeline_put(&p, &held.sink, d2);

	/*
	 * (c) A NEGATIVE count is not "released".  unpin_locked() saturates at
	 * zero, so the pipeline cannot produce one -- a negative count means the
	 * sink's bookkeeping was written by something that had no business writing
	 * it, and `_pins > 0` would read that as permission to reset over a
	 * callback still inside the sink.  Reached here by writing the field
	 * directly, which is the only way it can happen at all.
	 */
	fresh(&p, 4);
	mock_init(&held, &p, FRAME_POLICY_DROP, 0);
	held.sink._pins = -1;
	assert(frame_pipeline_attach(&p, &held.sink) == FRAME_PIPELINE_ERR_UNDRAINED);
	assert(held.open_calls == 0);
}

/*
 * I1c. a refusal leaves the sink's IN-FLIGHT bookkeeping alone, not just its
 * public counters.  `_busy` and `_pending` are what a LATEST sink's owner is
 * mid-way through; clearing them on the way to a refusal would drop a frame the
 * sink still owns and lose the pin that goes with it.
 */
static void test_attach_refusal_keeps_inflight_state(void)
{
	struct frame_pipeline p;
	struct mock m;

	fresh(&p, 5);
	mock_init(&m, &p, FRAME_POLICY_LATEST, 0 /* hold */);
	assert(frame_pipeline_attach(&p, &m.sink) == FRAME_PIPELINE_OK);

	struct frame_desc *d1 = pub(&p);      /* delivered: busy + 1 pin      */
	(void)pub(&p);                        /* coalesced: pending + 1 pin   */
	assert(m.sink._busy == 1);
	assert(m.sink._pending != NULL);
	assert(frame_pipeline_sink_pins(&p, &m.sink) == 2);

	/* Detach drops the pending pin (nothing will ever deliver it), so what is
	   left is the live delivery -- and a re-attach must not touch it. */
	assert(frame_pipeline_detach(&p, &m.sink) == 1);
	assert(m.sink._busy == 1);

	assert(frame_pipeline_attach(&p, &m.sink) == FRAME_PIPELINE_ERR_UNDRAINED);
	assert(m.sink._busy == 1);            /* still mid-delivery */
	assert(m.sink._pending == NULL);      /* dropped by detach, not by us */
	assert(frame_pipeline_sink_pins(&p, &m.sink) == 1);

	frame_pipeline_put(&p, &m.sink, d1);
	assert(frame_pipeline_attach(&p, &m.sink) == FRAME_PIPELINE_OK);
	assert(m.sink._busy == 0);            /* and now the reset runs */
}

/*
 * I2. attach refuses a sink that is already linked, and says so before it says
 * "full".
 *
 * Re-linking a linked sink writes `s->_next = p->sinks` while `s` is in that
 * list -- if it is the head, `s->_next` becomes `s` and every later traversal
 * of the registry never terminates.  So the assertion that matters after the
 * refusal is not the return value: it is that the registry can still be walked
 * at all.
 */
static void test_attach_refuses_already_attached(void)
{
	struct frame_pipeline p;
	struct mock m[FRAME_PIPELINE_MAX_SINKS];
	unsigned i;

	fresh(&p, 4);
	for (i = 0; i < FRAME_PIPELINE_MAX_SINKS; i++)
		mock_init(&m[i], &p, FRAME_POLICY_DROP, 1 /* auto put */);

	assert(frame_pipeline_attach(&p, &m[0].sink) == FRAME_PIPELINE_OK);
	assert(frame_pipeline_attach(&p, &m[0].sink) == FRAME_PIPELINE_ERR_ATTACHED);
	assert(m[0].open_calls == 1);                    /* open() not re-run */
	assert(frame_pipeline_sink_count(&p) == 1);      /* the list terminates */

	/* [!] And it keeps saying "already attached" once the pipeline is full --
	   reporting FULL there would send someone looking for a free sink slot when
	   the actual fault is that this sink is in one. */
	for (i = 1; i < FRAME_PIPELINE_MAX_SINKS; i++)
		assert(frame_pipeline_attach(&p, &m[i].sink) == FRAME_PIPELINE_OK);
	assert(frame_pipeline_sink_count(&p) == FRAME_PIPELINE_MAX_SINKS);
	assert(frame_pipeline_attach(&p, &m[0].sink) == FRAME_PIPELINE_ERR_ATTACHED);

	/* A different sink into a full pipeline is the one that gets FULL, and its
	   open() is not called either. */
	struct mock extra;
	mock_init(&extra, &p, FRAME_POLICY_DROP, 1);
	assert(frame_pipeline_attach(&p, &extra.sink) == FRAME_PIPELINE_ERR_FULL);
	assert(extra.open_calls == 0);

	/* The registry still delivers to everyone it should. */
	(void)pub(&p);
	for (i = 0; i < FRAME_PIPELINE_MAX_SINKS; i++)
		assert(m[i].consume_calls == 1);
	assert(extra.consume_calls == 0);
}

/*
 * I3. a sink's own open() rejection is NORMALISED, not passed through.
 *
 * Without this the core cannot own its return values at all: a board's open()
 * is free to return any negative it likes, so every code the core picks for its
 * own refusals would be forgeable -- and telling those apart is the entire
 * reason for naming them.  An implementation that still forwards open()'s value
 * passes both refusal tests above and fails only this one.
 */
static void test_attach_open_rejection_normalised(void)
{
	struct frame_pipeline p;
	struct mock m;

	fresh(&p, 4);
	mock_init(&m, &p, FRAME_POLICY_DROP, 1 /* auto put */);
	m.open_rc = -123;                     /* a value the core never chose */
	m.sink.delivered = 5;

	assert(frame_pipeline_attach(&p, &m.sink) == FRAME_PIPELINE_ERR_OPEN);
	assert(m.open_calls == 1);            /* the sink WAS asked */
	assert(frame_pipeline_sink_count(&p) == 0);       /* and not linked */
	assert(m.sink.delivered == 5);        /* core state left alone */
	assert(frame_pipeline_sink_pins(&p, &m.sink) == 0);

	/* The rejection is the sink's to withdraw. */
	m.open_rc = 0;
	assert(frame_pipeline_attach(&p, &m.sink) == FRAME_PIPELINE_OK);
	assert(frame_pipeline_sink_count(&p) == 1);
	assert(m.sink.delivered == 0);
	(void)pub(&p);
	assert(m.consume_calls == 1);
}

/* G. read_latest bytes + generation ---------------------------------------- */
static void test_read_latest(void)
{
	struct frame_pipeline p;
	uint8_t buf[SLOT_SZ];
	uint32_t gen = 0;
	fresh(&p, 4);

	assert(frame_pipeline_read_latest(&p, 0, buf, 4, &gen) < 0); /* no frame yet */

	struct frame_desc *a = frame_pipeline_acquire(&p);
	memset(a->data, 0xAB, SLOT_SZ);
	frame_pipeline_publish(&p, a, SLOT_SZ, FRAME_FMT_RGB565, 4, 2, 8);
	assert(frame_pipeline_read_latest(&p, 0, buf, 4, &gen) == 0);
	assert(gen == 1 && buf[0] == 0xAB && buf[3] == 0xAB);

	struct frame_desc *b = frame_pipeline_acquire(&p);
	memset(b->data, 0xCD, SLOT_SZ);
	frame_pipeline_publish(&p, b, SLOT_SZ, FRAME_FMT_RGB565, 4, 2, 8);
	assert(frame_pipeline_read_latest(&p, 0, buf, 4, &gen) == 0);
	assert(gen == 2 && buf[0] == 0xCD);   /* gen change = frame replaced */

	assert(frame_pipeline_read_latest(&p, SLOT_SZ, buf, 1, &gen) < 0); /* OOB */
}

/* H. counting sink keeps an N=4 ring cycling (producer-like) ---------------- */
static void test_ring_cycle(void)
{
	struct frame_pipeline p;
	struct mock m;
	int i;
	fresh(&p, 4);
	mock_init(&m, &p, FRAME_POLICY_DROP, 1 /* auto-put (counting) */);
	assert(frame_pipeline_attach(&p, &m.sink) == 0);

	/* Mimic the producer: each frame acquired, filled, published; the counting
	 * sink consumes+puts immediately, so the ring never stalls. */
	for (i = 0; i < 100; i++) {
		struct frame_desc *d = frame_pipeline_acquire(&p);
		assert(d != NULL);                /* never NULL with N=4 + auto-put */
		frame_pipeline_publish(&p, d, SLOT_SZ, FRAME_FMT_RGB565, 4, 2, 8);
	}
	assert(m.consume_calls == 100 && m.sink.delivered == 100);
	assert(m.sink.dropped == 0 && m.sink.errors == 0);

	struct frame_stats st;
	frame_pipeline_stats(&p, &st);
	assert(st.published == 100 && st.overruns == 0);
}

int main(void)
{
	test_acquire_latest();
	test_generation();
	test_drop();
	test_latest();
	test_refcount();
	test_detach();
	test_sink_count();
	test_detach_pending();
	test_sink_pins_drain();
	test_sink_pins_latest();
	test_attach_refuses_undrained();
	test_attach_undrained_precedence();
	test_attach_refusal_keeps_inflight_state();
	test_attach_refuses_already_attached();
	test_attach_open_rejection_normalised();
	test_read_latest();
	test_ring_cycle();
	assert(lock_depth == 0);              /* every lock balanced by unlock */
	printf("test_frame_pipeline: all assertions passed\n");
	return 0;
}
