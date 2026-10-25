/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    nn_core.c
 * @brief   The policy around the shared stream lifecycle (issue #130) -- see
 *          nn_core.h for what is here and what stays with the board.
 */
#include "nn_core.h"

#include <stddef.h>   /* NULL */
#include <string.h>   /* memset */

enum nn_stream_start_claim nn_core_gate_refusal(const struct nn_stream_life *l)
{
	uint8_t phase = 0u, kind = 0u;

	/* [!] THE HOLDER DECIDES THE WORDS (issue #122).  The lifecycle knows
	 * which holder it is; an idle lifecycle means an ordinary operation (a
	 * load, a bench) has the gate.  Read without a transition, so a refused
	 * start moves nothing. */
	nn_stream_life_snapshot(l, NULL, &phase, NULL, &kind);
	switch ((enum nn_stream_phase)phase) {
	case NN_STREAM_PHASE_LOST:
		return NN_STREAM_START_DEAD;
	case NN_STREAM_PHASE_IDLE:
		return NN_STREAM_START_BUSY;           /* an operation holds it */
	default:
		if (kind == (uint8_t)NN_STREAM_KIND_ONESHOT)
			return NN_STREAM_START_ONESHOT;
		return (phase == (uint8_t)NN_STREAM_PHASE_RUNNING)
		       ? NN_STREAM_START_RUNNING : NN_STREAM_START_BUSY;
	}
}

/* The gate is tested FIRST, and only a board that has one is asked. */
static int nn_core_gated(const struct nn_core_board *b)
{
	return b->gate_held != NULL && b->gate_held();
}

enum nn_stream_start_claim nn_core_admit(struct nn_core *c,
                                         const struct nn_core_board *b)
{
	enum nn_stream_start_claim r;
	unsigned s = b->cs_enter();

	if (nn_core_gated(b)) {
		r = nn_core_gate_refusal(&c->life);
	} else {
		r = nn_stream_life_begin(&c->life, NN_STREAM_KIND_STREAM);
		/* [!] A re-arm is admitted only where a re-arm is possible: from
		 * RUNNING.  Trying it after any other refusal would let it through
		 * from STOPPING -- and after NN_STREAM_START_ONESHOT it would take
		 * over a `nn run`'s session, which is issue #120.  rearm() refuses a
		 * one-shot as well. */
		if (r == NN_STREAM_START_RUNNING && b->rearm &&
		    nn_stream_life_rearm(&c->life))
			r = NN_STREAM_START_GO;
		if (r == NN_STREAM_START_GO && b->gate_take != NULL)
			b->gate_take(0);
	}
	b->cs_exit(s);
	return r;
}

enum nn_stream_start_claim nn_core_oneshot_admit(struct nn_core *c,
                                                 const struct nn_core_board *b)
{
	enum nn_stream_start_claim r;
	unsigned s = b->cs_enter();

	if (nn_core_gated(b)) {
		r = nn_core_gate_refusal(&c->life);
	} else {
		r = nn_stream_life_begin(&c->life, NN_STREAM_KIND_ONESHOT);
		if (r == NN_STREAM_START_GO && b->gate_take != NULL)
			b->gate_take(1);
	}
	b->cs_exit(s);
	return r;
}

uint32_t nn_core_oneshot_commit(struct nn_core *c,
                                const struct nn_core_board *b)
{
	uint32_t g;
	unsigned s = b->cs_enter();

	g = nn_stream_life_commit(&c->life);
	b->cs_exit(s);
	return g;
}

uint32_t nn_core_oneshot_claim(struct nn_core *c, const struct nn_core_board *b,
                               enum nn_stream_start_claim *why)
{
	uint32_t g = NN_STREAM_GEN_ANY;
	unsigned s = b->cs_enter();

	if (nn_core_gated(b)) {
		*why = nn_core_gate_refusal(&c->life);
	} else {
		*why = nn_stream_life_begin(&c->life, NN_STREAM_KIND_ONESHOT);
		if (*why == NN_STREAM_START_GO) {
			g = nn_stream_life_commit(&c->life);
			if (g == NN_STREAM_GEN_ANY) {
				(void)nn_stream_life_abort(&c->life);
				*why = NN_STREAM_START_BUSY;
			} else if (b->gate_take != NULL) {
				b->gate_take(1);
			}
		}
	}
	b->cs_exit(s);
	return g;
}

int nn_core_abort(struct nn_core *c, const struct nn_core_board *b)
{
	int took;
	unsigned s = b->cs_enter();

	took = nn_stream_life_abort(&c->life);
	if (took && b->gate_give != NULL)
		b->gate_give();
	b->cs_exit(s);
	return took;
}

/* The base of one counter: its sample if the board bases it, else zero. */
static uint32_t nn_core_based(uint8_t based, uint8_t bit, uint32_t v)
{
	return (based & bit) ? v : 0u;
}

uint32_t nn_core_commit(struct nn_core *c, const struct nn_core_board *b,
                        const uint32_t *acc0)
{
	struct nn_core_raw raw;
	uint32_t g, a;
	uint8_t m = b->based;
	unsigned s;

	/* Outside the critical section: these take the board's locks.  [!] THE
	 * COUNTERS FIRST, THEN THE RECORD -- see nn_core_board::based. */
	memset(&raw, 0, sizeof raw);
	if (m != 0u)
		b->counts(&raw, NULL);
	if (acc0 != NULL) {
		a = *acc0;
	} else {
		struct nn_det_snapshot rec;

		memset(&rec, 0, sizeof rec);
		b->record(&rec);
		a = rec.accepted;
	}

	s = b->cs_enter();
	g = nn_stream_life_commit(&c->life);
	if (g != NN_STREAM_GEN_ANY) {
		c->base.offered        = nn_core_based(m, NN_CORE_BASED_OFFERED,
		                                       raw.offered);
		c->base.skipped        = nn_core_based(m, NN_CORE_BASED_SKIPPED,
		                                       raw.skipped);
		c->base.infers         = nn_core_based(m, NN_CORE_BASED_INFERS,
		                                       raw.infers);
		c->base.errors         = nn_core_based(m, NN_CORE_BASED_ERRORS,
		                                       raw.errors);
		c->base.model_errors   = nn_core_based(m, NN_CORE_BASED_MODEL_ERRORS,
		                                       raw.model_errors);
		c->base.decoder_errors = nn_core_based(m,
		                                       NN_CORE_BASED_DECODER_ERRORS,
		                                       raw.decoder_errors);
		c->acc0 = a;
		c->t0   = b->ticks();
		c->ms   = 0u;
	}
	b->cs_exit(s);
	return g;
}

enum nn_stream_stop_claim nn_core_claim_stop(struct nn_core *c,
                                             const struct nn_core_board *b,
                                             uint32_t gen)
{
	enum nn_stream_stop_claim r;
	unsigned s = b->cs_enter();

	r = nn_stream_life_claim_stop(&c->life, gen);
	b->cs_exit(s);
	return r;
}

int nn_core_oneshot_end(struct nn_core *c, const struct nn_core_board *b,
                        uint32_t gen)
{
	int ok = 0;
	unsigned s = b->cs_enter();

	/* [!] THE GATE IS GIVEN BACK ONLY IF THE TRANSITION HAPPENED.  Nothing
	 * else can have claimed this one-shot's stop, so a refusal is an
	 * invariant failure; the gate then stays held rather than handing the
	 * hardware back on it. */
	if (nn_stream_life_claim_stop(&c->life, gen) == NN_STREAM_STOP_GO &&
	    nn_stream_life_finish(&c->life)) {
		if (b->gate_give != NULL)
			b->gate_give();
		ok = 1;
	}
	b->cs_exit(s);
	return ok;
}

void nn_core_counts(const struct nn_core_raw *raw,
                    const struct nn_core_base *base,
                    struct nn_stream_stats *out)
{
	/* [!] OFFERED, not ingested: `skipped` has to be a subset of `frames`
	 * or the pair cannot be read. */
	out->frames         = nn_core_since(raw->offered, base->offered);
	out->skipped        = nn_core_since(raw->skipped, base->skipped);
	out->infers         = nn_core_since(raw->infers, base->infers);
	out->errors         = nn_core_since(raw->errors, base->errors);
	out->model_errors   = nn_core_since(raw->model_errors,
	                                    base->model_errors);
	out->decoder_errors = nn_core_since(raw->decoder_errors,
	                                    base->decoder_errors);
	out->last_us        = raw->last_us;
}

void nn_core_take_final(struct nn_core *c, const struct nn_core_board *b,
                        struct nn_core_final *f, void *keep)
{
	struct nn_core_raw raw;
	struct nn_core_base base;
	struct nn_det_snapshot rec;
	uint32_t acc0;
	unsigned s = b->cs_enter();

	base = c->base;
	acc0 = c->acc0;
	b->cs_exit(s);

	/* Outside the critical section: these take the board's own locks. */
	memset(&raw, 0, sizeof raw);
	b->counts(&raw, keep);
	memset(&rec, 0, sizeof rec);
	b->record(&rec);

	memset(f, 0, sizeof *f);
	nn_core_counts(&raw, &base, &f->stats);
	/*
	 * [!] THE STREAM'S LAST RESULT IS LATCHED WITH ITS NUMBERS (issue #118).
	 * The stop no longer retires the record, so there is a last result to
	 * report -- taken now, after the board's stop boundary, so a later `nn
	 * run` cannot show through it.  Its epoch goes with it.
	 */
	f->stats.last_valid = nn_det_last_valid(&rec, acc0) ? 1u : 0u;
	f->stats.last_ndet  = (int32_t)rec.ndet;
	f->epoch            = rec.epoch;
}

static uint32_t nn_core_elapsed_ms(const struct nn_core_board *b, uint32_t t0)
{
	return (uint32_t)((b->ticks() - t0) * 1000u / b->ticks_per_s);
}

void nn_core_settle(struct nn_core *c, const struct nn_core_board *b,
                    enum nn_claim claim, const struct nn_core_final *f,
                    const void *extra)
{
	unsigned s = b->cs_enter();

	if (claim == NN_CLAIM_RETRYABLE) {
		(void)nn_stream_life_retry(&c->life);  /* stoppable again, same gen */
	} else {
		int terminal = (claim == NN_CLAIM_TERMINAL);
		int stream = (c->life.kind == (uint8_t)NN_STREAM_KIND_STREAM);
		uint32_t ending = c->life.gen;
		int took = terminal ? nn_stream_life_poison(&c->life)
		                    : nn_stream_life_finish(&c->life);

		if (took && stream) {
			c->ms = nn_core_elapsed_ms(b, c->t0);
			/* The ended stream's numbers, for every poll from here on. */
			if (f != NULL) {
				c->final = *f;
				c->final.stats.elapsed_ms = c->ms;
				c->final_gen = ending;
				if (b->latch_extra != NULL)
					b->latch_extra(extra);
			}
		}
		/* [!] AND THE GATE ONLY ON A FINISH THAT TOOK: never on a poison,
		 * which means something may still be inside it. */
		if (took && !terminal && b->gate_give != NULL)
			b->gate_give();
	}
	b->cs_exit(s);
}

int nn_core_ended(const struct nn_core *c)
{
	uint32_t g = NN_STREAM_GEN_ANY;

	nn_stream_life_snapshot(&c->life, &g, NULL, NULL, NULL);
	return g != NN_STREAM_GEN_ANY && g == c->final_gen;
}

int nn_core_poll(struct nn_core *c, const struct nn_core_board *b, uint32_t gen,
                 struct nn_stream_stats *out)
{
	struct nn_core_raw raw;
	struct nn_core_base base;
	struct nn_det_snapshot rec;
	uint32_t seq0 = 0u, seq1 = 0u, g = 0u, t0, ms, acc0;
	uint8_t  phase = 0u, kind = 0u;
	int      live, clock_on;
	unsigned s;

	if (out == NULL)
		return NN_SVC_ERR_ARG;

	/* Phase 1: identity and baselines. */
	s = b->cs_enter();
	nn_stream_life_snapshot(&c->life, &g, &phase, &seq0, &kind);
	/* [!] An ended stream answers from its latch, taken in the same critical
	 * section as the generation it belongs to -- see nn_core::final. */
	if (g != NN_STREAM_GEN_ANY && g == c->final_gen &&
	    (gen == NN_STREAM_GEN_ANY || gen == g)) {
		uint32_t ep = c->final.epoch;

		*out = c->final.stats;
		b->cs_exit(s);
		/* [!] ...except that a model change since the stop took its last
		 * result away (issue #118): the latch is history, but `last` says
		 * what is still there to read with `nn dets`.  The record takes its
		 * own lock, so the epoch is compared just after. */
		memset(&rec, 0, sizeof rec);
		b->record(&rec);
		if (rec.epoch != ep)
			out->last_valid = 0u;
		return NN_SVC_OK;
	}
	t0   = c->t0;
	ms   = c->ms;
	acc0 = c->acc0;
	base = c->base;
	b->cs_exit(s);

	if (g == NN_STREAM_GEN_ANY)
		return NN_SVC_ERR_STATE;                  /* nothing has ever run */
	/* [!] Only a DIFFERENT generation is somebody else's.  Our own, finished,
	 * still answers -- that is how a waiter tells that its stream ended from
	 * a successor running, and they call for different words. */
	if (gen != NN_STREAM_GEN_ANY && gen != g)
		return NN_SVC_ERR_GEN;

	/* Phase 2, OUTSIDE the critical section: these take their own locks. */
	memset(&raw, 0, sizeof raw);
	b->counts(&raw, NULL);
	memset(&rec, 0, sizeof rec);
	b->record(&rec);

	/* Phase 3: accept only if nothing moved -- the counter, not the
	 * generation and the phase. */
	s = b->cs_enter();
	nn_stream_life_snapshot(&c->life, NULL, NULL, &seq1, NULL);
	b->cs_exit(s);
	if (seq1 != seq0)
		return NN_SVC_ERR_STALE;

	/* [!] A `nn run` holding the lifecycle is not a running STREAM (issue
	 * #120): it is reported as not running, and its end does not date one. */
	live = (phase == (uint8_t)NN_STREAM_PHASE_RUNNING &&
	        kind == (uint8_t)NN_STREAM_KIND_STREAM);

	memset(out, 0, sizeof *out);
	out->running = (live && raw.producing) ? 1u : 0u;
	/* Per GENERATION, not per worker lifetime -- see nn_core::base. */
	nn_core_counts(&raw, &base, out);
	clock_on = b->clock_needs_producer ? (int)out->running : live;
	out->elapsed_ms = clock_on ? nn_core_elapsed_ms(b, t0) : ms;
	/*
	 * [!] THIS STREAM'S, OR NONE (issue #118).  The record keeps the
	 * previous stream's result across the boundary, so `valid` alone would
	 * open this one with the last one's items.  The record's accepted count
	 * against the base the commit latched says whether this one published.
	 */
	out->last_valid = nn_det_last_valid(&rec, acc0) ? 1u : 0u;
	/* [!] AND NEVER WHILE THE LIFECYCLE NAMES A ONE-SHOT (issue #118, review):
	 * the base is the last STREAM's, and a `nn run` publishes into the same
	 * record -- its publish would count against the stream's base. */
	if (kind != (uint8_t)NN_STREAM_KIND_STREAM)
		out->last_valid = 0u;
	out->last_ndet  = (int32_t)rec.ndet;
	return NN_SVC_OK;
}
