/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    nn_top.c
 * @brief   Top classes of an output tensor.  See nn_top.h.
 *
 * By insertion -- N is small and the vector is a class count, so nothing
 * cleverer earns its code size.  Moved here unchanged in its arithmetic from
 * shell/cmds/cmd_nn.c (issue #121).
 */
#include "nn_top.h"

#include <stddef.h>
#include <string.h>

void nn_top_of(const struct tensor_desc *t, struct nn_top5 *out)
{
	uint32_t esz, count, i;
	unsigned k;

	if (out == NULL)
		return;
	memset(out, 0, sizeof *out);
	if (t == NULL || t->data == NULL) {
		out->status = (uint8_t)NN_TOP_NO_OUTPUT;
		return;
	}
	out->dtype = t->dtype;
	switch (t->dtype) {
	case TENSOR_DTYPE_INT8:
	case TENSOR_DTYPE_UINT8:
		esz = 1u;
		break;
	case TENSOR_DTYPE_FLOAT32:
		esz = 4u;
		break;
	default:
		out->status = (uint8_t)NN_TOP_NO_STRIDE;
		return;
	}
	out->integer_scored = (t->dtype != TENSOR_DTYPE_FLOAT32) ? 1u : 0u;
	count = (uint32_t)(t->bytes / esz);
	out->count = count;

	for (k = 0u; k < NN_TOP_N; k++) {
		out->idx[k] = -1;
		out->v[k]   = -1e30f;
		out->raw[k] = 0;
	}
	for (i = 0u; i < count; i++) {
		int32_t raw;
		float v;

		switch (t->dtype) {
		case TENSOR_DTYPE_INT8:
			raw = ((const int8_t *)t->data)[i];
			v = ((float)raw - (float)t->zero_point) * t->scale;
			break;
		case TENSOR_DTYPE_UINT8:
			raw = ((const uint8_t *)t->data)[i];
			v = ((float)raw - (float)t->zero_point) * t->scale;
			break;
		default:              /* FLOAT32: no affine, see svc/tensor.h */
			raw = 0;
			v = ((const float *)t->data)[i];
			break;
		}
		for (k = 0u; k < NN_TOP_N; k++) {
			if (v > out->v[k]) {
				unsigned j;

				for (j = NN_TOP_N - 1u; j > k; j--) {
					out->v[j]   = out->v[j - 1u];
					out->idx[j] = out->idx[j - 1u];
					out->raw[j] = out->raw[j - 1u];
				}
				out->v[k]   = v;
				out->idx[k] = (int32_t)i;
				out->raw[k] = raw;
				if (out->n < NN_TOP_N)
					out->n++;
				break;
			}
		}
	}
	out->status = (uint8_t)NN_TOP_OK;
}
