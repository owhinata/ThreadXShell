/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    nn_det_record.c
 * @brief   The published decode record and its generation rule.  See
 *          nn_det_record.h.
 */
#include "nn_det_record.h"

#include <stddef.h>
#include <string.h>

void nn_det_record_boundary(struct nn_det_record *r)
{
	if (r == NULL)
		return;
	/* The generation and nothing else: the result stays, and so does whether
	 * its decoder can still describe it -- a boundary runs no decoder. */
	r->gen++;
}

void nn_det_record_invalidate(struct nn_det_record *r)
{
	if (r == NULL)
		return;
	/*
	 * The generation moves FIRST, so that even an implementation which read it
	 * back before finishing the clear could not observe the old value paired
	 * with a cleared record.
	 */
	r->gen++;
	r->epoch++;
	r->ndet       = 0;
	r->valid      = 0;
	r->reportable = 0u;
	r->kind       = (uint8_t)NN_DET_CALLER_BOXES;
	memset(r->dets, 0, sizeof r->dets);
	memset(&r->res, 0, sizeof r->res);
	memset(&r->extra, 0, sizeof r->extra);   /* the old model's shapes too */
	/* `accepted` is NOT reset: a reader's base is a point on that count, and
	 * moving the count underneath it would turn "none since" into "some". */
}

uint32_t nn_det_record_gen(const struct nn_det_record *r)
{
	return r != NULL ? r->gen : 0u;
}

int nn_det_record_admits(const struct nn_det_record *r, uint32_t gen)
{
	/* The publish's own test, asked early -- the same rule, not a second one. */
	return (r != NULL && gen == r->gen) ? 1 : 0;
}

int nn_det_record_publish(struct nn_det_record *r, const struct bf_det *d, int n,
                          const struct bf_result *res, uint32_t gen,
                          const struct nn_top5 *top)
{
	int copy;

	if (r == NULL)
		return 0;
	/*
	 * [!] THE WHOLE POINT.  A decode that started under a session which has
	 * since been reset lands nowhere -- otherwise a stop, which cannot cancel
	 * an inference already running, would be followed by that inference
	 * resurrecting the stopped session's boxes.
	 */
	if (gen != r->gen)
		return 0;

	copy = n;
	if (copy > BF_MAX_DET)
		copy = BF_MAX_DET;
	if (copy > 0 && d != NULL)
		memcpy(r->dets, d, (size_t)copy * sizeof(*d));

	/* Negative stays negative AND STAYS ITSELF: "the decoder did not recognise
	 * this model" is not "no faces" (issue #57), and it is not "this firmware
	 * is wired wrong" either (issue #118) -- see the header. */
	r->ndet = (n < 0) ? n : copy;
	if (res != NULL)
		r->res = *res;
	else
		memset(&r->res, 0, sizeof r->res);
	r->kind       = (uint8_t)NN_DET_CALLER_BOXES;
	r->reportable = 0u;    /* nothing to ask: the boxes are all here */
	/* The classes go with THIS result or not at all: a previous result's must
	 * not be left beside it (issue #121). */
	memset(&r->extra, 0, sizeof r->extra);
	if (top != NULL) {
		r->extra.what  = (uint8_t)NN_EXTRA_TOP;
		r->extra.u.top = *top;
	}
	r->valid      = 1;
	r->pub_gen    = gen;
	r->accepted++;
	return 1;
}

int nn_det_record_publish_external(struct nn_det_record *r, int n, uint32_t gen)
{
	if (r == NULL)
		return 0;
	if (gen != r->gen) {
		/* The same rule, for the same reason -- but this decode HAS run, and
		 * the plugin's account now describes it rather than what the record
		 * holds.  The count stays; the account is withheld (issue #118). */
		r->reportable = 0u;
		return 0;
	}

	/* Neither clamped nor normalised -- see the header.  The boxes and the
	 * diagnostics are not this decoder's to describe, so the stale ones from
	 * whatever ran before must not be left standing beside the new count. */
	r->ndet  = n;
	memset(r->dets, 0, sizeof r->dets);
	memset(&r->res, 0, sizeof r->res);
	r->kind       = (uint8_t)NN_DET_PLUGIN_REPORT;
	r->reportable = 1u;
	/* The plugin describes itself; the union's contents mean nothing once
	 * this says NONE, and clearing all of it would put ~300 B of stores in the
	 * producer's interrupt-disabled publish on one board. */
	r->extra.what = (uint8_t)NN_EXTRA_NONE;
	r->valid      = 1;
	r->pub_gen    = gen;
	r->accepted++;
	return 1;
}

int nn_det_record_publish_raw(struct nn_det_record *r, uint32_t gen,
                              const struct nn_raw_outputs *raw)
{
	unsigned i, n;

	if (r == NULL)
		return 0;
	if (gen != r->gen)
		return 0;          /* the same rule, for the same reason */

	/* Zero because there is nothing to count, not because a decoder found
	 * nothing -- see the header.  The boxes and the diagnostics describe a
	 * decoder that did not run, so whatever ran before this does not get to
	 * stand beside the inference that did. */
	r->ndet  = 0;
	memset(r->dets, 0, sizeof r->dets);
	memset(&r->res, 0, sizeof r->res);
	r->kind       = (uint8_t)NN_DET_RAW_TENSORS;
	r->reportable = 0u;    /* no decoder, so nobody to ask */
	/* The descriptors of THIS inference's model, with no way back into its
	 * buffers -- see nn_raw_outputs (issue #121). */
	memset(&r->extra, 0, sizeof r->extra);
	if (raw != NULL) {
		n = raw->n;
		if (n > NN_RAW_OUTPUTS_MAX)
			n = NN_RAW_OUTPUTS_MAX;
		r->extra.what        = (uint8_t)NN_EXTRA_RAW;
		r->extra.u.raw.count = raw->count;
		r->extra.u.raw.n     = (uint8_t)n;
		for (i = 0u; i < n; i++) {
			r->extra.u.raw.out[i]      = raw->out[i];
			r->extra.u.raw.out[i].data = NULL;
		}
	}
	r->valid      = 1;
	r->pub_gen    = gen;
	r->accepted++;
	return 1;
}

void nn_det_record_snapshot(const struct nn_det_record *r,
                            struct nn_det_snapshot *out,
                            struct bf_det *dets, int max)
{
	int n;

	if (out == NULL)
		return;
	if (r == NULL) {
		memset(out, 0, sizeof(*out));
		return;
	}
	out->valid      = r->valid;
	out->ndet       = r->ndet;
	out->res        = r->res;
	/* From the record, not asserted -- see the header. */
	out->kind       = r->kind;
	/* In the same breath as the result they describe (issue #118): a reader
	 * that took these separately could pair one publish's count with
	 * another's result. */
	out->reportable = r->reportable;
	out->accepted   = r->accepted;
	out->epoch      = r->epoch;
	out->current    = (r->valid != 0 && r->pub_gen == r->gen) ? 1u : 0u;
	/* [!] ENUMERATED, NOT NEGATED.  "Copy when it says caller boxes" leaves a
	   kind nobody has written yet alone; "copy unless it says plugin" would
	   hand that kind the previous decoder's boxes. */
	if (r->kind == (uint8_t)NN_DET_CALLER_BOXES && dets != NULL && max > 0) {
		n = r->ndet;
		if (n > max)
			n = max;
		if (n > 0)
			memcpy(dets, r->dets, (size_t)n * sizeof(*dets));
	}
}

int nn_det_last_valid(const struct nn_det_snapshot *s, uint32_t base)
{
	if (s == NULL)
		return 0;
	/* Both halves -- see the header.  Inequality, not order: the count wraps. */
	return (s->valid != 0 && s->accepted != base) ? 1 : 0;
}

void nn_det_record_extra(const struct nn_det_record *r,
                         struct nn_result_extra *out)
{
	if (out == NULL)
		return;
	if (r == NULL) {
		memset(out, 0, sizeof *out);
		return;
	}
	*out = r->extra;
}
