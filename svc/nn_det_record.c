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

void nn_det_record_reset(struct nn_det_record *r)
{
	if (r == NULL)
		return;
	/*
	 * The generation moves FIRST, so that even an implementation which read it
	 * back before finishing the clear could not observe the old value paired
	 * with a cleared record.
	 */
	r->gen++;
	r->ndet  = 0;
	r->valid = 0;
	r->kind  = (uint8_t)NN_DET_CALLER_BOXES;
	memset(&r->res, 0, sizeof r->res);
}

uint32_t nn_det_record_gen(const struct nn_det_record *r)
{
	return r != NULL ? r->gen : 0u;
}

int nn_det_record_publish(struct nn_det_record *r, const struct bf_det *d, int n,
                          const struct bf_result *res, uint32_t gen)
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

	/* Negative stays negative: "the decoder did not recognise this model" is
	 * not "no faces" (issue #57). */
	r->ndet = (n < 0) ? -1 : copy;
	if (res != NULL)
		r->res = *res;
	else
		memset(&r->res, 0, sizeof r->res);
	r->kind  = (uint8_t)NN_DET_CALLER_BOXES;
	r->valid = 1;
	return 1;
}

int nn_det_record_publish_external(struct nn_det_record *r, int n, uint32_t gen)
{
	if (r == NULL)
		return 0;
	if (gen != r->gen)
		return 0;          /* the same rule, for the same reason */

	/* Neither clamped nor normalised -- see the header.  The boxes and the
	 * diagnostics are not this decoder's to describe, so the stale ones from
	 * whatever ran before must not be left standing beside the new count. */
	r->ndet  = n;
	memset(r->dets, 0, sizeof r->dets);
	memset(&r->res, 0, sizeof r->res);
	r->kind  = (uint8_t)NN_DET_PLUGIN_REPORT;
	r->valid = 1;
	return 1;
}

int nn_det_record_publish_raw(struct nn_det_record *r, uint32_t gen)
{
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
	r->kind  = (uint8_t)NN_DET_RAW_TENSORS;
	r->valid = 1;
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
	out->valid = r->valid;
	out->ndet  = r->ndet;
	out->res   = r->res;
	/* From the record, not asserted -- see the header. */
	out->kind  = r->kind;
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
