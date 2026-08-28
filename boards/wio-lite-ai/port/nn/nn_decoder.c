/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    nn_decoder.c
 * @brief   nn_tensor -> tensor_desc, plus this board's decoder state.  See
 *          nn_decoder.h.
 */
#include "nn_decoder.h"

#include <stddef.h>
#include <string.h>

#include "mem_sections.h"

/*
 * The candidate scratch, in the PSRAM AI carve-out it has always been in.
 *
 * [!] ONLY THE SCRATCH GOES HERE.  `.psram_ai` is NOLOAD, so an object with
 * initialised fields placed in it is never loaded -- and NOLOAD keeps whatever
 * the previous run left, so it would fail by appearing to work.  The decoder's
 * state therefore stays in ordinary internal RAM below, which also means
 * `ai thresh` still answers when the PSRAM bring-up failed (it is fail-soft, and
 * the shell runs without it).
 *
 * Write-before-read on every decode and CPU-only, which is what made the
 * carve-out appropriate in the first place; the two anchor tables that used to
 * sit beside it are gone, since the shared decoder computes the centres.
 */
static struct bf_cand   nn_dec_scratch[BF_MAX_CAND] PSRAM_AI __attribute__((aligned(32)));

/* Ordinary .bss.  Deliberately NOT PSRAM_AI -- see above. */
static struct blazeface nn_dec;
static int              nn_dec_ready;

/*
 * Bind on first use.
 *
 * A lazy flag is safe HERE in a way it would not be inside the shared decoder:
 * every path that can reach a decode is serialised by this board's NN session,
 * and `ai thresh` only touches a single word that is atomic by construction.
 */
static int nn_decoder_bind(void)
{
	int rc;

	if (nn_dec_ready)
		return BF_OK;
	rc = blazeface_init(&nn_dec, nn_dec_scratch, sizeof nn_dec_scratch);
	if (rc != BF_OK)
		return rc;
	nn_dec_ready = 1;
	return BF_OK;
}

/* enum nn_dtype -> enum tensor_dtype.  Anything this decoder cannot read maps to
 * UNSUPPORTED so it is refused rather than read as some other type. */
static uint8_t nn_dtype_to_tensor(uint8_t dtype)
{
	switch (dtype) {
	case NN_DTYPE_INT8:    return (uint8_t)TENSOR_DTYPE_INT8;
	case NN_DTYPE_UINT8:   return (uint8_t)TENSOR_DTYPE_UINT8;
	case NN_DTYPE_INT16:   return (uint8_t)TENSOR_DTYPE_INT16;
	case NN_DTYPE_INT32:   return (uint8_t)TENSOR_DTYPE_INT32;
	case NN_DTYPE_FLOAT32: return (uint8_t)TENSOR_DTYPE_FLOAT32;
	default:               return (uint8_t)TENSOR_DTYPE_UNSUPPORTED;
	}
}

/*
 * Translate one output tensor.
 *
 * [!] A RANK ABOVE FOUR IS REFUSED, NOT TRUNCATED.  A shortened shape can still
 * match a lookup, and then the decoder reads a tensor it was not looking for
 * while every check it makes passes.  Rank 0 says "not representable" and every
 * shape test downstream fails on it.
 */
static void nn_desc_from_tensor(struct tensor_desc *d, const struct nn_tensor *t)
{
	memset(d, 0, sizeof(*d));
	d->data  = t->data;
	d->bytes = t->bytes;
	if (t->ndim <= TENSOR_MAX_DIMS) {
		d->rank = t->ndim;
		for (unsigned i = 0; i < t->ndim; i++)
			d->dims[i] = (int32_t)t->dims[i];
	}
	d->dtype      = nn_dtype_to_tensor(t->dtype);
	d->scale      = t->scale;
	d->zero_point = t->zero_point;
}

int nn_decoder_run(struct nn_model *m, struct bf_det *out, int max,
                   struct bf_result *res)
{
	struct tensor_desc desc[NN_MAX_IO];
	int n_out, i;
	int rc;

	rc = nn_decoder_bind();
	if (rc != BF_OK)
		goto fail;
	if (m == NULL) {
		rc = BF_ERR_ARG;
		goto fail;
	}

	n_out = nn_output_count(m);
	if (n_out < 0)
		n_out = 0;
	if (n_out > NN_MAX_IO)
		n_out = NN_MAX_IO;
	for (i = 0; i < n_out; i++) {
		struct nn_tensor *t = nn_output(m, i);

		if (t == NULL) {
			/* A hole in the output set is not a model-shape problem and
			 * must not be reported as one; leaving it UNSUPPORTED lets
			 * the decoder decide whether the four it wants are still
			 * there, which is the same answer by a defensible route. */
			memset(&desc[i], 0, sizeof desc[i]);
			continue;
		}
		nn_desc_from_tensor(&desc[i], t);
	}
	return blazeface_decode(&nn_dec, desc, (unsigned)n_out, out, max, res);

fail:
	if (res != NULL) {
		memset(res, 0, sizeof(*res));
		res->status = rc;
	}
	return rc;
}

int nn_decoder_set_thresh_milli(unsigned milli)
{
	int rc = nn_decoder_bind();

	if (rc != BF_OK)
		return rc;
	return blazeface_set_thresh_milli(&nn_dec, milli);
}

unsigned nn_decoder_get_thresh_milli(void)
{
	if (nn_decoder_bind() != BF_OK)
		return 0u;
	return blazeface_get_thresh_milli(&nn_dec);
}
