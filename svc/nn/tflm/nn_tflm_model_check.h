/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    nn_tflm_model_check.h
 * @brief   What a TFLM model must satisfy before it is planned -- the bounded
 *          flatbuffer verifier and the I/O dims walk -- shared by both
 *          interpreting backends and by the three host gates (issue #175).
 *
 * TWO CHECKS, in the order a loader runs them:
 *
 *   nn_tflm_verify_model_buffer()  every offset in the buffer lands inside it,
 *                                  within the limits below (see "THE VERIFIER")
 *   nn_tflm_model_io_check()       subgraph 0's I/O is indexable, 1..NN_MAX_IO
 *                                  per list, rank <= NN_MAX_DIMS, and its
 *                                  dimensions fit a descriptor -- in that order,
 *                                  so its work is constant per list entry
 *
 * THE I/O CHECK: every input and output tensor of subgraph 0 has dimensions a
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
 * Structure is checked over both lists BEFORE anything else, so a model broken in
 * two ways is FORMAT; then the count (SHAPE), the rank (TENSOR) and the values
 * (DIMS), an order that keeps the walk's work constant (see the function).  On a board without a verifier this is the
 * trust GetModel() and AllocateTensors() already extend to the bytes, with the
 * index bounds now in front of them.
 *
 * THE VERIFIER (moved here from svc/nn/ethosu/npu_verify.h, which delegates to
 * it).  flatbuffers' defaults are depth 64 and a million tables; the limits here
 * are 16 and 4096, with the nested-flatbuffer check off (a no-op for this schema:
 * neither pinned schema_generated.h has a nested_flatbuffer field).  What each
 * limit bounds -- the reasoning this file used to state differently:
 *
 *   [!] DEPTH IS BOUNDED BY THE SCHEMA, NOT BY max_depth.  The generated table
 *   verifiers are type-directed: Tensor::Verify() calls
 *   QuantizationParameters::Verify() because the schema says so, whatever the
 *   bytes are, and only a table start counts a level.  Both pinned schemas
 *   (lib/tflite-micro's and grove-vision-ai-v2's SDK tflmtag2412 schema.fbs,
 *   tensorflow/compiler/mlir/lite/schema/) have no cycle among tables; the
 *   longest chain is 6: Model > SubGraph > Tensor > SparsityParameters >
 *   DimensionMetadata > Int32Vector.  So no buffer, valid or hostile, drives the
 *   recursion past 6 and max_depth 16 is not what bounds the stack: it is the
 *   guard for a future schema that grows a cycle.  Reproduce with
 *   cmake/fixtures/model_io/schema_table_depth.py <schema.fbs> (union-aware; the
 *   boards' host tests run it on the schema each gate was built from and fail
 *   if a cycle appears or the chain exceeds NN_TFLM_VERIFY_MAX_DEPTH).
 *
 *   max_tables IS THE VERIFIER'S OWN WORK BOUND, not the whole load's (the I/O
 *   walk below is bounded by its order; AllocateTensors() is another matter): the
 *   number of table VERIFICATION VISITS (each
 *   table start counts once per visit, not every byte the verifier reads).
 *   flatbuffers does not detect shared sub-tables, so a small crafted buffer can
 *   point many vector entries at one table at several levels and multiply the
 *   visits; a million would let a <= 512 KB file keep a console verifying for a
 *   long time.  4096 is a compatibility policy over the models found, not a proof
 *   for every model: measured on the host, every pinned and SDK model needs depth
 *   4 and at most 785 tables (TFLM mobilenet_v2 224, 3.98 MB; blazeface int8 640,
 *   a Vela model 19..697), so 4096 leaves >= 5x.  A legitimate model above it is
 *   refused as malformed -- NN_MODEL_ERR_FORMAT on the board, the verifier's
 *   REJECT on the host gates.
 *
 *   STACK.  The deepest verifier chain is the schema's, the same under any
 *   limit: measured (-fstack-usage, the boards' own compile lines) about 300 B
 *   below the caller on f746g-disco (Model 72, SubGraph 48, Tensor 40, Sparsity
 *   48, DimensionMetadata 40, SparseIndexVector 28, Int32Vector 24), and on
 *   grove-vision-ai-v2 24 B frames or less along the same chain (its deepest
 *   recorded path, a FLATBUFFERS_ASSERT reaching fprintf, is 740 B) -- not the
 *   "~4.1 KB at depth 64" an earlier comment derived by assuming depth followed
 *   the data.  The whole `nn model load` chain down this branch on f746g-disco
 *   is about 1.4 KB of the console's 4,096 B (1.6 KB with the 208 B exception
 *   reserve; the board's README has the figures).
 *
 * [!] ONE DEFINITION, FIVE CALLERS: svc/nn/tflm/nn_tflm.cc (wio-lite-ai,
 * f746g-disco), svc/nn/ethosu/nn_ethosu.cc (grove-vision-ai-v2), and the host gates
 * that exist to reach the board's verdict before a transfer -- both boards'
 * scripts/verify_tflite.cc and grove-vision-ai-v2's scripts/verify_vela_model.cc.
 * A gate that passed what the board refuses would only have moved the failure to
 * the serial line.  Each host rule lists this header (and nn.h) in its DEPENDS.
 * The verifier has four of them: nn_tflm.cc under NN_TFLM_VERIFY (f746g-disco;
 * wio-lite-ai builds it out), npu_verify.h (grove-vision-ai-v2's firmware and
 * verify_vela_model, behind its length floor) and both verify_tflite.
 *
 * C++ (the schema's accessors, TfLiteTensor).  Forced inline (below), no state, no
 * storage; the including function decides where the code lands.
 */
#ifndef NN_TFLM_MODEL_CHECK_H
#define NN_TFLM_MODEL_CHECK_H

#include "flatbuffers/verifier.h"
#include "tensorflow/lite/schema/schema_generated.h"
#include "tensorflow/lite/c/common.h"

#include "nn.h"   /* NN_MODEL_ERR_*, NN_MAX_IO / NN_MAX_DIMS, nn_tensor_dim_fits() */

#include <stddef.h>
#include <stdint.h>

/* [!] FORCED INLINE.  Left to the inliner, grove-vision-ai-v2's build outlined the
 * two list helpers into .text, which is ITCM there, and reached them through
 * veneers from its .text_cold load path (that board's README, "Cold code in
 * SRAM").  Inlined, every byte of this check is in the calling function, wherever
 * the caller's board places that. */
#define NN_TFLM_CHECK_INLINE static inline __attribute__((always_inline))

/** Verifier limits: nesting levels (schema-bounded at 6, see the file header) and
 *  table verification visits (the work bound; measured <= 785). */
#define NN_TFLM_VERIFY_MAX_DEPTH   16u
#define NN_TFLM_VERIFY_MAX_TABLES  4096u

/*
 * Does @p len bytes at @p buf hold a .tflite flatbuffer that is safe to follow,
 * within the limits above?  Nothing here says the model MEANS anything; it says
 * every offset lands inside the buffer, which has to be true before the first
 * accessor after tflite::GetModel() (a cast) follows one.  The caller has checked
 * that @p len covers the identifier it reads at offset 4.
 */
NN_TFLM_CHECK_INLINE bool nn_tflm_verify_model_buffer(const uint8_t *buf, size_t len)
{
	flatbuffers::Verifier::Options opts;

	opts.max_depth                = NN_TFLM_VERIFY_MAX_DEPTH;
	opts.max_tables               = NN_TFLM_VERIFY_MAX_TABLES;
	opts.check_nested_flatbuffers = false;

	flatbuffers::Verifier v(buf, len, opts);

	return tflite::VerifyModelBuffer(v);
}

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

/* @p list (it has passed nn_tflm_io_list_ok()) names 1..NN_MAX_IO tensors --
 * NN_MODEL_ERR_SHAPE's rule, the count the backends check again after planning. */
NN_TFLM_CHECK_INLINE bool nn_tflm_io_list_count_ok(const flatbuffers::Vector<int32_t> *list)
{
	return list->size() >= 1u && list->size() <= (uint32_t)NN_MAX_IO;
}

/* Every tensor in @p list (count already within NN_MAX_IO) declares at most
 * NN_MAX_DIMS dimensions -- the rank half of NN_MODEL_ERR_TENSOR's rule.  A
 * missing shape is a scalar (rank 0), as TFLM reads it. */
NN_TFLM_CHECK_INLINE bool nn_tflm_io_list_rank_ok(const flatbuffers::Vector<int32_t> *list,
                                           const nn_tflm_tensor_vec *tensors)
{
	for (uint32_t j = 0; j < list->size(); j++) {
		const tflite::Tensor *t = tensors->Get((uint32_t)list->Get(j));
		const flatbuffers::Vector<int32_t> *shape = t ? t->shape() : nullptr;

		if (shape && shape->size() > (uint32_t)NN_MAX_DIMS)
			return false;
	}
	return true;
}

/* Every dimension of every tensor in @p list fits a descriptor.  The list has
 * passed the count and rank checks, so this reads at most NN_MAX_IO x
 * NN_MAX_DIMS values. */
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
 * The model's I/O, before it is planned, in this order: 0, or
 *   NN_MODEL_ERR_FORMAT  subgraph 0, its tensors or an I/O list missing, or an
 *                        index outside the tensor vector (structure)
 *   NN_MODEL_ERR_SHAPE   an I/O list naming outside 1..NN_MAX_IO tensors (count)
 *   NN_MODEL_ERR_TENSOR  an I/O tensor declaring more than NN_MAX_DIMS
 *                        dimensions (rank)
 *   NN_MODEL_ERR_DIMS    a dimension outside 0..NN_TENSOR_DIM_MAX (value)
 * SHAPE and TENSOR are the codes the backends give the same failures after
 * planning (they keep those checks as the backstop); judged here they only come
 * sooner.  The order is also what bounds the work: the index check is one
 * comparison per list entry, and everything after it reads at most NN_MAX_IO x
 * NN_MAX_DIMS values per list.  Without the count and rank checks first, one
 * tensor named ~65,000 times with a ~65,000-entry shape -- scalar vectors, which
 * the verifier checks in O(1) and does not count as tables -- passed the
 * verifier and cost ~4e9 iterations here (issue #175 review).
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
	if (!nn_tflm_io_list_count_ok(sg->inputs()) ||
	    !nn_tflm_io_list_count_ok(sg->outputs()))
		return NN_MODEL_ERR_SHAPE;
	if (!nn_tflm_io_list_rank_ok(sg->inputs(), tensors) ||
	    !nn_tflm_io_list_rank_ok(sg->outputs(), tensors))
		return NN_MODEL_ERR_TENSOR;
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
