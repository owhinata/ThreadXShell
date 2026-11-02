/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    nn_tflm_model_check.h
 * @brief   What a TFLM model must satisfy before it is planned, beyond the
 *          flatbuffer being well formed -- shared by both interpreting backends
 *          and by the three host gates (issue #175).
 *
 * TODAY ONE CHECK: every input and output tensor of subgraph 0 has dimensions a
 * descriptor can hold (nn.h, nn_tensor_dim_fits(): 0..NN_TENSOR_DIM_MAX, the range
 * of struct nn_tensor's 16-bit dims).  Without it a dimension of 70,000 was
 * published as 4,464 -- a shape the model does not have, which `nn info` printed
 * and every consumer of the descriptor sized its work by.
 *
 * [!] ON THE MODEL, BEFORE AllocateTensors().  TFLM's allocator builds the I/O
 * tensors' dims from these `Tensor.shape` vectors (the interpreter's I/O is
 * subgraph 0's inputs() / outputs() in both pinned trees), and this check is
 * reached before the planner runs -- which could otherwise refuse a large or
 * negative dimension as NN_MODEL_ERR_ARENA first -- and before the previous
 * interpreter is destroyed.
 *
 * [!] DECLARED SHAPES, NOT NECESSARILY THE PUBLISHED ONES.  A kernel's Prepare()
 * may rewrite an output's dims during AllocateTensors() (the eval tensor's dims
 * are the source of truth, micro_allocator.cc), so in general the planned shape
 * can differ from the declared one.  With the kernels the registered profiles hold
 * today that does not happen to graph I/O; a kernel that rewrites dims is caught
 * only by the backstop, nn_tflm_tensor_dims_fit() on the planned tensors, which
 * both backends keep with the same code.  And the host gates differ here:
 * wio-lite-ai's and f746g-disco's verify_tflite do not plan the model, so they
 * check the declared shapes only (grove-vision-ai-v2's verify_vela_model plans
 * and runs the backstop as well).  A declared -1 (a dimension a RESHAPE would
 * resolve at Prepare()) is refused here as DIMS.
 *
 * [!] IT INDEXES NOTHING UNCHECKED.  A verifier proves offsets, not tensor
 * indices, and wio-lite-ai runs no on-board verifier at all: subgraph 0, its
 * tensor vector and its input / output lists must be present, and every index
 * must lie inside the tensor vector, or the answer is NN_MODEL_ERR_FORMAT (a
 * model whose structure does not hold together -- TFLM would index out of range).
 * Structure is checked over both lists BEFORE any dimension, so a model broken in
 * both ways is FORMAT, never DIMS.  On a board without a verifier this is the
 * trust GetModel() and AllocateTensors() already extend to the bytes, with the
 * index bounds now in front of them.
 *
 * [!] ONE DEFINITION, FIVE CALLERS: svc/nn/tflm/nn_tflm.cc (wio-lite-ai,
 * f746g-disco), svc/nn/ethosu/nn_ethosu.cc (grove-vision-ai-v2), and the host gates
 * that exist to reach the board's verdict before a transfer -- both boards'
 * scripts/verify_tflite.cc and grove-vision-ai-v2's scripts/verify_vela_model.cc.
 * A gate that passed what the board refuses would only have moved the failure to
 * the serial line.  Each host rule lists this header (and nn.h) in its DEPENDS.
 *
 * C++ (the schema's accessors, TfLiteTensor).  Forced inline (below), no state, no
 * storage; the including function decides where the code lands.
 */
#ifndef NN_TFLM_MODEL_CHECK_H
#define NN_TFLM_MODEL_CHECK_H

#include "tensorflow/lite/schema/schema_generated.h"
#include "tensorflow/lite/c/common.h"

#include "nn.h"   /* NN_MODEL_ERR_FORMAT / _DIMS, nn_tensor_dim_fits() */

#include <stdint.h>

/* [!] FORCED INLINE.  Left to the inliner, grove-vision-ai-v2's build outlined the
 * two list helpers into .text, which is ITCM there, and reached them through
 * veneers from its .text_cold load path (that board's README, "Cold code in
 * SRAM").  Inlined, every byte of this check is in the calling function, wherever
 * the caller's board places that. */
#define NN_TFLM_CHECK_INLINE static inline __attribute__((always_inline))

/* Subgraph 0's tensor vector, as the schema types it. */
typedef flatbuffers::Vector<flatbuffers::Offset<tflite::Tensor>> nn_tflm_tensor_vec;

/* One of subgraph 0's index lists names only tensors that exist. */
NN_TFLM_CHECK_INLINE bool nn_tflm_io_list_ok(const flatbuffers::Vector<int32_t> *list,
                                      const nn_tflm_tensor_vec *tensors)
{
	if (!list)
		return false;
	for (uint32_t j = 0; j < list->size(); j++) {
		const int32_t ti = list->Get(j);

		if (ti < 0 || (uint32_t)ti >= tensors->size())
			return false;
	}
	return true;
}

/* Every dimension of every tensor in @p list fits a descriptor.  The list has
 * passed nn_tflm_io_list_ok().  A missing shape is a scalar, as TFLM reads it. */
NN_TFLM_CHECK_INLINE bool nn_tflm_io_list_dims_fit(const flatbuffers::Vector<int32_t> *list,
                                            const nn_tflm_tensor_vec *tensors)
{
	for (uint32_t j = 0; j < list->size(); j++) {
		const tflite::Tensor *t = tensors->Get((uint32_t)list->Get(j));
		const flatbuffers::Vector<int32_t> *shape = t ? t->shape() : nullptr;

		for (uint32_t d = 0; shape && d < shape->size(); d++)
			if (!nn_tensor_dim_fits(shape->Get(d)))
				return false;
	}
	return true;
}

/*
 * The model's I/O, before it is planned: 0, NN_MODEL_ERR_FORMAT (subgraph 0, its
 * tensors or an I/O list missing, or an index outside the tensor vector) or
 * NN_MODEL_ERR_DIMS (a dimension outside 0..NN_TENSOR_DIM_MAX).  The counts are
 * not judged here (NN_MODEL_ERR_SHAPE, after planning), nor the rank
 * (NN_MODEL_ERR_TENSOR).
 */
NN_TFLM_CHECK_INLINE int nn_tflm_model_io_check(const tflite::Model *model)
{
	const auto *subgraphs = model->subgraphs();

	if (!subgraphs || subgraphs->size() == 0u)
		return NN_MODEL_ERR_FORMAT;

	const tflite::SubGraph *sg = subgraphs->Get(0);
	const nn_tflm_tensor_vec *tensors = sg ? sg->tensors() : nullptr;

	if (!tensors || !nn_tflm_io_list_ok(sg->inputs(), tensors) ||
	    !nn_tflm_io_list_ok(sg->outputs(), tensors))
		return NN_MODEL_ERR_FORMAT;
	if (!nn_tflm_io_list_dims_fit(sg->inputs(), tensors) ||
	    !nn_tflm_io_list_dims_fit(sg->outputs(), tensors))
		return NN_MODEL_ERR_DIMS;
	return 0;
}

/* The backstop on a planned tensor, just before its descriptor is published: the
 * same rule over the dims TFLM built.  Its rank has passed NN_MODEL_ERR_TENSOR's
 * check. */
NN_TFLM_CHECK_INLINE bool nn_tflm_tensor_dims_fit(const TfLiteTensor *t)
{
	for (int i = 0; t->dims && i < t->dims->size; i++)
		if (!nn_tensor_dim_fits((int32_t)t->dims->data[i]))
			return false;
	return true;
}

#endif /* NN_TFLM_MODEL_CHECK_H */
