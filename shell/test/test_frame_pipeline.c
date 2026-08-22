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
static void lk_at_lock(void);
static void lk_at_unlock(void);
static void host_lock(void *ctx)   { (void)ctx; lk_at_lock();  lock_depth++; }
static void host_unlock(void *ctx) { (void)ctx; lock_depth--; lk_at_unlock(); }
static const struct frame_os HOST_OS = { NULL, host_lock, host_unlock };

/*
 * J. the registry keeps its own concurrency contract (issue #79).
 *
 * [!] NONE OF THIS IS REACHABLE ON HARDWARE.  All three boards serialise attach
 * and detach under a board lock, which is why a3cd07a could document the gap
 * instead of closing it -- so these tests are not a convenience, they are the
 * only deterministic gate the change has.
 *
 * [!] AND THE RACES ARE DRIVEN, NOT WAITED FOR.  The host lock above is a depth
 * counter, so nothing interleaves by accident.  A sink re-enters the pipeline
 * from inside its own open() / close() / consume(), which puts the second call
 * exactly where a second thread would have been -- deterministically, with no
 * threads.  In every case the assertion that matters is about the WINNER, not
 * about the inner call failing.
 *
 * [!] AND TWO MIS-IMPLEMENTATIONS THIS CANNOT SEE, said here rather than left
 * for someone to assume are covered.  Re-entry can only happen where the core
 * calls out, so anything wrong strictly BETWEEN two of the core's own
 * statements is invisible to a single thread:
 *
 *   - dropping a sink's callback count before writing its statistics rather
 *     than with them.  Both are inside one locked section, and nothing on the
 *     host can observe between them;
 *   - reading the pipeline's format at the open() call site instead of using
 *     the snapshot taken under the claim.  Nothing can change it in between
 *     here, because there is only one thread to change it with.
 *
 * Both are held by review and by the code saying why, not by this file.  The
 * test below shows only that open() is handed a CONSISTENT triple -- which is
 * the torn-read half of the problem, not the whole of it.
 *
 * Everything else about the transitions IS fixed here, and deliberately at the
 * lock boundaries rather than by the result: each approved transition writes
 * its state, its owner, its reservation, its list edges (BOTH of them), its
 * pending frame and its session reset inside one locked section, and the audit
 * records all of those at every unlock.  Checking the outcome instead would
 * pass every one of the moves above -- the outcome is the same; the window is
 * the bug.
 */

/* What a re-entrant callback should do when it runs. */
enum reenter {
	RE_NONE = 0,
	RE_ATTACH_SELF,     /* attach the same sink to the same pipeline    */
	RE_DETACH_SELF,     /* detach the same sink from the same pipeline  */
	RE_ATTACH_OTHER,    /* attach a different sink (capacity)           */
	RE_ATTACH_P2,       /* attach the same sink to a SECOND pipeline    */
	RE_SET_FORMAT,      /* change the pipeline's geometry underneath    */
};

struct mock;
static int reenter_run(struct mock *m, enum reenter what);

/* Every re-entrant callback checks this first: the core must not be holding the
   pipeline lock when it calls out.  An implementation that held it would pass
   every assertion below on the host and deadlock on a real non-recursive
   TX_MUTEX -- the depth counter cannot see the difference by itself. */
#define ASSERT_UNLOCKED()  assert(lock_depth == 0)

/* ---- lock-boundary audit for one attach (issue #79) ---------------------- */
/*
 * Claiming OUTSIDE the lock passes every re-entrancy test, because the callback
 * re-enters after the claim either way -- while two real threads would slip
 * between the check and the claim.  So the boundaries are audited directly:
 * nothing claimed when the first lock is taken, everything claimed when it is
 * released.  Auditing only the release would still let the claim move BEFORE
 * the lock.
 */
#define LK_MAX 8
static struct {
	int on, fail, locks, unlocks;
	struct frame_pipeline *p;
	struct frame_sink     *s;
	uint8_t  state[LK_MAX];      /* the sink's state at each unlock  */
	unsigned reserved[LK_MAX];   /* and the pipeline's reservation   */
	int      linked[LK_MAX];     /* and whether it is in the list    */
	int      other_linked[LK_MAX];  /* and whether a NEIGHBOUR still is */
	struct frame_sink *other;    /* the neighbour to watch, or NULL   */
	int      callbacks[LK_MAX];  /* and its in-flight callback count */
	const void *owner[LK_MAX];   /* and who owns it                  */
	int      clean[LK_MAX];      /* and whether its session was reset */
	const void *head[LK_MAX];    /* the list head                     */
	const void *next[LK_MAX];    /* and the sink's own edge out of it  */
	const void *pending[LK_MAX]; /* and its coalesced frame            */
	int      pins[LK_MAX];
	int      entry_state, entry_reserved;
	const void *entry_owner;
} lk;

static int lk_reaches(const struct frame_sink *want)
{
	struct frame_sink *it;
	int n = 0;

	if (want == NULL)
		return 0;
	for (it = lk.p->sinks; it != NULL && n <= LK_MAX; it = it->_next, n++)
		if (it == want)
			return 1;
	return 0;
}

static int lk_is_linked(void) { return lk_reaches(lk.s); }

static void lk_at_lock(void)
{
	if (!lk.on)
		return;
	if (lk.locks++ == 0) {
		lk.entry_state    = lk.s->_state;
		lk.entry_reserved = (int)lk.p->_reserved;
		lk.entry_owner    = lk.s->_owner;
	}
}

/* Every unlock is recorded, not just the first.  Auditing only the claim would
   leave the COMMIT free to move outside the lock -- the callback re-enters
   before it either way, so no re-entrancy test can see the difference. */
static void lk_at_unlock(void)
{
	if (!lk.on)
		return;
	if (lk.unlocks < LK_MAX) {
		lk.state[lk.unlocks]     = lk.s->_state;
		lk.reserved[lk.unlocks]  = lk.p->_reserved;
		lk.linked[lk.unlocks]    = lk_is_linked();
		lk.other_linked[lk.unlocks] = lk_reaches(lk.other);
		lk.callbacks[lk.unlocks] = lk.s->_callbacks;
		lk.owner[lk.unlocks]     = lk.s->_owner;
		lk.head[lk.unlocks]      = lk.p->sinks;
		lk.next[lk.unlocks]      = lk.s->_next;
		lk.pending[lk.unlocks]   = lk.s->_pending;
		lk.pins[lk.unlocks]      = lk.s->_pins;
		lk.clean[lk.unlocks]     = (lk.s->_busy == 0 &&
		                            lk.s->_pending == NULL &&
		                            lk.s->delivered == 0 &&
		                            lk.s->dropped == 0 &&
		                            lk.s->errors == 0);
	}
	lk.unlocks++;
}

/* ---- mock sink ----------------------------------------------------------- */
struct mock {
	struct frame_sink        sink;
	struct frame_pipeline   *p;
	int                      auto_put;  /* 1 = put inside consume (counting)   */
	int                      open_rc;   /* open() return                       */
	int                      consume_calls;
	int                      open_calls;
	int                      close_calls;
	const struct frame_desc *last;      /* last consumed frame (for manual put) */

	/* Re-entrancy (issue #79): what each callback should do when it runs, and
	   what the pipeline answered it.  `re_on_consume` fires on the Nth consume
	   so the LATEST hand-off can be targeted separately from the delivery. */
	enum reenter             re_open, re_close, re_consume;
	int                      re_on_consume;
	int                      re_put_first;   /* put before re-entering        */
	int                      re_detach_first;/* detach before re-entering     */
	struct frame_pipeline   *re_p2;          /* the second pipeline           */
	struct mock             *re_other;       /* the other sink                */
	int                      re_rc;          /* what the inner call answered  */
	int                      re_detach_rc;
	int                      pins_in_cb;    /* sink_pins() from inside consume */
	int                      detach_in_cb;  /* a second detach from inside it  */
};

static enum frame_format mock_last_fmt;
static uint16_t          mock_last_w, mock_last_h;

static int mock_open(void *ctx, enum frame_format fmt, uint16_t w, uint16_t h)
{
	struct mock *m = ctx;

	ASSERT_UNLOCKED();
	mock_last_fmt = fmt;
	mock_last_w = w;
	mock_last_h = h;
	m->open_calls++;
	if (m->re_open != RE_NONE)
		m->re_rc = reenter_run(m, m->re_open);
	return m->open_rc;
}

static void mock_close(void *ctx)
{
	struct mock *m = ctx;

	ASSERT_UNLOCKED();
	m->close_calls++;
	if (m->re_close != RE_NONE)
		m->re_rc = reenter_run(m, m->re_close);
}

static int mock_consume(void *ctx, const struct frame_desc *f)
{
	struct mock *m = ctx;

	ASSERT_UNLOCKED();
	m->consume_calls++;
	m->last = f;
	if (m->re_consume != RE_NONE && m->consume_calls == m->re_on_consume) {
		if (m->re_detach_first)
			m->re_detach_rc = frame_pipeline_detach(m->p, &m->sink);
		if (m->re_put_first)
			frame_pipeline_put(m->p, &m->sink, f);
		/* Taken here on purpose: pins are back but this callback has not
		   returned, and no other moment in the test can see that. */
		m->pins_in_cb   = frame_pipeline_sink_pins(m->p, &m->sink);
		m->detach_in_cb = frame_pipeline_detach(m->p, &m->sink);
		m->re_rc = reenter_run(m, m->re_consume);
		return 0;
	}
	if (m->auto_put)
		frame_pipeline_put(m->p, &m->sink, f);
	return 0;
}

/* The inner call.  Deliberately the ONLY place a test re-enters the pipeline,
   so every case says which door it came back through. */
static int reenter_run(struct mock *m, enum reenter what)
{
	switch (what) {
	case RE_ATTACH_SELF:  return frame_pipeline_attach(m->p, &m->sink);
	case RE_DETACH_SELF:  return frame_pipeline_detach(m->p, &m->sink);
	case RE_ATTACH_OTHER: return frame_pipeline_attach(m->p, &m->re_other->sink);
	case RE_ATTACH_P2:    return frame_pipeline_attach(m->re_p2, &m->sink);
	case RE_SET_FORMAT:
		frame_pipeline_set_format(m->p, FRAME_FMT_JPEG, 99, 77);
		return 0;
	case RE_NONE:         break;
	}
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
	m->sink.close   = mock_close;
}

/* ---- shared backing store ------------------------------------------------ */
#define SLOT_SZ 16u
#define NSLOTS  5u
static uint8_t mem[NSLOTS * SLOT_SZ];
static uint8_t mem2[NSLOTS * SLOT_SZ];   /* a SECOND pipeline (issue #79) */

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

/* J1. re-entry from open(): the winner is what matters, not the refusal. */
static void test_reenter_from_open(void)
{
	struct frame_pipeline p;
	struct mock m, other;
	unsigned i;

	/* (a) the same sink, attached again from inside its own open(). */
	fresh(&p, 4);
	mock_init(&m, &p, FRAME_POLICY_DROP, 1 /* auto put */);
	m.re_open = RE_ATTACH_SELF;
	assert(frame_pipeline_attach(&p, &m.sink) == FRAME_PIPELINE_OK);
	assert(m.re_rc == FRAME_PIPELINE_ERR_TRANSITION);
	assert(frame_pipeline_sink_count(&p) == 1);   /* linked exactly once   */
	assert(m.close_calls == 0);                   /* nobody closed it      */
	(void)pub(&p);
	assert(m.consume_calls == 1);                 /* and it still works    */

	/* (b) detached from inside its own open().  The detach must find nothing
	   to unlink and say so -- answering a bare zero would tell its caller the
	   sink was out while open() was still running on it. */
	fresh(&p, 4);
	mock_init(&m, &p, FRAME_POLICY_DROP, 1);
	m.re_open = RE_DETACH_SELF;
	assert(frame_pipeline_attach(&p, &m.sink) == FRAME_PIPELINE_OK);
	assert(m.re_rc == FRAME_PIPELINE_ERR_TRANSITION);
	assert(m.close_calls == 0);
	assert(frame_pipeline_sink_count(&p) == 1);
	(void)pub(&p);
	assert(m.consume_calls == 1);

	/* (c) a DIFFERENT sink taking the last free slot from inside open().  It
	   must not get it: the outer attach reserved it before releasing the lock,
	   which is the whole point of counting reservations. */
	fresh(&p, 4);
	mock_init(&other, &p, FRAME_POLICY_DROP, 1);
	for (i = 0; i + 1 < FRAME_PIPELINE_MAX_SINKS; i++) {
		static struct mock fill[FRAME_PIPELINE_MAX_SINKS];

		mock_init(&fill[i], &p, FRAME_POLICY_DROP, 1);
		assert(frame_pipeline_attach(&p, &fill[i].sink) == FRAME_PIPELINE_OK);
	}
	mock_init(&m, &p, FRAME_POLICY_DROP, 1);
	m.re_open = RE_ATTACH_OTHER;
	m.re_other = &other;
	assert(frame_pipeline_attach(&p, &m.sink) == FRAME_PIPELINE_OK);
	assert(m.re_rc == FRAME_PIPELINE_ERR_FULL);
	assert(other.open_calls == 0);
	assert(frame_pipeline_sink_count(&p) == FRAME_PIPELINE_MAX_SINKS);
}

/*
 * J2. re-entry from close(), which is the window an unfinished detach leaves.
 *
 * [!] This is the one that catches a transition never cleared.  If detach
 * forgot to leave DETACHING behind, the attach after it would be refused
 * forever and the sink would be wedged with nothing to un-wedge it.
 */
static void test_reenter_from_close(void)
{
	struct frame_pipeline p;
	struct mock m;

	/* (a) attach the same sink from inside its own close(). */
	fresh(&p, 4);
	mock_init(&m, &p, FRAME_POLICY_DROP, 1);
	assert(frame_pipeline_attach(&p, &m.sink) == FRAME_PIPELINE_OK);
	m.re_close = RE_ATTACH_SELF;
	assert(frame_pipeline_detach(&p, &m.sink) == 0);
	assert(m.re_rc == FRAME_PIPELINE_ERR_TRANSITION);
	assert(m.close_calls == 1);
	m.re_close = RE_NONE;
	/* And once the detach has returned, the sink is free again. */
	assert(frame_pipeline_attach(&p, &m.sink) == FRAME_PIPELINE_OK);
	(void)pub(&p);
	assert(m.consume_calls == 1);

	/* (b) detach it again from inside its own close(): once, not twice. */
	fresh(&p, 4);
	mock_init(&m, &p, FRAME_POLICY_DROP, 1);
	assert(frame_pipeline_attach(&p, &m.sink) == FRAME_PIPELINE_OK);
	m.re_close = RE_DETACH_SELF;
	assert(frame_pipeline_detach(&p, &m.sink) == 0);
	assert(m.re_rc == FRAME_PIPELINE_ERR_TRANSITION);
	assert(m.close_calls == 1);
}

/*
 * J3. ownership outlives the detach, and then outlives the pin.
 *
 * A detached sink can still hold a delivery pin, and the put that returns it
 * arrives on the OLD pipeline -- so handing the sink to a second pipeline at
 * close() would let that put move `_pins` under a different mutex.
 */
static void test_owner_across_drain(void)
{
	struct frame_pipeline p1, p2;
	struct mock m;
	struct frame_desc *d;

	fresh(&p1, 4);
	assert(frame_pipeline_init(&p2, &HOST_OS, mem2, SLOT_SZ, 4) == 0);
	mock_init(&m, &p1, FRAME_POLICY_DROP, 0 /* hold the pin */);

	assert(frame_pipeline_attach(&p1, &m.sink) == FRAME_PIPELINE_OK);
	d = pub(&p1);
	assert(frame_pipeline_detach(&p1, &m.sink) == 1);   /* undrained */

	/* p2 may not have it while p1's pin is still out. */
	assert(frame_pipeline_attach(&p2, &m.sink) == FRAME_PIPELINE_ERR_OWNER);

	frame_pipeline_put(&p1, &m.sink, d);
	assert(frame_pipeline_attach(&p2, &m.sink) == FRAME_PIPELINE_OK);
	assert(frame_pipeline_sink_count(&p1) == 0);
	assert(frame_pipeline_sink_count(&p2) == 1);
	m.p = &p2;
	m.auto_put = 1;
	(void)pub(&p2);
	assert(m.consume_calls == 2);                       /* delivered by p2 */
}

/*
 * J4. [!] AND THE PIN IS NOT THE LAST THING THE CORE OWES.
 *
 * publish() calls consume() outside the lock and re-takes it afterwards to
 * write the sink's statistics.  So a sink can reach zero pins from inside its
 * own callback while the core still has a write to make -- and releasing
 * ownership there lets that write land on the next pipeline's session.
 *
 * Driven from inside consume(): detach, put, then try the other pipeline
 * BEFORE returning.
 */
static void test_owner_across_callback(void)
{
	struct frame_pipeline p1, p2;
	struct mock m;

	fresh(&p1, 4);
	assert(frame_pipeline_init(&p2, &HOST_OS, mem2, SLOT_SZ, 4) == 0);
	mock_init(&m, &p1, FRAME_POLICY_DROP, 0);
	m.re_consume     = RE_ATTACH_P2;
	m.re_on_consume  = 1;
	m.re_detach_first = 1;
	m.re_put_first   = 1;
	m.re_p2          = &p2;

	assert(frame_pipeline_attach(&p1, &m.sink) == FRAME_PIPELINE_OK);
	(void)pub(&p1);

	assert(m.re_detach_rc == 1);                  /* the pin was still out  */
	/* Refused: pins were zero by then, but the epilogue below had not run. */
	assert(m.re_rc == FRAME_PIPELINE_ERR_OWNER);
	/* And the two answers taken from inside that window, which is the only
	   place they can be: zero pins with a callback still out must NOT read as
	   "nothing outstanding" -- zero is what a port frees on. */
	assert(m.pins_in_cb == FRAME_PIPELINE_ERR_QUIESCE);
	assert(m.detach_in_cb == FRAME_PIPELINE_ERR_QUIESCE);

	/* Now the epilogue has run, and only now is the sink free. */
	assert(frame_pipeline_attach(&p2, &m.sink) == FRAME_PIPELINE_OK);
	assert(m.sink.delivered == 0);                /* p1's write did not land */
	m.p = &p2;
	m.re_consume = RE_NONE;
	m.auto_put = 1;
	(void)pub(&p2);
	assert(m.sink.delivered == 1);
}

/* J5. the same, through put()'s LATEST hand-off, which has its own epilogue. */
static void test_owner_across_callback_latest(void)
{
	struct frame_pipeline p1, p2;
	struct mock m;
	struct frame_desc *d1;

	fresh(&p1, 5);
	assert(frame_pipeline_init(&p2, &HOST_OS, mem2, SLOT_SZ, 4) == 0);
	mock_init(&m, &p1, FRAME_POLICY_LATEST, 0);
	m.re_consume     = RE_ATTACH_P2;
	m.re_on_consume  = 2;          /* the hand-off, not the first delivery */
	m.re_detach_first = 1;
	m.re_put_first   = 1;
	m.re_p2          = &p2;

	assert(frame_pipeline_attach(&p1, &m.sink) == FRAME_PIPELINE_OK);
	d1 = pub(&p1);                 /* delivered: consume #1 holds it       */
	(void)pub(&p1);                /* coalesced into pending               */
	frame_pipeline_put(&p1, &m.sink, d1);   /* hands off -> consume #2     */

	assert(m.consume_calls == 2);
	assert(m.re_rc == FRAME_PIPELINE_ERR_OWNER);
	assert(frame_pipeline_attach(&p2, &m.sink) == FRAME_PIPELINE_OK);
}

/* J6. the three detaches that must do nothing at all. */
static void test_detach_no_ops(void)
{
	struct frame_pipeline p1, p2;
	struct mock m;
	struct frame_desc *d;

	/* (a) never attached: the current count, and not a word to the sink. */
	fresh(&p1, 4);
	mock_init(&m, &p1, FRAME_POLICY_DROP, 1);
	assert(frame_pipeline_detach(&p1, &m.sink) == 0);
	assert(m.close_calls == 0);
	assert(m.sink._state == FRAME_SINK_UNOWNED);
	assert(m.sink._owner == NULL);
	assert(m.sink._next == NULL);

	/* (b) the wrong pipeline: owned by p1, asked of p2. */
	assert(frame_pipeline_init(&p2, &HOST_OS, mem2, SLOT_SZ, 4) == 0);
	assert(frame_pipeline_attach(&p1, &m.sink) == FRAME_PIPELINE_OK);
	assert(frame_pipeline_detach(&p2, &m.sink) == FRAME_PIPELINE_ERR_OWNER);
	assert(m.close_calls == 0);
	assert(frame_pipeline_sink_count(&p1) == 1);   /* still p1's */
	(void)pub(&p1);
	assert(m.consume_calls == 1);                  /* and still fed by p1 */

	/* (c) a second detach while draining -- never a bare zero when a callback
	   is out, because a pin count cannot say "a callback still has your ctx". */
	fresh(&p1, 4);
	mock_init(&m, &p1, FRAME_POLICY_DROP, 0 /* hold */);
	assert(frame_pipeline_attach(&p1, &m.sink) == FRAME_PIPELINE_OK);
	d = pub(&p1);
	assert(frame_pipeline_detach(&p1, &m.sink) == 1);
	assert(m.close_calls == 1);
	assert(frame_pipeline_detach(&p1, &m.sink) == 1);   /* pins out, no cb  */
	assert(m.close_calls == 1);                         /* not closed twice */
	frame_pipeline_put(&p1, &m.sink, d);
}

/*
 * J7. the state, the owner, membership and the counters must agree -- and all
 * three entry points must say the same KIND of thing when they do not.
 *
 * [!] The classification is what a port acts on: it retries a transition and
 * gives up on corruption.  One function calling a corrupted sink retryable
 * while another calls it terminal is a port spinning on it forever.
 */
static void test_inconsistent_state(void)
{
	struct frame_pipeline p;
	struct mock m;

	/* ATTACHING while linked: linked is not legal for a transition, so this is
	   corruption wearing the mask of "already attached". */
	fresh(&p, 4);
	mock_init(&m, &p, FRAME_POLICY_DROP, 1);
	assert(frame_pipeline_attach(&p, &m.sink) == FRAME_PIPELINE_OK);
	m.sink._state = FRAME_SINK_ATTACHING;
	assert(frame_pipeline_attach(&p, &m.sink) == FRAME_PIPELINE_ERR_STATE);
	assert(frame_pipeline_detach(&p, &m.sink) == FRAME_PIPELINE_ERR_STATE);
	assert(frame_pipeline_sink_pins(&p, &m.sink) == FRAME_PIPELINE_ERR_STATE);
	assert(m.close_calls == 0);

	/* UNOWNED while linked: attach keeps a3cd07a's membership-first answer,
	   the other two call it what it is.  Either way nothing is unlinked. */
	m.sink._state = FRAME_SINK_UNOWNED;
	m.sink._owner = NULL;
	assert(frame_pipeline_attach(&p, &m.sink) == FRAME_PIPELINE_ERR_ATTACHED);
	assert(frame_pipeline_detach(&p, &m.sink) == FRAME_PIPELINE_ERR_STATE);
	assert(frame_pipeline_sink_pins(&p, &m.sink) == FRAME_PIPELINE_ERR_STATE);
	assert(m.close_calls == 0);
	assert(frame_pipeline_sink_count(&p) == 1);

	/* ATTACHED with no owner: the row that looks redundant and is not -- a
	   summary of the model would let this walk into the destructive detach and
	   call a board's close() on a sink whose ownership is already corrupt. */
	fresh(&p, 4);
	mock_init(&m, &p, FRAME_POLICY_DROP, 1);
	assert(frame_pipeline_attach(&p, &m.sink) == FRAME_PIPELINE_OK);
	m.sink._owner = NULL;
	assert(frame_pipeline_detach(&p, &m.sink) == FRAME_PIPELINE_ERR_STATE);
	assert(frame_pipeline_sink_pins(&p, &m.sink) == FRAME_PIPELINE_ERR_STATE);
	assert(m.close_calls == 0);
	assert(frame_pipeline_sink_count(&p) == 1);   /* not unlinked */

	/* A negative PIN count on a LINKED sink: refused before anything
	   destructive.  Unlink first and report the error afterwards, and the port
	   reads "the detach did not happen" while the core has already unlinked --
	   a disagreement worse than either fact on its own. */
	fresh(&p, 4);
	mock_init(&m, &p, FRAME_POLICY_DROP, 1);
	assert(frame_pipeline_attach(&p, &m.sink) == FRAME_PIPELINE_OK);
	m.sink._pins = -1;
	assert(frame_pipeline_detach(&p, &m.sink) == FRAME_PIPELINE_ERR_STATE);
	assert(m.close_calls == 0);
	assert(frame_pipeline_sink_count(&p) == 1);   /* still linked */
	m.sink._pins = 0;

	/* The same on a LINKED sink, which the unowned row above cannot speak for:
	   a negative callback count is corruption in every state. */
	fresh(&p, 4);
	mock_init(&m, &p, FRAME_POLICY_DROP, 1);
	assert(frame_pipeline_attach(&p, &m.sink) == FRAME_PIPELINE_OK);
	m.sink._callbacks = -1;
	/* attach answers its own refusal first -- the sink IS in the registry, and
	   "already attached" is the true thing to say about it.  Terminal either
	   way, which is the part that has to match. */
	assert(frame_pipeline_attach(&p, &m.sink) == FRAME_PIPELINE_ERR_ATTACHED);
	assert(frame_pipeline_detach(&p, &m.sink) == FRAME_PIPELINE_ERR_STATE);
	assert(frame_pipeline_sink_pins(&p, &m.sink) == FRAME_PIPELINE_ERR_STATE);
	assert(m.close_calls == 0);
	m.sink._callbacks = 0;

	/* [!] And a negative PIN on an unowned sink, through DETACH.  This is the
	   one place the count is returned rather than classified, so passing it
	   through would hand the caller a -1 that its error test reads as a code.
	   attach still answers UNDRAINED here (a3cd07a, and a landed test): the
	   NAMES differ, which is allowed -- both are terminal, and terminal versus
	   retryable is all a port acts on. */
	fresh(&p, 4);
	mock_init(&m, &p, FRAME_POLICY_DROP, 1);
	m.sink._pins = -1;
	assert(frame_pipeline_detach(&p, &m.sink) == FRAME_PIPELINE_ERR_STATE);
	assert(frame_pipeline_sink_pins(&p, &m.sink) == FRAME_PIPELINE_ERR_STATE);
	assert(frame_pipeline_attach(&p, &m.sink) == FRAME_PIPELINE_ERR_UNDRAINED);
	m.sink._pins = 0;

	/* A negative callback count is the bookkeeping itself being untrustworthy,
	   so it is terminal from all three -- NOT the retryable "not quiescent",
	   which would have a port retry a corruption for ever. */
	fresh(&p, 4);
	mock_init(&m, &p, FRAME_POLICY_DROP, 1);
	m.sink._callbacks = -1;
	assert(frame_pipeline_attach(&p, &m.sink) == FRAME_PIPELINE_ERR_STATE);
	assert(frame_pipeline_detach(&p, &m.sink) == FRAME_PIPELINE_ERR_STATE);
	assert(frame_pipeline_sink_pins(&p, &m.sink) == FRAME_PIPELINE_ERR_STATE);
	assert(m.open_calls == 0);

	/* [!] UNOWNED with a callback out.  The count is the core's own, so a sink
	   nobody owns cannot legitimately have one -- and letting it through had
	   detach answer a BARE ZERO while a callback still held the caller's ctx,
	   which is the single thing zero must never mean here. */
	fresh(&p, 4);
	mock_init(&m, &p, FRAME_POLICY_DROP, 1);
	m.sink._callbacks = 1;
	assert(frame_pipeline_attach(&p, &m.sink) == FRAME_PIPELINE_ERR_STATE);
	assert(frame_pipeline_detach(&p, &m.sink) == FRAME_PIPELINE_ERR_STATE);
	assert(frame_pipeline_sink_pins(&p, &m.sink) == FRAME_PIPELINE_ERR_STATE);
	m.sink._callbacks = 0;

	/* ATTACHING without the capacity its claim took: the reservation is part of
	   the model, so a sink mid-attach that nothing reserved for is corrupt --
	   not a transition to wait out, which would retry for ever. */
	fresh(&p, 4);
	mock_init(&m, &p, FRAME_POLICY_DROP, 1);
	m.sink._state = FRAME_SINK_ATTACHING;
	m.sink._owner = &p;
	assert(p._reserved == 0u);
	assert(frame_pipeline_attach(&p, &m.sink) == FRAME_PIPELINE_ERR_STATE);
	assert(frame_pipeline_detach(&p, &m.sink) == FRAME_PIPELINE_ERR_STATE);
	assert(frame_pipeline_sink_pins(&p, &m.sink) == FRAME_PIPELINE_ERR_STATE);

	/* A reservation past the maximum is not a full pipeline, it is a broken
	   one, and every entry point has to say so rather than "full". */
	fresh(&p, 4);
	mock_init(&m, &p, FRAME_POLICY_DROP, 1);
	p._reserved = FRAME_PIPELINE_MAX_SINKS + 1u;
	assert(frame_pipeline_attach(&p, &m.sink) == FRAME_PIPELINE_ERR_STATE);
	assert(frame_pipeline_detach(&p, &m.sink) == FRAME_PIPELINE_ERR_STATE);
	assert(frame_pipeline_sink_pins(&p, &m.sink) == FRAME_PIPELINE_ERR_STATE);
	p._reserved = 0u;

	/* links + reserved past the maximum: not a full pipeline, a broken one.
	   Letting it through had attach answer "full" while detach and sink_pins
	   carried on as though nothing were wrong -- the same corruption split
	   across terminal and normal. */
	fresh(&p, 4);
	mock_init(&m, &p, FRAME_POLICY_DROP, 1);
	assert(frame_pipeline_attach(&p, &m.sink) == FRAME_PIPELINE_OK);
	p._reserved = FRAME_PIPELINE_MAX_SINKS;
	assert(frame_pipeline_detach(&p, &m.sink) == FRAME_PIPELINE_ERR_STATE);
	assert(frame_pipeline_sink_pins(&p, &m.sink) == FRAME_PIPELINE_ERR_STATE);
	assert(m.close_calls == 0);
	p._reserved = 0u;

	/* ATTACHING with a counter up.  Both are zero there by construction -- the
	   claim was granted only after attach found them so, and nothing can raise
	   them while the sink is out of the registry -- so a positive one is
	   corruption, not a transition to wait out. */
	fresh(&p, 4);
	mock_init(&m, &p, FRAME_POLICY_DROP, 1);
	m.sink._state = FRAME_SINK_ATTACHING;
	m.sink._owner = &p;
	p._reserved = 1u;
	m.sink._pins = 1;
	assert(frame_pipeline_attach(&p, &m.sink) == FRAME_PIPELINE_ERR_STATE);
	assert(frame_pipeline_detach(&p, &m.sink) == FRAME_PIPELINE_ERR_STATE);
	assert(frame_pipeline_sink_pins(&p, &m.sink) == FRAME_PIPELINE_ERR_STATE);
	m.sink._pins = 0;
	m.sink._callbacks = 1;
	assert(frame_pipeline_attach(&p, &m.sink) == FRAME_PIPELINE_ERR_STATE);
	assert(frame_pipeline_detach(&p, &m.sink) == FRAME_PIPELINE_ERR_STATE);
	assert(frame_pipeline_sink_pins(&p, &m.sink) == FRAME_PIPELINE_ERR_STATE);
	m.sink._callbacks = 0;
	p._reserved = 0u;

	/* A negative pin count OUTSIDE the unowned case is corruption, and all
	   three must agree -- one of them calling it a transition (retryable) while
	   another called it corrupt is a port retrying a broken sink for ever. */
	fresh(&p, 4);
	mock_init(&m, &p, FRAME_POLICY_DROP, 1);
	m.sink._state = FRAME_SINK_DETACHING;
	m.sink._owner = &p;
	m.sink._pins  = -1;
	assert(frame_pipeline_attach(&p, &m.sink) == FRAME_PIPELINE_ERR_STATE);
	assert(frame_pipeline_detach(&p, &m.sink) == FRAME_PIPELINE_ERR_STATE);
	assert(frame_pipeline_sink_pins(&p, &m.sink) == FRAME_PIPELINE_ERR_STATE);
	m.sink._pins = 0;
	m.sink._state = FRAME_SINK_UNOWNED;
	m.sink._owner = NULL;

	/* A quiescent DRAINING cannot be reached: the section that makes a sink
	   quiescent moves it to UNOWNED in the same breath.  The innocent reading
	   is the dangerous one -- sink_pins() would answer a bare zero, and zero is
	   what a port reads as permission to tear down. */
	fresh(&p, 4);
	mock_init(&m, &p, FRAME_POLICY_DROP, 1);
	m.sink._state = FRAME_SINK_DRAINING;
	m.sink._owner = &p;
	assert(frame_pipeline_attach(&p, &m.sink) == FRAME_PIPELINE_ERR_STATE);
	assert(frame_pipeline_detach(&p, &m.sink) == FRAME_PIPELINE_ERR_STATE);
	assert(frame_pipeline_sink_pins(&p, &m.sink) == FRAME_PIPELINE_ERR_STATE);
}

/* J8. a damaged registry terminates with an answer instead of hanging. */
static void test_damaged_registry(void)
{
	struct frame_pipeline p;
	struct mock m, m2;

	fresh(&p, 4);
	mock_init(&m, &p, FRAME_POLICY_DROP, 1);
	mock_init(&m2, &p, FRAME_POLICY_DROP, 1);
	assert(frame_pipeline_attach(&p, &m.sink) == FRAME_PIPELINE_OK);
	assert(frame_pipeline_attach(&p, &m2.sink) == FRAME_PIPELINE_OK);

	/* The self-loop this issue's sibling exists to prevent.  Asking "linked
	   exactly once" of it is exactly how a walk would never come back. */
	m2.sink._next = &m2.sink;
	assert(frame_pipeline_attach(&p, &m.sink) == FRAME_PIPELINE_ERR_STATE);
	assert(frame_pipeline_detach(&p, &m.sink) == FRAME_PIPELINE_ERR_STATE);
	assert(frame_pipeline_sink_pins(&p, &m.sink) == FRAME_PIPELINE_ERR_STATE);

	/* [!] And the walks that are NOT the registry's own have to terminate on
	   the same list, or the gate is decorative: bounding only the entry points
	   leaves the producer to hang on the very list they refused. */
	assert(frame_pipeline_sink_count(&p) > 0);
	(void)pub(&p);
	m2.sink._next = NULL;                          /* leave it walkable */
}

/* J9. open() is told the geometry the claim agreed to, not a torn read. */
static void test_format_snapshot(void)
{
	struct frame_pipeline p;
	struct mock m;

	fresh(&p, 4);
	frame_pipeline_set_format(&p, FRAME_FMT_RGB565, 4, 2);
	mock_init(&m, &p, FRAME_POLICY_DROP, 1);
	m.re_open = RE_SET_FORMAT;
	assert(frame_pipeline_attach(&p, &m.sink) == FRAME_PIPELINE_OK);
	/* The set_format ran from inside open(); what open() saw must be what the
	   claim held, never a mixture of the two. */
	assert(mock_last_fmt == FRAME_FMT_RGB565);
	assert(mock_last_w == 4 && mock_last_h == 2);
}

/*
 * J10. every state change happens inside a locked section, checked at the
 * boundaries rather than inferred from the result.
 *
 * [!] BOTH EDGES OF THE LINK, not just "is it in the list".  Setting the head
 * under the lock and the sink's own `_next` after it leaves a publish walking
 * from a head whose successor is stale -- and a test that only asks whether the
 * target is reachable cannot tell, because it is.
 *
 * [!] This is the only thing that can see it.  An implementation that claims
 * outside the lock, or commits outside it, passes every re-entrancy test above
 * -- the callback re-enters after the claim and before the commit either way --
 * while two real threads would slip straight into the gap.
 */
static void test_lock_boundaries(void)
{
	struct frame_pipeline p;
	struct mock m;
	struct frame_desc *d;

	/* attach: nothing claimed on the way in; claimed at the first release;
	   linked, committed and the reservation given back by the last. */
	fresh(&p, 4);
	mock_init(&m, &p, FRAME_POLICY_DROP, 0 /* hold */);
	/* Leftovers from an imagined previous session.  The commit has to clear
	   them INSIDE the section that links the sink -- once it is in the list a
	   publish can select it, and a stale _busy or _pending is then read by the
	   producer.  Only the boundary can see that; the final state looks the same
	   either way. */
	m.sink._busy    = 1;
	m.sink._pending = (const struct frame_desc *)&m;
	m.sink.delivered = 9;
	m.sink.dropped   = 8;
	m.sink.errors    = 7;
	lk.on = 1; lk.locks = 0; lk.unlocks = 0; lk.p = &p; lk.s = &m.sink;
	assert(frame_pipeline_attach(&p, &m.sink) == FRAME_PIPELINE_OK);
	lk.on = 0;
	assert(lk.clean[lk.unlocks - 1]);          /* reset with the link, not after */
	assert(lk.entry_state == FRAME_SINK_UNOWNED && lk.entry_reserved == 0);
	assert(lk.entry_owner == NULL);
	assert(lk.unlocks >= 2);
	assert(lk.state[0] == FRAME_SINK_ATTACHING);   /* claimed under the lock */
	assert(lk.reserved[0] == 1u && lk.linked[0] == 0);
	/* [!] The owner too.  Setting it after the unlock but before open() looks
	   identical to every re-entrancy test -- the callback sees it either way --
	   while a second thread would find a claim that is not yet a claim. */
	assert(lk.owner[0] == &p);
	assert(lk.state[lk.unlocks - 1] == FRAME_SINK_ATTACHED);
	assert(lk.reserved[lk.unlocks - 1] == 0u);
	assert(lk.linked[lk.unlocks - 1] == 1);        /* committed under it too */
	assert(lk.head[lk.unlocks - 1] == &m.sink);    /* both edges of the link */
	assert(lk.next[lk.unlocks - 1] == NULL);       /* (empty list: no next)  */

	/* The other edge, with a sink already at the head: the new one must point
	   at it by the time the lock is released. */
	{
		struct mock first;

		fresh(&p, 4);
		mock_init(&first, &p, FRAME_POLICY_DROP, 1);
		mock_init(&m, &p, FRAME_POLICY_DROP, 0 /* hold */);
		assert(frame_pipeline_attach(&p, &first.sink) == FRAME_PIPELINE_OK);
		m.sink._next = (struct frame_sink *)&lk;   /* a sentinel to overwrite */
		lk.on = 1; lk.locks = 0; lk.unlocks = 0; lk.p = &p; lk.s = &m.sink;
		assert(frame_pipeline_attach(&p, &m.sink) == FRAME_PIPELINE_OK);
		lk.on = 0;
		assert(lk.head[lk.unlocks - 1] == &m.sink);
		assert(lk.next[lk.unlocks - 1] == &first.sink);
	}

	/* detach: unlinked AND marked in one section, then settled in another --
	   and the LATEST pending dropped in that same section.  Leaving the drop
	   until after the unlock lets an in-flight put() hand that frame on, and
	   the sink takes a delivery it has already been unlinked from. */
	fresh(&p, 5);
	mock_init(&m, &p, FRAME_POLICY_LATEST, 0 /* hold */);
	assert(frame_pipeline_attach(&p, &m.sink) == FRAME_PIPELINE_OK);
	d = pub(&p);                                   /* delivered              */
	(void)pub(&p);                                 /* coalesced into pending */
	assert(m.sink._pending != NULL);
	lk.on = 1; lk.locks = 0; lk.unlocks = 0; lk.p = &p; lk.s = &m.sink;
	assert(frame_pipeline_detach(&p, &m.sink) == 1);
	lk.on = 0;
	assert(lk.unlocks >= 2);
	assert(lk.state[0] == FRAME_SINK_DETACHING);   /* not left until after   */
	assert(lk.linked[0] == 0);                     /* unlinked in the same   */
	assert(lk.owner[0] == &p);                     /* still ours while closing */
	assert(lk.pending[0] == NULL);                 /* and the pending dropped */
	assert(lk.pins[0] == 1);                       /* with its pin, in there  */
	assert(lk.state[lk.unlocks - 1] == FRAME_SINK_DRAINING);
	frame_pipeline_put(&p, &m.sink, d);
	assert(m.sink._state == FRAME_SINK_UNOWNED);

	/* [!] And BOTH edges of the unlink, which needs a neighbour to see.
	   Detaching a lone sink cannot tell "repair the incoming edge" from "cut
	   the list at the target": with one sink both leave an empty list.  So:
	   two sinks, detach the HEAD, and check at the first unlock that the list
	   now starts at the other one, that it is still reachable, and that the
	   target's own edge was cleared in the same section. */
	{
		struct mock keep;

		fresh(&p, 4);
		mock_init(&keep, &p, FRAME_POLICY_DROP, 1);
		mock_init(&m, &p, FRAME_POLICY_DROP, 1);
		assert(frame_pipeline_attach(&p, &keep.sink) == FRAME_PIPELINE_OK);
		assert(frame_pipeline_attach(&p, &m.sink) == FRAME_PIPELINE_OK);
		assert(p.sinks == &m.sink);                /* the target is the head */
		lk.on = 1; lk.locks = 0; lk.unlocks = 0;
		lk.p = &p; lk.s = &m.sink; lk.other = &keep.sink;
		assert(frame_pipeline_detach(&p, &m.sink) == 0);
		lk.on = 0; lk.other = NULL;
		assert(lk.head[0] == &keep.sink);          /* incoming edge repaired */
		assert(lk.next[0] == NULL);                /* outgoing edge cleared  */
		assert(lk.linked[0] == 0);
		assert(lk.other_linked[0] == 1);           /* the neighbour survives */
		(void)pub(&p);
		assert(keep.consume_calls == 1);
		assert(m.consume_calls == 0);
	}

	/* And the delivery's callback count: raised in the section that chose the
	   sink, not somewhere between the unlock and the consume.  Moving it out
	   reads the same to a single-threaded callback and races sink_pins() and
	   detach() for real. */
	fresh(&p, 4);
	mock_init(&m, &p, FRAME_POLICY_DROP, 1);
	assert(frame_pipeline_attach(&p, &m.sink) == FRAME_PIPELINE_OK);
	d = frame_pipeline_acquire(&p);                /* its own lock cycle     */
	assert(d != NULL);
	lk.on = 1; lk.locks = 0; lk.unlocks = 0;
	frame_pipeline_publish(&p, d, SLOT_SZ, FRAME_FMT_RGB565, 4, 2, 8);
	lk.on = 0;
	assert(lk.callbacks[0] == 1);                  /* up before the unlock   */
	assert(lk.callbacks[lk.unlocks - 1] == 0);     /* and down with the stats */

	/* [!] And the LATEST hand-off, which is a SECOND way to start a callback.
	   Auditing only publish leaves it free to raise the count after the unlock
	   -- a callback that re-enters sees it either way, so no case above can
	   tell, while detach and sink_pins race it for real. */
	{
		struct frame_desc *d1;

		fresh(&p, 5);
		mock_init(&m, &p, FRAME_POLICY_LATEST, 0 /* hold */);
		assert(frame_pipeline_attach(&p, &m.sink) == FRAME_PIPELINE_OK);
		d1 = pub(&p);                          /* delivered                  */
		(void)pub(&p);                         /* coalesced into pending     */
		lk.on = 1; lk.locks = 0; lk.unlocks = 0;
		frame_pipeline_put(&p, &m.sink, d1);   /* hands off inside put()     */
		lk.on = 0;
		assert(m.consume_calls == 2);
		assert(lk.callbacks[0] == 1);          /* raised with the hand-off   */
		assert(lk.callbacks[lk.unlocks - 1] == 0);
	}
}

/* J11. a rejected open() gives everything back. */
static void test_release_paths(void)
{
	struct frame_pipeline p;
	struct mock m, m2;
	unsigned i;

	fresh(&p, 4);
	mock_init(&m, &p, FRAME_POLICY_DROP, 1);
	m.open_rc = -1;
	lk.on = 1; lk.locks = 0; lk.unlocks = 0; lk.p = &p; lk.s = &m.sink;
	assert(frame_pipeline_attach(&p, &m.sink) == FRAME_PIPELINE_ERR_OPEN);
	lk.on = 0;
	assert(m.sink._state == FRAME_SINK_UNOWNED);
	assert(m.sink._owner == NULL);
	assert(p._reserved == 0u);
	/* [!] And the release happened INSIDE the last locked section, not after
	   it.  Checking only the state on return cannot tell the difference, while
	   another thread would see a sink that is still ATTACHING with a
	   reservation nobody is going to use. */
	assert(lk.unlocks >= 2);
	assert(lk.state[lk.unlocks - 1] == FRAME_SINK_UNOWNED);
	assert(lk.owner[lk.unlocks - 1] == NULL);
	assert(lk.reserved[lk.unlocks - 1] == 0u);
	assert(lk.linked[lk.unlocks - 1] == 0);

	/* The same sink attaches cleanly next time... */
	m.open_rc = 0;
	assert(frame_pipeline_attach(&p, &m.sink) == FRAME_PIPELINE_OK);

	/* ...and the slot the refusal reserved is available to everyone else. */
	for (i = 1; i < FRAME_PIPELINE_MAX_SINKS; i++) {
		static struct mock rest[FRAME_PIPELINE_MAX_SINKS];

		mock_init(&rest[i], &p, FRAME_POLICY_DROP, 1);
		assert(frame_pipeline_attach(&p, &rest[i].sink) == FRAME_PIPELINE_OK);
	}
	mock_init(&m2, &p, FRAME_POLICY_DROP, 1);
	assert(frame_pipeline_attach(&p, &m2.sink) == FRAME_PIPELINE_ERR_FULL);
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
	test_reenter_from_open();
	test_reenter_from_close();
	test_owner_across_drain();
	test_owner_across_callback();
	test_owner_across_callback_latest();
	test_detach_no_ops();
	test_inconsistent_state();
	test_damaged_registry();
	test_format_snapshot();
	test_lock_boundaries();
	test_release_paths();
	test_read_latest();
	test_ring_cycle();
	assert(lock_depth == 0);              /* every lock balanced by unlock */
	printf("test_frame_pipeline: all assertions passed\n");
	return 0;
}
