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
	/* Every member, this one included -- see the header for why leaving it to
	   the caller's memset stopped being safe once there was a third kind. */
	out->kind  = (uint8_t)NN_DET_CALLER_BOXES;
	if (dets != NULL && max > 0) {
		n = r->ndet;
		if (n > max)
			n = max;
		if (n > 0)
			memcpy(dets, r->dets, (size_t)n * sizeof(*dets));
	}
}
