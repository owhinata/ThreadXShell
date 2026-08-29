/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    nn_decoder.c
 * @brief   npu_tensor -> tensor_desc, plus this board's decoder state.  See
 *          nn_decoder.h.
 */
#include "nn_decoder.h"

#include <stddef.h>
#include <string.h>

#define LOG_TAG "npu"
#include "log.h"

/*
 * The decoder's state and its candidate scratch.
 *
 * PLAIN .bss, both of them, which is where the Grove copy of the decoder already
 * kept its candidate array: it is written before it is read on every decode and
 * the CPU is its only user, so it wants neither a carve-out nor NOLOAD.  The two
 * are separate objects rather than one because the shared decoder's contract
 * says so -- on the other two boards the scratch goes in a NOLOAD carve-out and
 * the state must not follow it there.
 */
static struct bf_cand   nn_dec_scratch[BF_MAX_CAND];
static struct blazeface nn_dec;
static int              nn_dec_ready;

/*
 * Bind on first use.
 *
 * A lazy flag is safe HERE in a way it would not be inside the shared decoder:
 * both callers hold `nn_busy` (cmd_nn.c) for the whole of any operation that can
 * reach a decode, so this runs serialised.  Failure is logged once and reported
 * as BF_ERR_UNINIT rather than swallowed -- it would mean the build is wrong, not
 * that the model is.
 */
static int nn_decoder_bind(void)
{
	int rc;

	if (nn_dec_ready)
		return BF_OK;
	rc = blazeface_init(&nn_dec, nn_dec_scratch, sizeof nn_dec_scratch);
	if (rc != BF_OK) {
		LOG_ERR("decoder init failed (%d)", rc);
		return rc;
	}
	nn_dec_ready = 1;
	return BF_OK;
}

/*
 * Translate one NPU tensor into the neutral descriptor.
 *
 * [!] AN UNTRANSLATABLE TENSOR IS MARKED UNSUPPORTED, NOT GUESSED AT.  This
 * board's models are int8 throughout -- the op resolver carries AddEthosU() and
 * nothing else, so a graph that reached the NPU is fully quantised -- and
 * npu_tensor_is_int8() is the only type predicate the C++ side exports.  Anything
 * else therefore gets TENSOR_DTYPE_UNSUPPORTED, which makes the decoder refuse
 * the tensor instead of reading its bytes as a type they are not.
 *
 * The rank is passed through unchanged: npu_tflm.cc already refuses to truncate a
 * higher-rank tensor into four dimensions, so a rank of 0 arriving here means
 * "not representable", and every shape test downstream fails on it.
 */
void nn_decoder_desc(struct tensor_desc *d, const struct npu_tensor *t)
{
	memset(d, 0, sizeof(*d));
	d->data  = t->data;
	d->bytes = t->bytes;
	d->rank  = t->rank;
	for (unsigned i = 0; i < TENSOR_MAX_DIMS; i++)
		d->dims[i] = t->dims[i];
	d->dtype      = npu_tensor_is_int8(t->type) ? (uint8_t)TENSOR_DTYPE_INT8
	                                            : (uint8_t)TENSOR_DTYPE_UNSUPPORTED;
	d->scale      = t->scale;
	d->zero_point = t->zero_point;
}

/* Translate a whole output set.  Returns how many were written. */
static unsigned nn_desc_all(struct tensor_desc *d, unsigned cap,
                            const struct npu_tensor *outs, unsigned n)
{
	unsigned i;

	if (n > cap)
		n = cap;
	for (i = 0; i < n; i++)
		nn_decoder_desc(&d[i], &outs[i]);
	return i;
}

int nn_decoder_shapes_ok(const struct npu_tensor *outs, unsigned n)
{
	struct tensor_desc desc[NN_DECODER_MAX_OUTPUTS];
	unsigned m;

	if (outs == NULL)
		return 0;
	m = nn_desc_all(desc, (unsigned)NN_DECODER_MAX_OUTPUTS, outs, n);
	return blazeface_shapes_ok(desc, m);
}

int nn_decoder_run(const struct npu_tensor *outs, unsigned n,
                   struct bf_det *out, int max, struct bf_result *res)
{
	struct tensor_desc desc[NN_DECODER_MAX_OUTPUTS];
	unsigned m;
	int rc;

	rc = nn_decoder_bind();
	if (rc != BF_OK) {
		if (res != NULL) {
			memset(res, 0, sizeof(*res));
			res->status = rc;
		}
		return rc;
	}
	if (outs == NULL) {
		if (res != NULL) {
			memset(res, 0, sizeof(*res));
			res->status = BF_ERR_ARG;
		}
		return BF_ERR_ARG;
	}
	m = nn_desc_all(desc, (unsigned)NN_DECODER_MAX_OUTPUTS, outs, n);
	return blazeface_decode(&nn_dec, desc, m, out, max, res);
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
