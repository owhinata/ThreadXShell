/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    tensor.h
 * @brief   Board-independent tensor descriptor: the data contract a shared model
 *          post-processor sees (issue #97).
 *
 * Three boards run inference behind three different backends, and each has its
 * own tensor type: `struct nn_tensor` on f746g-disco and wio-lite-ai (port/nn),
 * `struct npu_tensor` on grove-vision-ai-v2 (port/npu).  Both live in board
 * headers, so neither can be included from svc/.  This is the neutral shape a
 * board's glue fills in so that ONE decoder can read tensors from any of them --
 * the same role svc/frame.h plays for the camera pipeline.
 *
 * WIDEST OF THE THREE, DELIBERATELY.  `size_t bytes` and signed `int32_t dims[]`
 * come from the Grove descriptor; the other two boards carry `uint32_t bytes`
 * and `uint16_t dims[]`, which convert into these without loss.  Going the other
 * way would not, which is why the conversion is one-directional: a board fills
 * this in, and nothing converts back.
 *
 * THE ELEMENT TYPE IS A PORTABLE TAG, NOT THE BACKEND'S.  Grove's descriptor
 * carries an opaque TfLiteType whose meaning is pinned by a static_assert in
 * C++; f746/wio carry their own `enum nn_dtype`.  Translating into @ref
 * tensor_dtype is the board's job precisely so that the assert stays where the
 * knowledge is.  A type this file does not name must map to
 * TENSOR_DTYPE_UNSUPPORTED so that a consumer refuses it rather than reading the
 * buffer as something it is not.
 */
#ifndef TENSOR_H
#define TENSOR_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Dimensions carried.  A board MUST REJECT a tensor of higher rank rather than
 *  truncate it to fit: a truncated shape can still match a lookup, and then the
 *  consumer reads the wrong tensor while every check it makes passes. */
#define TENSOR_MAX_DIMS 4

/**
 * Element type, portable across backends.
 *
 * UNSUPPORTED is value 0 so that a zeroed descriptor is refused rather than
 * silently read as int8.
 */
enum tensor_dtype {
	TENSOR_DTYPE_UNSUPPORTED = 0,
	TENSOR_DTYPE_INT8,
	TENSOR_DTYPE_UINT8,
	TENSOR_DTYPE_INT16,
	TENSOR_DTYPE_INT32,
	TENSOR_DTYPE_FLOAT32,
};

/**
 * One output tensor, as a consumer in svc/ sees it.
 *
 * @ref data is read-only to the consumer and stays owned by the backend; it is
 * valid only while that backend says the model is open.  @ref scale and
 * @ref zero_point describe the affine quantisation of an integer @ref dtype and
 * are MEANINGLESS for TENSOR_DTYPE_FLOAT32 -- f746/wio publish scale 0 for an
 * unquantised tensor, so a consumer must not demand a usable scale before it
 * knows the type.
 */
struct tensor_desc {
	const void *data;                   /**< backend-owned buffer, read-only    */
	size_t      bytes;                  /**< length of that buffer              */
	int32_t     dims[TENSOR_MAX_DIMS];  /**< MSB..LSB; entries past rank are 0  */
	uint8_t     rank;                   /**< <= TENSOR_MAX_DIMS                 */
	uint8_t     dtype;                  /**< enum tensor_dtype                  */
	float       scale;                  /**< affine scale (integer dtypes only) */
	int32_t     zero_point;             /**< affine zero point (ditto)          */
};

#ifdef __cplusplus
}
#endif

#endif /* TENSOR_H */
