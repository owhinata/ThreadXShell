/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    nn_core_frame.c
 * @brief   The frame path of the shared nn core (issue #130) -- see
 *          nn_core_frame.h for what is here and what stays with the board.
 *
 * [!] NO MUTABLE STORAGE, and no call into a plugin of its own: the decode and
 * the paint are the board's hooks.
 */
#include "nn_core_frame.h"

#include <stddef.h>   /* NULL */
#include <string.h>   /* memset */

/* One transition, one critical section: the test and the change together. */
static int nn_cf_step(struct nn_core_frame *f,
                      const struct nn_core_frame_ops *o, uint8_t op)
{
	int moved;
	unsigned s = o->cs_enter();

	moved = nn_handoff_step(&f->word, op);
	o->cs_exit(s);
	return moved;
}

/* ---- the producer ---------------------------------------------------------- */

enum nn_core_on_frame nn_core_on_frame(struct nn_core_frame *f,
                                       const struct nn_core_frame_ops *o,
                                       void *ctx, unsigned part,
                                       unsigned nparts)
{
	const int last = (part + 1u >= nparts);
	int writing;
	unsigned s;

	if (o->present != NULL)
		o->present(ctx, part, nparts);

	s = o->cs_enter();
	/*
	 * [!] A FILL BEGINS AT THE FIRST PART AND NEVER PART WAY THROUGH.  The
	 * parts before this one were not written, so a frame begun here would
	 * hand the worker an input whose top is whatever was there before.
	 */
	if (part == 0u) {
		f->frame_no++;
		(void)nn_handoff_step(&f->word, (uint8_t)NN_HO_OP_BEGIN);
	}
	writing = (f->word == (uint8_t)NN_HO_FILLING);
	if (writing && part == 0u)
		f->job_frame = f->frame_no;
	o->cs_exit(s);

	/* [!] Skipped once per frame, at its last part: a frame the worker did
	 * not want, or one abandoned part way through, is one frame not
	 * inferred however many parts it had. */
	if (!writing)
		return last ? NN_CORE_FR_SKIPPED : NN_CORE_FR_NONE;

	/* FILLING is the producer's alone until it HANDs or ABANDONs, so the
	 * input is written OUTSIDE the critical section. */
	if (o->prep(ctx, part, nparts) != 0) {
		/* After the writing stopped, never before: ABANDON hands the input
		 * back to nobody but the next first part. */
		(void)nn_cf_step(f, o, (uint8_t)NN_HO_OP_ABANDON);
		return NN_CORE_FR_ABANDONED;
	}

	if (!last) {
		/* Still the producer's?  Only a JOIN can take FILLING away, and a
		 * JOIN under a producer that is still writing is the invariant the
		 * old `raced` counter watched for -- counted, not assumed. */
		s = o->cs_enter();
		writing = (f->word == (uint8_t)NN_HO_FILLING);
		o->cs_exit(s);
		return writing ? NN_CORE_FR_WROTE : NN_CORE_FR_RACED;
	}

	/* After the input is written, never before. */
	if (!nn_cf_step(f, o, (uint8_t)NN_HO_OP_HAND))
		return NN_CORE_FR_RACED;
	o->infer_start(ctx);
	return NN_CORE_FR_HANDED;
}

int nn_core_frame_abandon(struct nn_core_frame *f,
                          const struct nn_core_frame_ops *o)
{
	return nn_cf_step(f, o, (uint8_t)NN_HO_OP_ABANDON);
}

/* ---- the worker ------------------------------------------------------------ */

int nn_core_frame_want(struct nn_core_frame *f,
                       const struct nn_core_frame_ops *o)
{
	return nn_cf_step(f, o, (uint8_t)NN_HO_OP_ARM);
}

int nn_core_frame_take(struct nn_core_frame *f,
                       const struct nn_core_frame_ops *o)
{
	return nn_cf_step(f, o, (uint8_t)NN_HO_OP_TAKE);
}

void nn_core_frame_done(struct nn_core_frame *f,
                        const struct nn_core_frame_ops *o, int more)
{
	(void)nn_cf_step(f, o, more ? (uint8_t)NN_HO_OP_DONE
	                            : (uint8_t)NN_HO_OP_DONE_LAST);
}

int nn_core_frame_discard(struct nn_core_frame *f,
                          const struct nn_core_frame_ops *o)
{
	int took;
	unsigned s = o->cs_enter();

	/* The worker's own two steps, in one section: nobody can see the job as
	 * RUNNING, because it is not going to run. */
	took = nn_handoff_step(&f->word, (uint8_t)NN_HO_OP_TAKE);
	if (took)
		(void)nn_handoff_step(&f->word, (uint8_t)NN_HO_OP_DONE_LAST);
	o->cs_exit(s);
	return took;
}

static int nn_cf_held(const struct nn_core_frame_ops *o)
{
	return o->lease_held != NULL && o->lease_held();
}

void nn_core_on_infer_done(struct nn_core_frame *f,
                           const struct nn_core_frame_ops *o, void *ctx,
                           uint32_t gen, int more)
{
	enum nn_core_done what;
	int n = 0, took = 0;
	/* Whether the board's worker took the lease: it is given back below. */
	const int leased = nn_cf_held(o);

	if (!o->is_plugin(ctx)) {
		/*
		 * [!] NOBODY DECODED THIS, AND THAT IS STILL PUBLISHED (issue
		 * #116), under the generation rule like any result: `nn run` waits
		 * on a counter the board bumps only for a publish that was taken.
		 */
		took = o->publish_raw(ctx, gen);
		what = NN_CORE_DONE_RAW;
	} else if (!leased) {
		/* [!] An entry check like the board's own: nothing was asked of
		 * the plugin, and the miss is counted where it was refused. */
		if (o->lease_note_unheld != NULL)
			o->lease_note_unheld();
		what = NN_CORE_DONE_NOT_HELD;
	} else if (!o->admits(ctx, gen)) {
		/* [!] THE GENERATION BEFORE THE DECODE, UNDER THE LEASE (issue
		 * #118): a boundary that landed during the inference means this
		 * frame cannot be published, and decoding it anyway would rewrite
		 * the plugin's result -- the account of what the record holds --
		 * for nothing. */
		what = NN_CORE_DONE_RETIRED;
	} else if (o->outputs != NULL && o->outputs(ctx) != 0) {
		what = NN_CORE_DONE_NO_OUTPUTS;
	} else if (o->decode(ctx, &n) != 0) {
		/* Not reachable while the hold above stands; the decode did not
		 * run, so nothing is published. */
		what = NN_CORE_DONE_NOT_HELD;
	} else {
		/*
		 * [!] PUBLISHED AT ONCE, WHATEVER IT SAYS (issue #118), negative
		 * counts included, and still inside the lease: a console that takes
		 * it next sees the record and the plugin's result describe the same
		 * frame.  The frame the plugin now holds is noted for the panel's
		 * lag whether or not the record took it -- it is what the plugin
		 * will paint.
		 */
		took = o->publish(ctx, n, gen);
		f->res_frame = f->job_frame;
		what = NN_CORE_DONE_DECODED;
	}
	if (leased)
		o->lease_give();
	o->account(ctx, what, n, took);
	/* After the outputs have been read for the last time, never before: the
	 * producer may write the input again from here. */
	nn_core_frame_done(f, o, more);
}

/* ---- the stop -------------------------------------------------------------- */

int nn_core_frame_join(struct nn_core_frame *f,
                       const struct nn_core_frame_ops *o)
{
	return nn_cf_step(f, o, (uint8_t)NN_HO_OP_JOIN);
}

void nn_core_frame_reset(struct nn_core_frame *f,
                         const struct nn_core_frame_ops *o, int stats)
{
	unsigned s = o->cs_enter();

	f->frame_no  = 0u;
	f->job_frame = 0u;
	f->res_frame = 0u;
	f->drawn     = 0u;
	f->drawn_lag = 0u;
	if (stats) {
		f->lag_sum = 0u;
		f->lag_n   = 0u;
		f->lag_max = 0u;
	}
	o->cs_exit(s);
}

/* ---- the panel ------------------------------------------------------------- */

/*
 * [!] A RESULT THIS SESSION PUBLISHED, AND THE PLUGIN'S (issues #110, #116,
 * #118).  The record keeps the last result across a stop, so `valid` alone would
 * open a new stream wearing the last one's boxes; a RAW_TENSORS record means an
 * inference ran that nothing decoded, and there is nothing to put on the
 * picture.
 *
 * NOT INLINED, AND THAT IS A STACK DECISION: the snapshot is popped before the
 * paint hook runs, so it is not in the depth a plugin's draw() is entered at.
 */
static __attribute__((noinline)) int
nn_cf_drawable(const struct nn_core_panel *p, void *ctx)
{
	struct nn_det_snapshot snap;

	memset(&snap, 0, sizeof snap);
	if (!p->record(ctx, &snap))
		return 0;
	return snap.valid != 0 && snap.current != 0u &&
	       snap.kind == (uint8_t)NN_DET_PLUGIN_REPORT;
}

enum nn_core_draw nn_core_draw(struct nn_core_frame *f,
                               const struct nn_core_frame_ops *o,
                               const struct nn_core_panel *p, void *ctx)
{
	enum nn_core_draw r = NN_CORE_DRAW_DECLINED;

	/* Asked once, before anything is held: with no plugin there is no lease
	 * to try and nothing to paint. */
	if (o->lease_try == NULL || !o->is_plugin(ctx))
		return NN_CORE_DRAW_NONE;
	/*
	 * [!] A TRY, NEVER A WAIT.  The panel outranks the worker and holds the
	 * picture; waiting here would hold it for as long as a decode takes.  A
	 * refusal shows the frame bare and is counted by the lease as a miss.
	 */
	if (!o->lease_try(PLUGIN_LEASE_PANEL))
		return NN_CORE_DRAW_MISSED;
	/* [!] Lease, THEN the frame lock -- the one order every holder uses. */
	if (p->frame_lock != NULL)
		p->frame_lock(ctx);

	if ((p->may_draw == NULL || p->may_draw(ctx)) && nn_cf_drawable(p, ctx)) {
		p->paint(ctx);
		/* How many frames behind the picture this result is (issue #129),
		 * read under the same hold the worker wrote it in. */
		f->drawn_lag = f->frame_no - f->res_frame;
		f->drawn     = 1u;
		r = NN_CORE_DRAW_PAINTED;
	}
	o->lease_give();
	return r;
}

void nn_core_on_present_done(struct nn_core_frame *f,
                             const struct nn_core_frame_ops *o)
{
	unsigned s;

	if (!f->drawn)
		return;
	s = o->cs_enter();
	f->lag_sum += f->drawn_lag;
	f->lag_n++;
	if (f->drawn_lag > f->lag_max)
		f->lag_max = f->drawn_lag;
	o->cs_exit(s);
	f->drawn = 0u;
}
