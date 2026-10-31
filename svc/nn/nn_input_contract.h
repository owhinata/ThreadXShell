/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    nn_input_contract.h
 * @brief   Camera pixel -> model input tensor: which input tensors a camera can
 *          fill, and the value one 8-bit colour channel becomes (issue #162).
 *
 * WHO USES IT: wio-lite-ai and f746g-disco, whose producers write camera pixels
 * straight into the TFLM (or null / stedgeai) input tensor.  How a board samples
 * its pixels -- wio's band-by-band downsample, f746's whole-frame nearest
 * neighbour, the RGB565 -> 8-bit expansion -- stays in the board; what a sampled
 * 8-bit value turns into, and whether the tensor can take it at all, is here,
 * once.  grove-vision-ai-v2 does not use this: its NPU path has its own fixed
 * pixel transform and its own admission, and its containers assume them.
 *
 * TWO HALVES:
 *
 *   - nn_input_admit() (nn_input_contract.c) is the admission test, a pure
 *     function a board calls once, after the model is open and before it touches
 *     the camera or a plugin.  Each refusal has its own code; none is folded
 *     into another.  Board-specific conditions (wio's band tiling) come after it,
 *     in the board.
 *   - nn_input_f32() / nn_input_i8() below are the per-element conversion, inline
 *     because the producer calls them in its innermost loop.  They own nothing and
 *     read nothing but their arguments.
 *
 * [!] THE NORMALISATION IS ONE EXPLICIT FUSED MULTIPLY-ADD (__builtin_fmaf), NOT
 * `v * gain + bias` LEFT TO THE COMPILER.  Whether the compiler contracts that
 * expression into one rounding depends on -ffp-contract and the -std it implies;
 * the target (Cortex-M7, FPv5 VFMA) and the host tests would then compute
 * different floats from the same source.  Spelled as fmaf it is one rounding
 * everywhere: VFMA.F32 on the target, the C library's correctly rounded fmaf()
 * on the host.  This is also exactly what wio-lite-ai's producer compiled to
 * before issue #162 (VFMA.F32 with the bias as the accumulator).
 *
 * [!] fmaf PINS ONLY THAT ONE STEP.  The int8 path's `* inv_scale` followed by
 * the +-0.5 addition could still be contracted into a VFMA under gnu17's default
 * -ffp-contract=fast.  It is not today only because the range limit's compares
 * sit between the two -- that is incidental, not guaranteed.  The disassembly
 * check of each producer that adopts this (issue #162 commits 3 and 5) must
 * confirm there is no VFMA with 0.5 as an operand.
 *
 * [!] FLOATING-POINT ENVIRONMENT ASSUMED: round to nearest, subnormals not
 * flushed to zero -- the FPSCR reset defaults.  Nothing in either firmware writes
 * FPSCR or FPDSCR (issue #162 plan); a host test runs in the same mode.
 */
#ifndef NN_INPUT_CONTRACT_H
#define NN_INPUT_CONTRACT_H

#include <stdint.h>

#include "nn.h"   /* struct nn_tensor, enum nn_dtype */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Why nn_input_admit() refused an input tensor.  0 is "a camera can fill it";
 * every refusal is its own negative value, in the order the test checks them
 * (the first that applies is the one returned).
 */
enum nn_input_refusal {
	NN_INPUT_OK            =  0,
	NN_INPUT_ERR_ABSENT    = -1,  /**< no input tensor, or it has no buffer       */
	NN_INPUT_ERR_DTYPE     = -2,  /**< neither float32 nor int8                   */
	NN_INPUT_ERR_RANK      = -3,  /**< not 4 dimensions (N, H, W, C)              */
	NN_INPUT_ERR_BATCH     = -4,  /**< N is not 1                                 */
	NN_INPUT_ERR_EMPTY     = -5,  /**< H or W is 0                                */
	NN_INPUT_ERR_CHANNELS  = -6,  /**< C is neither 1 (red only) nor 3 (RGB)      */
	NN_INPUT_ERR_SCALE     = -7,  /**< int8 scale is not a positive normal float  */
	NN_INPUT_ERR_ZERO_POINT = -8, /**< int8 zero point outside [-128, 127]        */
	NN_INPUT_ERR_ALIGN     = -9,  /**< float32 buffer not 4-byte aligned          */
	NN_INPUT_ERR_BYTES     = -10, /**< buffer shorter than H x W x C elements     */
};

/**
 * Can a camera fill @p in?  Returns NN_INPUT_OK or one enum nn_input_refusal.
 * Pure: reads only @p in, keeps nothing.
 *
 * What it guarantees on NN_INPUT_OK, which the conversion below relies on:
 *   - dtype is NN_DTYPE_FLOAT32 or NN_DTYPE_INT8;
 *   - ndim == 4, dims[0] == 1, dims[1] (H) and dims[2] (W) non-zero, dims[3] (C)
 *     is 1 or 3;
 *   - for int8: scale is a positive NORMAL float (so 1/scale is finite and
 *     non-zero) and zero_point is in [-128, 127];
 *   - data is non-NULL, 4-byte aligned for float32, and bytes >= H * W * C *
 *     element size, computed in 64 bits (a product that wraps 32 bits is
 *     refused, not passed).
 */
int nn_input_admit(const struct nn_tensor *in);

/**
 * A short name for @p code (one of enum nn_input_refusal), for the board's
 * message; "unknown" for any other value.  Never NULL.
 */
const char *nn_input_refusal_name(int code);

/* ------------------------------------------------- one element, one channel -- */

/**
 * The float32 input value of one 8-bit channel @p v:
 *   norm_signed == 0 -> [0, 1]   (v * (1/255)),
 *   norm_signed != 0 -> [-1, 1]  (v * (1/127.5) - 1),
 * as ONE fused multiply-add.  The gain constants are float reciprocals, so the
 * ends can land a rounding away from exactly 0 / +-1 -- never further.
 */
static inline __attribute__((always_inline)) float nn_input_f32(uint8_t v, int norm_signed)
{
	const float gain = norm_signed ? (1.0f / 127.5f) : (1.0f / 255.0f);
	const float bias = norm_signed ? -1.0f : 0.0f;

	return __builtin_fmaf((float)v, gain, bias);
}

/**
 * The int8 limit applied to a value in quantised units before it is rounded.
 *
 * WHY 255: the result is trunc(f +- 0.5) + zero_point, then saturated to
 * [-128, 127], and the zero point is anywhere in [-128, 127] (the admission test
 * guarantees it).  For f >= 255 the sum is at least trunc(255.5) + (-128) = 127
 * whatever the zero point, so it saturates to 127; for f <= -255 it is at most
 * trunc(-255.5) + 127 = -128, so it saturates to -128.  Limiting f to
 * [-255, 255] therefore changes no result that was defined before, and it makes
 * the float -> int conversion and the zero-point addition defined for every f:
 * |trunc(f +- 0.5)| <= 255, and 255 + 128 is far inside int32.  Without it, a
 * very small scale makes f exceed the int32 range and the conversion undefined;
 * that region is now specified as saturation.  255.5 is exact in float, so the
 * limit itself rounds the same way as its neighbours.
 */
#define NN_INPUT_Q8_LIMIT 255.0f

/**
 * Round @p f (a value already in quantised units, i.e. real / scale) to int8 with
 * @p zero_point: limit to +-NN_INPUT_Q8_LIMIT, round half away from zero by
 * adding +-0.5 and converting toward zero, add the zero point, saturate to
 * [-128, 127].  This is close to, but NOT exactly, TFLM's QUANTIZE rounding
 * (std::round): just below +-0.5 (e.g. 0x1.fffffep-2) the float addition rounds
 * to 1.0, so the result is one away from std::round; and TFLM divides by the
 * scale where this multiplies by 1/scale, which is not identical either.
 * Keeping wio-lite-ai's existing rounding unchanged is issue #162's decision.
 * A NaN is limited to the lower end rather than reaching the
 * conversion (unreachable for an admitted tensor; kept so that no input makes
 * the conversion undefined).
 *
 * @p zero_point must be in [-128, 127] (nn_input_admit() guarantees it).
 */
static inline __attribute__((always_inline)) int8_t nn_input_q8(float f, int32_t zero_point)
{
	int32_t q;

	if (f > NN_INPUT_Q8_LIMIT)
		f = NN_INPUT_Q8_LIMIT;
	if (!(f >= -NN_INPUT_Q8_LIMIT))   /* also catches NaN */
		f = -NN_INPUT_Q8_LIMIT;
	q = (int32_t)(f + (f >= 0.0f ? 0.5f : -0.5f)) + zero_point;
	if (q < -128)
		q = -128;
	else if (q > 127)
		q = 127;
	return (int8_t)q;
}

/**
 * The int8 input value of one 8-bit channel @p v: the normalised value
 * (nn_input_f32()) times @p inv_scale, rounded by nn_input_q8().  The order is
 * fused multiply-add, multiply by 1/scale, limit, sign test, +-0.5, conversion
 * toward zero, zero point, saturation.
 *
 * @p inv_scale is 1 / the tensor's scale, computed ONCE by the caller (per frame,
 * or per band) -- a divide per channel would cost more than the rest.  The
 * admission test makes the scale a positive normal float, so @p inv_scale is
 * finite and the product never becomes NaN or infinite (|normalised| is about 1).
 */
static inline __attribute__((always_inline)) int8_t
nn_input_i8(uint8_t v, int norm_signed, float inv_scale, int32_t zero_point)
{
	return nn_input_q8(nn_input_f32(v, norm_signed) * inv_scale, zero_point);
}

#ifdef __cplusplus
}
#endif

#endif /* NN_INPUT_CONTRACT_H */
