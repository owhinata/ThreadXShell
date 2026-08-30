/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    npu_desc.c
 * @brief   npu_tensor -> tensor_desc.  See npu_desc.h.
 */
#include "npu_desc.h"

#include <string.h>

/*
 * [!] AN UNTRANSLATABLE TENSOR IS MARKED UNSUPPORTED, NOT GUESSED AT.  This
 * board's models are int8 throughout -- the op resolver carries AddEthosU() and
 * nothing else, so a graph that reached the NPU is fully quantised -- and
 * npu_tensor_is_int8() is the only type predicate the C++ side exports.
 * Anything else therefore gets TENSOR_DTYPE_UNSUPPORTED, which makes a reader
 * refuse the tensor instead of reading its bytes as a type they are not.
 *
 * The rank is passed through unchanged: npu_tflm.cc already refuses to truncate
 * a higher-rank tensor into four dimensions, so a rank of 0 arriving here means
 * "not representable", and every shape test downstream fails on it.
 */
void npu_desc_of(struct tensor_desc *d, const struct npu_tensor *t)
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
