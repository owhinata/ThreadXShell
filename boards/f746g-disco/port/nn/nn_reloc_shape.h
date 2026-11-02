/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    nn_reloc_shape.h
 * @brief   stedgeai_reloc backend: an ai_buffer's shape -> struct nn_tensor's
 * dims / ndim / bytes (issue #176).
 *
 * The half of reloc_map() (nn_stedgeai_reloc.c) that needs no ST header, split out
 * so test/test_reloc_shape.c holds it to the boundaries the firmware cannot be made
 * to show (no model on hand has a dimension above 65535).  Pure, state-free, no
 * storage: the caller reads B / H / W / C and the element bits out of the ai_buffer
 * and passes them in; everything that needs ai_buffer itself (data pointer, dtype,
 * quantization) stays in reloc_map().
 *
 * The mapping is the one reloc_map() has always done: the legacy runtime stores
 * every shape as 4-D BCWH and a 0 element means "absent", so each 0 becomes 1
 * before anything else; W <= 1 is the 3-D logical tensor [B,H,C] (dims[3] = 1,
 * ndim 3), anything wider the 4-D NHWC [B,H,W,C]; bytes = B*H*W*C * (bits / 8).
 *
 * Refusals, each its own code (no code folded into another):
 *   - NN_MODEL_ERR_TENSOR  bits == 0: no element size.  (A NULL data pointer is the
 *                          same refusal; reloc_map() checks it, the pointer is not
 *                          an input here.)
 *   - NN_MODEL_ERR_DIMS    a dimension, after the 0 -> 1 replacement, that the 16-bit
 *                          descriptor cannot hold.  The rule is nn_tensor_dim_fits()
 *                          (svc/nn/nn.h), its one statement; it takes int32_t, so a
 *                          value above INT32_MAX is refused here first rather than
 *                          converted (it would turn negative, which is refused too,
 *                          but by an implementation-defined conversion).
 *   - NN_MODEL_ERR_ARENA   bytes above @p cap, the region the buffer lives in.  The
 *                          product is formed in 64 bits and stops as soon as it
 *                          passes @p cap, so it neither wraps 32 bits nor overflows
 *                          64 (each factor is <= 65535 once DIMS has passed and
 *                          bits / 8 <= 15, and the running product is <= cap <
 *                          2^32 before each multiply).  The caller passes the
 *                          capacity; nn_stedgeai_reloc.c says why it is the
 *                          activation arena.
 * DIMS is decided before ARENA: a dimension the descriptor cannot hold is that
 * answer whatever the size.  Sub-byte elements (bits 1..7) keep bytes == 0, as
 * before: bits / 8 is the element size this mapping has always used.
 */
#ifndef NN_RELOC_SHAPE_H
#define NN_RELOC_SHAPE_H

#include <stdint.h>

#include "nn.h"   /* NN_MAX_DIMS, NN_MODEL_ERR_*, nn_tensor_dim_fits() */

/** The shape half of one nn_tensor, written only on success. */
struct nn_reloc_shape {
	uint16_t dims[NN_MAX_DIMS];
	uint8_t  ndim;
	uint32_t bytes;
};

/* One dimension, after the 0 -> 1 replacement: does the descriptor hold it? */
static inline int nn_reloc_dim_ok(uint32_t d)
{
	if (d > (uint32_t)INT32_MAX)
		return 0;
	return nn_tensor_dim_fits((int32_t)d);
}

/**
 * Map one legacy-runtime shape.  Returns 0 and fills @p out, or NN_MODEL_ERR_TENSOR
 * / NN_MODEL_ERR_DIMS / NN_MODEL_ERR_ARENA (see the file comment) with @p out
 * untouched.
 */
static inline int nn_reloc_shape_map(uint32_t B, uint32_t H, uint32_t W, uint32_t C,
                                     uint32_t bits, uint32_t cap,
                                     struct nn_reloc_shape *out)
{
	const uint32_t f[4] = { B ? B : 1u, H ? H : 1u, W ? W : 1u, C ? C : 1u };
	uint32_t       esz;
	uint64_t       bytes;
	unsigned       i;

	if (bits == 0u)
		return NN_MODEL_ERR_TENSOR;
	for (i = 0; i < 4u; i++)
		if (!nn_reloc_dim_ok(f[i]))
			return NN_MODEL_ERR_DIMS;

	esz = bits / 8u;
	bytes = esz;
	if (esz != 0u) {
		for (i = 0; i < 4u; i++) {
			bytes *= f[i];
			if (bytes > cap)
				return NN_MODEL_ERR_ARENA;
		}
	}

	if (f[2] <= 1u) {                    /* 3-D logical tensor [B,H,C] */
		out->ndim    = 3;
		out->dims[0] = (uint16_t)f[0];
		out->dims[1] = (uint16_t)f[1];
		out->dims[2] = (uint16_t)f[3];
		out->dims[3] = 1;
	} else {                             /* 4-D NHWC [B,H,W,C] */
		out->ndim    = 4;
		out->dims[0] = (uint16_t)f[0];
		out->dims[1] = (uint16_t)f[1];
		out->dims[2] = (uint16_t)f[2];
		out->dims[3] = (uint16_t)f[3];
	}
	out->bytes = (uint32_t)bytes;
	return 0;
}

#endif /* NN_RELOC_SHAPE_H */
