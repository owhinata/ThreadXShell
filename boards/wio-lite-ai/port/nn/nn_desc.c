/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    nn_desc.c
 * @brief   nn_tensor -> tensor_desc.  See nn_desc.h.
 */
#include "nn_desc.h"

#include <string.h>

/* enum nn_dtype -> enum tensor_dtype.  Anything a reader cannot read maps to
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
 * match a lookup, and then the reader takes a tensor it was not looking for
 * while every check it makes passes.  Rank 0 says "not representable" and every
 * shape test downstream fails on it.
 *
 * [!] AND THE DESTINATION IS CLEARED FIRST.  These land in a caller's array that
 * is reused across models and across frames; a member this function stopped
 * writing would silently keep the previous tensor's value, and a stale scale is
 * not an obvious failure -- it is a plausible number.
 */
void nn_desc_of(struct tensor_desc *d, const struct nn_tensor *t)
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
