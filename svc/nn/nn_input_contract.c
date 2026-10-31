/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    nn_input_contract.c
 * @brief   The admission test for a camera-filled input tensor.  See
 *          nn_input_contract.h.
 *
 * Owns no storage (the shared-TU audit holds it to that), includes no HAL, no
 * ThreadX and no libm: a pure function of the tensor descriptor it is handed.
 *
 * WHY IT TAKES `struct nn_tensor` AND NOT THE INDIVIDUAL VALUES.  Every one of the
 * ten items is a field of that one descriptor, which is what nn_input() hands the
 * board.  Taking the fields one by one would let a caller pass a different copy
 * of one of them (a cached height, the previous model's scale) and have it
 * admitted against the wrong tensor; taking the descriptor, the test and the
 * tensor cannot disagree.  A host test builds one as easily as the values.
 */
#include "nn_input_contract.h"

/*
 * Is @p f a positive normal float -- not zero, not negative, not subnormal, not
 * infinite, not NaN?  Read from the bits rather than with isnormal(): svc/ links
 * no libm, and a comparison chain (f >= FLT_MIN && f <= FLT_MAX) would depend on
 * the compiler treating NaN as unordered, which it does, but the bit test says
 * the same thing with nothing to reason about.  IEEE 754 binary32: sign bit 31,
 * exponent bits 30..23; normal <=> exponent neither 0 nor 255.
 */
static int nn_input_scale_ok(float f)
{
	union {
		float    f;
		uint32_t u;
	} b;
	uint32_t exp;

	b.f = f;
	if ((b.u >> 31) != 0u)
		return 0;
	exp = (b.u >> 23) & 0xFFu;
	return exp != 0u && exp != 0xFFu;
}

int nn_input_admit(const struct nn_tensor *in)
{
	uint64_t need;
	uint32_t elem;

	if (in == (const struct nn_tensor *)0 || in->data == (void *)0)
		return NN_INPUT_ERR_ABSENT;
	if (in->dtype == (uint8_t)NN_DTYPE_FLOAT32)
		elem = 4u;
	else if (in->dtype == (uint8_t)NN_DTYPE_INT8)
		elem = 1u;
	else
		return NN_INPUT_ERR_DTYPE;
	if (in->ndim != 4u)
		return NN_INPUT_ERR_RANK;
	if (in->dims[0] != 1u)
		return NN_INPUT_ERR_BATCH;
	if (in->dims[1] == 0u || in->dims[2] == 0u)
		return NN_INPUT_ERR_EMPTY;
	if (in->dims[3] != 1u && in->dims[3] != 3u)
		return NN_INPUT_ERR_CHANNELS;
	if (elem == 1u) {
		/* A per-axis quantised tensor arrives with scale 0: its real
		   parameters are in a structure nn_tensor does not carry, so
		   quantising with any assumed scale would be silently wrong. */
		if (!nn_input_scale_ok(in->scale))
			return NN_INPUT_ERR_SCALE;
		if (in->zero_point < -128 || in->zero_point > 127)
			return NN_INPUT_ERR_ZERO_POINT;
	}
	/* [!] A float32 buffer must be 4-byte aligned: the producer stores each
	   element with VSTR, which takes a UsageFault on an unaligned address on
	   the Cortex-M7.  Every backend aligns its tensors to 16 or 32 bytes today;
	   this check is here so a backend that did not is refused, not faulted. */
	if (elem == 4u && ((uintptr_t)in->data & 3u) != 0u)
		return NN_INPUT_ERR_ALIGN;
	/* 64 bits: dims are 16-bit, so H * W * C * 4 < 2^50 cannot wrap here,
	   while in 32 bits it can and would pass a short buffer. */
	need = (uint64_t)in->dims[1] * in->dims[2] * in->dims[3] * elem;
	if (need > (uint64_t)in->bytes)
		return NN_INPUT_ERR_BYTES;
	return NN_INPUT_OK;
}

const char *nn_input_refusal_name(int code)
{
	switch (code) {
	case NN_INPUT_OK:             return "ok";
	case NN_INPUT_ERR_ABSENT:     return "no input tensor or buffer";
	case NN_INPUT_ERR_DTYPE:      return "input type is not float32 or int8";
	case NN_INPUT_ERR_RANK:       return "input is not 4-dimensional";
	case NN_INPUT_ERR_BATCH:      return "input batch is not 1";
	case NN_INPUT_ERR_EMPTY:      return "input height or width is 0";
	case NN_INPUT_ERR_CHANNELS:   return "input channels are not 1 or 3";
	case NN_INPUT_ERR_SCALE:      return "int8 input scale is not a positive normal number";
	case NN_INPUT_ERR_ZERO_POINT: return "int8 input zero point is outside -128..127";
	case NN_INPUT_ERR_ALIGN:      return "float32 input buffer is not 4-byte aligned";
	case NN_INPUT_ERR_BYTES:      return "input buffer is shorter than its shape";
	default:                      return "unknown";
	}
}
