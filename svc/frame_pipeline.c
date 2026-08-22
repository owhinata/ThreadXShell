/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    frame_pipeline.c
 * @brief   Camera frame ring + sink dispatch -- freestanding core (svc/ layer,
 *          owhinata/stm32f746g-disco#47 design / owhinata/stm32f746g-disco#46
 * implementation).
 *
 * Pure bookkeeping over caller-provided SDRAM slots: it calls no HAL and no
 * ThreadX.  Mutual exclusion is the injected @ref frame_os; all waiting and
 * threading lives in the glue (port/camera).  The core is always entered from
 * thread context under that lock -- never from an ISR.  consume() is the only
 * callback invoked with the lock released, so a non-recursive injected mutex is
 * safe.  Host-unit-tested in shell/test/test_frame_pipeline.c with a no-op lock.
 *
 * Concurrency contract (see docs/{ja,en}/architecture/frame-pipeline.md):
 *   - publish() stamps gen + pre-pins one reference per delivering sink under
 *     the lock, then calls consume() outside it;
 *   - a sink balances each pre-pin / get() with exactly one put();
 *   - a LATEST sink coalesces to one pending frame (pinned); on completion put()
 *     transfers that pin to the next delivery;
 *   - acquire() returns a free, unpinned, non-latest slot (so a pull reader's
 *     latest frame is not recycled under it);
 *   - detach() unlinks and reports the sink's in-flight pin count; the caller
 *     drains the sink's thread before freeing it.
 */
#include "frame_pipeline.h"

#include <string.h>

enum { SLOT_FREE = 0, SLOT_FILLING = 1 };

/* ---- injected mutual exclusion ------------------------------------------- */

static void pl_lock(struct frame_pipeline *p)
{
	if (p->os && p->os->lock)
		p->os->lock(p->os->ctx);
}

static void pl_unlock(struct frame_pipeline *p)
{
	if (p->os && p->os->unlock)
		p->os->unlock(p->os->ctx);
}

static struct frame_slot *slot_of(const struct frame_desc *f)
{
	return (struct frame_slot *)f->_slot;
}

/* Drop one pin a sink holds on f's slot (caller holds the lock). */
static void unpin_locked(struct frame_sink *s, const struct frame_desc *f)
{
	struct frame_slot *slot = slot_of(f);

	if (slot->refcount > 0)
		slot->refcount--;
	if (s && s->_pins > 0)
		s->_pins--;
}

/*
 * Ownership ends at quiescence, not at the last put (issue #79).
 *
 * publish() calls consume() outside the lock and re-takes it to write the
 * sink's statistics; put()'s LATEST hand-off does the same.  So the core is
 * still touching the sink after its pin is home, and handing the sink to
 * another pipeline on that signal would let the old epilogue write the new
 * session's counters under a different mutex.  Whichever of the two arrives
 * last releases it.  Caller holds the lock.
 */
static void sink_try_release_locked(struct frame_sink *s)
{
	if (s->_state == FRAME_SINK_DRAINING && s->_pins == 0 &&
	    s->_callbacks == 0) {
		s->_state = FRAME_SINK_UNOWNED;
		s->_owner = NULL;
	}
}

/* ---- producer side ------------------------------------------------------- */

int frame_pipeline_init(struct frame_pipeline *p, const struct frame_os *os,
                        void *slot_mem, uint32_t slot_size, uint32_t nslots)
{
	uint32_t i;

	if (!p || !slot_mem || slot_size == 0u || nslots == 0u ||
	    nslots > FRAME_PIPELINE_MAX_SLOTS)
		return -1;

	memset(p, 0, sizeof *p);
	p->os        = os;
	p->nslots    = nslots;
	p->slot_size = slot_size;
	p->latest    = -1;
	p->fmt       = FRAME_FMT_RGB565;

	for (i = 0; i < nslots; i++) {
		struct frame_slot *slot = &p->slots[i];

		slot->desc.data  = (uint8_t *)slot_mem + (size_t)i * slot_size;
		slot->desc._slot = slot;
		slot->state      = SLOT_FREE;
		slot->refcount   = 0;
	}
	return 0;
}

void frame_pipeline_set_format(struct frame_pipeline *p, enum frame_format fmt,
                               uint16_t w, uint16_t h)
{
	pl_lock(p);
	p->fmt    = fmt;
	p->width  = w;
	p->height = h;
	pl_unlock(p);
}

struct frame_desc *frame_pipeline_acquire(struct frame_pipeline *p)
{
	struct frame_desc *r = NULL;
	uint32_t i;

	pl_lock(p);
	for (i = 0; i < p->nslots; i++) {
		struct frame_slot *slot = &p->slots[i];

		if (slot->state == SLOT_FREE && slot->refcount == 0 &&
		    (int)i != p->latest) {
			slot->state = SLOT_FILLING;
			r = &slot->desc;
			p->stats.captured++;
			break;
		}
	}
	if (!r)
		p->stats.overruns++;
	pl_unlock(p);
	return r;
}

void frame_pipeline_publish(struct frame_pipeline *p, struct frame_desc *f,
                            uint32_t bytes, enum frame_format fmt,
                            uint16_t w, uint16_t h, uint16_t stride)
{
	struct frame_slot *slot = slot_of(f);
	struct frame_sink *deliver[FRAME_PIPELINE_MAX_SINKS];
	unsigned ndeliver = 0;
	struct frame_sink *s;
	unsigned i;
	/* [!] Bounded like every other walk (issue #79).  Bounding only the
	   registry's own walk would leave the gate defeatable: hand the same
	   damaged list to a publish and it hangs anyway, in the producer. */
	unsigned nwalk = 0;

	pl_lock(p);
	p->gen++;
	f->gen    = p->gen;
	f->bytes  = bytes;
	f->format = (uint8_t)fmt;
	f->width  = w;
	f->height = h;
	f->stride = stride;
	slot->state = SLOT_FREE;          /* no longer filling; now published */
	p->latest   = (int)(slot - p->slots);
	p->stats.published = p->gen;

	for (s = p->sinks; s != NULL && nwalk < FRAME_PIPELINE_MAX_SINKS;
	     s = s->_next, nwalk++) {
		if (s->_busy) {
			if (s->policy == FRAME_POLICY_DROP) {
				s->dropped++;
				continue;
			}
			/* FRAME_POLICY_LATEST: keep only the newest pending. */
			if (s->_pending) {
				unpin_locked(s, s->_pending);
				s->dropped++;
			}
			s->_pending = f;
			slot->refcount++;
			s->_pins++;
			continue;
		}
		s->_busy = 1;
		slot->refcount++;
		s->_pins++;
		/* [!] The callback is counted here, not just the pin (issue #79).  The
		   core is still touching this sink after the pin comes home -- the
		   epilogue below writes its statistics -- so a pin at zero does not
		   mean the core is done with it.  Ownership waits for both. */
		s->_callbacks++;
		if (ndeliver < FRAME_PIPELINE_MAX_SINKS)
			deliver[ndeliver++] = s;
	}
	pl_unlock(p);

	for (i = 0; i < ndeliver; i++) {
		int rc = deliver[i]->consume(deliver[i]->ctx, f);

		pl_lock(p);
		if (rc < 0)
			deliver[i]->errors++;
		deliver[i]->delivered++;
		/* Dropped in the SAME locked section that wrote those statistics, so
		   the count never says "done" while a write is still to come. */
		deliver[i]->_callbacks--;
		sink_try_release_locked(deliver[i]);
		pl_unlock(p);
	}
}

/* ---- sink registry ------------------------------------------------------- */

/*
 * Walk the registry once, bounded (issue #79).
 *
 * Answers both questions that need the same walk: how many links the list has,
 * and how many of them are @p s.  A list that has not ended after more links
 * than can exist is damaged -- a cycle, or a duplicate -- and saying so is the
 * point: "linked exactly once" cannot be asked of a looped list without
 * reproducing the very hang this file is closing.
 *
 * Returns OK, or ERR_STATE for a damaged list.  Caller holds the lock.
 */
static int registry_walk_locked(const struct frame_pipeline *p,
                                const struct frame_sink *s,
                                unsigned *links, unsigned *found)
{
	const struct frame_sink *it;
	unsigned n = 0, f = 0;

	for (it = p->sinks; it != NULL; it = it->_next) {
		if (n >= FRAME_PIPELINE_MAX_SINKS + 1u)
			return FRAME_PIPELINE_ERR_STATE;   /* a cycle: stop walking  */
		n++;
		if (it == s)
			f++;
	}
	if (n > FRAME_PIPELINE_MAX_SINKS)
		return FRAME_PIPELINE_ERR_STATE;           /* more than can exist    */
	*links = n;
	*found = f;
	return FRAME_PIPELINE_OK;
}

/*
 * Do the state, the owner, membership and the callback count agree? (issue #79)
 *
 * The model is required exactly, not summarised, because every condition left
 * out is a way for a corrupted sink to reach real work.  The two that look
 * redundant are the ones that bite: an ATTACHED sink with no owner would
 * otherwise walk into detach and have a board's close() called on it, and an
 * UNOWNED sink that is still linked would let detach answer a bare pin count
 * while it sits in the list -- a caller reading that zero tears down with the
 * sink still attached.
 *
 * [!] A negative CALLBACK count belongs here because it makes every other
 * reading suspect.  A negative PIN count deliberately does NOT: it stays
 * "undrained" at the entry points, which is what a3cd07a decided and what a
 * landed test holds.  A negative pin is "this sink still owes something" seen
 * through corrupted bookkeeping; a negative callback count is the bookkeeping
 * itself being untrustworthy.
 *
 * Returns OK or ERR_STATE.  Caller holds the lock.
 */
static int sink_consistent_locked(const struct frame_pipeline *p,
                                  const struct frame_sink *s,
                                  unsigned links, unsigned found)
{
	int outstanding = (s->_pins != 0 || s->_callbacks != 0);

	if (s->_callbacks < 0)
		return FRAME_PIPELINE_ERR_STATE;
	/* The reservation is part of the model too: a sink cannot be mid-attach
	   without the capacity its claim took, and a TOTAL past the maximum is not
	   a full pipeline, it is a broken one -- attach would answer "full" while
	   detach and sink_pins carried on as though nothing were wrong. */
	if (p->_reserved > FRAME_PIPELINE_MAX_SINKS ||
	    links + p->_reserved > FRAME_PIPELINE_MAX_SINKS)
		return FRAME_PIPELINE_ERR_STATE;

	switch ((enum frame_sink_state)s->_state) {
	case FRAME_SINK_UNOWNED:
		/* [!] No callbacks either.  The count is the core's own, so a sink
		   nobody owns cannot legitimately have one out -- and letting that
		   through had detach answer a bare zero while a callback still held
		   the caller's ctx, which is the one thing zero must never mean.
		   `_pins` stays out of this on purpose: a3cd07a answers UNDRAINED for
		   a leftover pin here, and a landed test holds it there. */
		if (s->_owner != NULL || found != 0u || s->_callbacks != 0)
			return FRAME_PIPELINE_ERR_STATE;
		break;
	case FRAME_SINK_ATTACHING:
		/* Both counters are zero here by construction: the claim was granted
		   only after attach found them so, and nothing can raise them while
		   the sink is not in the registry.  A positive one is therefore
		   corruption -- and calling it a transition would have all three ask a
		   port to retry it for ever. */
		if (s->_owner != p || found != 0u || p->_reserved == 0u ||
		    s->_pins != 0 || s->_callbacks != 0)
			return FRAME_PIPELINE_ERR_STATE;
		break;
	case FRAME_SINK_DETACHING:
		if (s->_owner != p || found != 0u || s->_pins < 0)
			return FRAME_PIPELINE_ERR_STATE;
		break;
	case FRAME_SINK_ATTACHED:
		if (s->_owner != p || found != 1u || s->_pins < 0)
			return FRAME_PIPELINE_ERR_STATE;
		break;
	case FRAME_SINK_DRAINING:
		/* "something outstanding" is part of the model: the section that makes
		   a sink quiescent moves it to UNOWNED in the same breath, so DRAINING
		   with nothing left cannot be reached -- and reading it innocently is
		   the dangerous way round, since sink_pins() would answer a bare zero
		   and zero is what a port reads as permission to tear down. */
		if (s->_owner != p || found != 0u || !outstanding || s->_pins < 0)
			return FRAME_PIPELINE_ERR_STATE;
		break;
	default:
		return FRAME_PIPELINE_ERR_STATE;
	}
	return FRAME_PIPELINE_OK;
}


/*
 * May @p s be attached to @p p?  (issue #72, reworked by #79)
 *
 * The order IS the specification -- overlapping rows leave the answer to
 * whoever writes the code:
 *
 *   1. this operation's own legitimate refusals, which are ordinary misuse
 *      rather than corruption and keep their own names.  A sink already in the
 *      registry is "already attached" when the state agrees it could be
 *      (a3cd07a landed membership-first and a test holds it there) -- and is
 *      corruption when the state says it is mid-transition or draining, since
 *      linked is not a legal pairing for any of those;
 *   2. the shared consistency check;
 *   3. the state's own answer, then the counters, then capacity.
 *
 * Already-linked stays ahead of capacity, or a re-attach into a full pipeline
 * would be reported as "full", sending someone to look for a sink slot when the
 * actual fault is that this sink is already in one.
 *
 * The caller holds the lock.  Nothing here calls out.
 */
static int attach_refusal_locked(const struct frame_pipeline *p,
                                 const struct frame_sink *s,
                                 unsigned links, unsigned found)
{
	int err;

	if (found != 0u) {
		if (s->_state == FRAME_SINK_ATTACHED ||
		    s->_state == FRAME_SINK_UNOWNED)
			return FRAME_PIPELINE_ERR_ATTACHED;
		return FRAME_PIPELINE_ERR_STATE;
	}
	/* Owned elsewhere is misuse, not corruption -- and it has its own name for
	   the same reason detach's does: a sink legitimately held by another
	   pipeline is not damaged just because this one asked about it. */
	if (s->_owner != NULL && s->_owner != p)
		return FRAME_PIPELINE_ERR_OWNER;
	err = sink_consistent_locked(p, s, links, found);
	if (err != FRAME_PIPELINE_OK)
		return err;

	switch ((enum frame_sink_state)s->_state) {
	case FRAME_SINK_ATTACHING:
	case FRAME_SINK_DETACHING:
		return FRAME_PIPELINE_ERR_TRANSITION;
	case FRAME_SINK_ATTACHED:
		return FRAME_PIPELINE_ERR_ATTACHED;  /* unreachable: found would be 1 */
	case FRAME_SINK_DRAINING:
		/* Kept apart on purpose: a sink whose callback put from inside itself
		   genuinely reaches pins 0 with that callback still out, and answering
		   "undrained" there tells an owner to return a pin it already did. */
		return (s->_callbacks > 0) ? FRAME_PIPELINE_ERR_QUIESCE
		                           : FRAME_PIPELINE_ERR_UNDRAINED;
	case FRAME_SINK_UNOWNED:
		break;
	}
	/*
	 * [!] EXACTLY ZERO, not "zero or less".  unpin_locked() saturates its
	 * decrement at zero, so this count cannot go negative through the pipeline
	 * -- a negative one means the sink's bookkeeping was written by something
	 * that had no business writing it, and reading that as "released" would let
	 * the reset below run over a callback that is still inside the sink.
	 */
	if (s->_pins != 0)
		return FRAME_PIPELINE_ERR_UNDRAINED;
	if (s->_callbacks > 0)
		return FRAME_PIPELINE_ERR_QUIESCE;
	if (p->_reserved > FRAME_PIPELINE_MAX_SINKS)
		return FRAME_PIPELINE_ERR_STATE;     /* must not wrap into "room"    */
	if (links + p->_reserved >= FRAME_PIPELINE_MAX_SINKS)
		return FRAME_PIPELINE_ERR_FULL;
	return FRAME_PIPELINE_OK;
}

int frame_pipeline_attach(struct frame_pipeline *p, struct frame_sink *s)
{
	enum frame_format fmt = FRAME_FMT_RGB565;
	uint16_t w = 0, h = 0;
	unsigned links = 0, found = 0;
	int err;

	if (!p || !s)
		return FRAME_PIPELINE_ERR_PARAM;

	pl_lock(p);
	err = registry_walk_locked(p, s, &links, &found);
	if (err == FRAME_PIPELINE_OK)
		err = attach_refusal_locked(p, s, links, found);
	if (err == FRAME_PIPELINE_OK) {
		/* The claim.  Nothing about the decision above can change under it now:
		   no other attach of this sink gets past it, a detach finds nothing to
		   unlink, and the pin count cannot move because only a linked sink is
		   ever delivered to.  The format is claimed with it -- open() runs
		   unlocked, and a set_format() in between would otherwise hand the sink
		   a mix of old and new fields. */
		s->_owner = p;
		s->_state = FRAME_SINK_ATTACHING;
		p->_reserved++;
		fmt = p->fmt;
		w   = p->width;
		h   = p->height;
	}
	pl_unlock(p);
	if (err != FRAME_PIPELINE_OK)
		return err;                  /* refused, and nothing was touched */

	/*
	 * Normalised, not passed through (issue #72).  A board's open() can return
	 * any negative it likes, so forwarding it would make every value the core
	 * picks for its own refusals forgeable -- and the whole point of naming
	 * those refusals is that a caller can tell them apart from this one.
	 */
	if (s->open && s->open(s->ctx, fmt, w, h) < 0) {
		pl_lock(p);
		s->_state = FRAME_SINK_UNOWNED;
		s->_owner = NULL;
		if (p->_reserved > 0u)
			p->_reserved--;
		pl_unlock(p);
		return FRAME_PIPELINE_ERR_OPEN;
	}

	pl_lock(p);
	if (p->_reserved > 0u)
		p->_reserved--;
	/* Safe to reset only because the refusal above proved there is nothing here
	   worth keeping: no pins, no callbacks, not linked.  The three counters are
	   per-session and the boards read them that way. */
	s->_busy    = 0;
	s->_pending = NULL;
	s->_pins    = 0;
	s->_callbacks = 0;
	s->delivered = 0;
	s->dropped   = 0;
	s->errors    = 0;
	s->_state = FRAME_SINK_ATTACHED;
	s->_next  = p->sinks;
	p->sinks  = s;
	pl_unlock(p);
	return FRAME_PIPELINE_OK;
}

const char *frame_pipeline_strerror(int rc)
{
	switch (rc) {
	case FRAME_PIPELINE_OK:             return "ok";
	case FRAME_PIPELINE_ERR_PARAM:      return "bad argument";
	case FRAME_PIPELINE_ERR_FULL:       return "pipeline full";
	case FRAME_PIPELINE_ERR_OPEN:       return "sink open() rejected";
	case FRAME_PIPELINE_ERR_ATTACHED:   return "already attached";
	case FRAME_PIPELINE_ERR_UNDRAINED:  return "sink still holds frame pins";
	case FRAME_PIPELINE_ERR_TRANSITION: return "attach or detach in progress";
	case FRAME_PIPELINE_ERR_QUIESCE:    return "a sink callback has not returned";
	case FRAME_PIPELINE_ERR_OWNER:      return "sink belongs to another pipeline";
	case FRAME_PIPELINE_ERR_STATE:      return "sink bookkeeping is inconsistent";
	default:                            return "unknown";
	}
}

int frame_pipeline_detach(struct frame_pipeline *p, struct frame_sink *s)
{
	struct frame_sink **pp;
	unsigned links = 0, found = 0;
	int err, inflight;

	if (!p || !s)
		return FRAME_PIPELINE_ERR_PARAM;

	pl_lock(p);
	err = registry_walk_locked(p, s, &links, &found);
	/* This operation's own legitimate refusal, ahead of the shared check: a
	   sink that belongs to another pipeline is being misused, not corrupted. */
	if (err == FRAME_PIPELINE_OK && s->_owner != NULL && s->_owner != p)
		err = FRAME_PIPELINE_ERR_OWNER;
	/* [!] And a corrupted pin count is refused BEFORE anything destructive.
	   Unlink first and report the error afterwards, and the port reads "the
	   detach did not happen" while the core has already unlinked -- a
	   disagreement worse than either fact on its own.  (A negative CALLBACK
	   count never reaches here: the consistency check answers it.) */
	if (err == FRAME_PIPELINE_OK && s->_pins < 0)
		err = FRAME_PIPELINE_ERR_STATE;
	if (err == FRAME_PIPELINE_OK)
		err = sink_consistent_locked(p, s, links, found);
	if (err != FRAME_PIPELINE_OK) {
		pl_unlock(p);
		return err;
	}

	switch ((enum frame_sink_state)s->_state) {
	case FRAME_SINK_ATTACHING:
	case FRAME_SINK_DETACHING:
		pl_unlock(p);
		return FRAME_PIPELINE_ERR_TRANSITION;
	case FRAME_SINK_DRAINING:
		/* Never a bare zero here: a callback still running with the caller's
		   ctx cannot be expressed as a pin count, and zero is what the caller
		   is told to read as permission to free. */
		err = (s->_callbacks > 0) ? FRAME_PIPELINE_ERR_QUIESCE : s->_pins;
		pl_unlock(p);
		return err;
	case FRAME_SINK_UNOWNED:
		/* Nobody's sink: a real no-op.  No pending drop, no close(), no state
		   change -- telling a sink it was detached when it never was linked is
		   the third of this issue's four holes. */
		inflight = s->_pins;
		pl_unlock(p);
		return inflight;
	case FRAME_SINK_ATTACHED:
		break;
	}

	for (pp = &p->sinks; *pp != NULL; pp = &(*pp)->_next) {
		if (*pp == s) {
			*pp = s->_next;
			break;
		}
	}
	s->_next = NULL;
	/* A LATEST pending frame was never delivered to the sink, so the sink can
	   never put() it.  Drop it here -- BEFORE the count is read below, so what
	   the caller drains towards zero is only what the sink can actually hand
	   back, and so the sink is not left holding a pin it can never return
	   (which would strand it in DRAINING for good).  A call already scheduled
	   by publish() or by put()'s pending hand-off still runs after this unlink;
	   each holds its own pin and its own callback count, so both stay honest. */
	if (s->_pending) {
		unpin_locked(s, s->_pending);
		s->_pending = NULL;
	}
	inflight = s->_pins;
	s->_state = FRAME_SINK_DETACHING;
	pl_unlock(p);

	if (s->close)
		s->close(s->ctx);

	pl_lock(p);
	s->_state = FRAME_SINK_DRAINING;
	sink_try_release_locked(s);
	pl_unlock(p);
	return inflight;
}

int frame_pipeline_sink_pins(struct frame_pipeline *p,
                             const struct frame_sink *s)
{
	unsigned links = 0, found = 0;
	int err, pins;

	if (!p || !s)
		return 0;

	/* Under the lock even though the count is one word, and not for tearing:
	 * publish() and put() move it from other threads, and an unsynchronised
	 * read alongside them is a data race whatever the width.  Since #79 the
	 * walk is here too -- the three entry points have to classify a damaged
	 * sink the same way, or a port retries what another call called terminal.
	 */
	pl_lock(p);
	err = registry_walk_locked(p, s, &links, &found);
	if (err == FRAME_PIPELINE_OK && s->_owner != NULL && s->_owner != p)
		err = FRAME_PIPELINE_ERR_OWNER;
	if (err == FRAME_PIPELINE_OK)
		err = sink_consistent_locked(p, s, links, found);
	if (err == FRAME_PIPELINE_OK) {
		if (s->_state == FRAME_SINK_ATTACHING ||
		    s->_state == FRAME_SINK_DETACHING)
			err = FRAME_PIPELINE_ERR_TRANSITION;
		else if (s->_callbacks > 0)
			err = FRAME_PIPELINE_ERR_QUIESCE;
		else if (s->_pins < 0)
			err = FRAME_PIPELINE_ERR_STATE;
	}
	pins = (err == FRAME_PIPELINE_OK) ? s->_pins : err;
	pl_unlock(p);
	return pins;
}

int frame_pipeline_sink_count(struct frame_pipeline *p)
{
	struct frame_sink *it;
	int n = 0;

	if (!p)
		return 0;

	pl_lock(p);
	/* Bounded, for the reason above.  A healthy pipeline still reports exactly
	   its sinks; a damaged list stops ONE PAST the maximum, which reads as
	   "something is attached" -- the fail-closed direction for the one caller,
	   whose question is whether the registry is empty enough to start. */
	for (it = p->sinks; it != NULL && n <= (int)FRAME_PIPELINE_MAX_SINKS;
	     it = it->_next)
		n++;
	pl_unlock(p);
	return n;
}

/* ---- slot reference counting --------------------------------------------- */

void frame_pipeline_get(struct frame_pipeline *p, struct frame_sink *s,
                        const struct frame_desc *f)
{
	struct frame_slot *slot = slot_of(f);

	pl_lock(p);
	slot->refcount++;
	if (s)
		s->_pins++;
	pl_unlock(p);
}

void frame_pipeline_put(struct frame_pipeline *p, struct frame_sink *s,
                        const struct frame_desc *f)
{
	const struct frame_desc *next = NULL;

	pl_lock(p);
	unpin_locked(s, f);
	if (s) {
		s->_busy = 0;
		if (s->_pending) {
			/* Transfer the pending pin into the next delivery. */
			next = s->_pending;
			s->_pending = NULL;
			s->_busy = 1;
			s->_callbacks++;      /* the hand-off consume below (issue #79) */
		} else {
			/* Nothing more scheduled: this may be the last thing the core
			   owed a draining sink. */
			sink_try_release_locked(s);
		}
	}
	pl_unlock(p);

	if (next) {
		int rc = s->consume(s->ctx, next);

		pl_lock(p);
		if (rc < 0)
			s->errors++;
		s->delivered++;
		s->_callbacks--;
		sink_try_release_locked(s);
		pl_unlock(p);
	}
}

/* ---- pull access --------------------------------------------------------- */

int frame_pipeline_read_latest(struct frame_pipeline *p, uint32_t off,
                               void *dst, uint32_t len, uint32_t *gen)
{
	int rc = -1;

	if (!dst || len == 0u)
		return -1;

	pl_lock(p);
	if (p->latest >= 0) {
		struct frame_slot *slot = &p->slots[p->latest];

		if (off < slot->desc.bytes && len <= slot->desc.bytes - off) {
			memcpy(dst, (const uint8_t *)slot->desc.data + off, len);
			if (gen)
				*gen = slot->desc.gen;
			rc = 0;
		}
	}
	pl_unlock(p);
	return rc;
}

const struct frame_desc *frame_pipeline_pin_latest(struct frame_pipeline *p)
{
	const struct frame_desc *r = NULL;

	pl_lock(p);
	if (p->latest >= 0) {
		struct frame_slot *slot = &p->slots[p->latest];

		slot->refcount++;               /* keep it out of acquire() until put() */
		r = &slot->desc;
	}
	pl_unlock(p);
	return r;                           /* release with frame_pipeline_put(p, NULL, r) */
}

/* ---- statistics ---------------------------------------------------------- */

void frame_pipeline_stats(struct frame_pipeline *p, struct frame_stats *out)
{
	if (!out)
		return;
	pl_lock(p);
	*out = p->stats;
	pl_unlock(p);
}
