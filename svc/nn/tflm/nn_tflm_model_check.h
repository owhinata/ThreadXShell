/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    nn_tflm_model_check.h
 * @brief   What a TFLM model must satisfy before it is planned -- the bounded
 *          flatbuffer verifier and the graph walk (structure, lengths, ranks,
 *          I/O dims) -- shared by both interpreting backends and by the three
 *          host gates (issues #175, #178).
 *
 * TWO CHECKS, in the order a loader runs them:
 *
 *   nn_tflm_verify_model_buffer()  every offset in the buffer lands inside it,
 *                                  within the limits below (see "THE VERIFIER")
 *   nn_tflm_model_io_check()       the graph holds together (every subgraph's
 *                                  tensors, every operator's tensor lists, each
 *                                  at most NN_TFLM_MAX_OP_TENSORS long and naming
 *                                  only tensors that exist), subgraph 0's I/O is
 *                                  1..NN_MAX_IO per list, every tensor's rank is
 *                                  at most NN_TFLM_MAX_TENSOR_RANK (subgraph 0's
 *                                  I/O at most NN_MAX_DIMS), and the I/O
 *                                  dimensions fit a descriptor -- in that order,
 *                                  lengths before elements, so its work is a
 *                                  small constant per table the verifier visited
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
 * The same holds for EVERY subgraph and EVERY operator (issue #178): TFLM's
 * planner writes subgraph_allocation_info[index] for each operator input and
 * output without a range check (micro_allocation_info.cc, MarkAllocationLifetimes)
 * and reads eval_tensors[index] for each intermediate (ValidateSubgraph), and it
 * hands a kernel a null inputs / outputs list as it found it.  So every subgraph
 * must have a tensor vector; an operator vector may be absent or empty (TFLM's
 * NumSubgraphOperators() reads that as no operators), but every operator present
 * has an inputs and an outputs list; inputs name -1 (TFLM's optional tensor) or a
 * tensor of the operator's subgraph, outputs and intermediates (which may be
 * absent) a tensor only.  NOT checked: the inputs / outputs lists of a subgraph
 * other than 0, neither their length nor their indices -- TFLM reads them only
 * through a control-flow operator (IF / WHILE / CALL_ONCE, in
 * MarkAllocationLifetimes), and no profile registers one (nn_tflm_ops.h), so a
 * model that would use them is refused by the resolver; a profile that adds one
 * has to extend this walk first.  Structure is checked BEFORE anything else, so a model
 * broken in two ways is FORMAT; then the count (SHAPE), the rank (TENSOR) and the
 * values (DIMS).  On a board without a verifier this is the trust GetModel() and
 * AllocateTensors() already extend to the bytes, with the index bounds now in
 * front of them.
 *
 * [!] LENGTHS BEFORE ELEMENTS: WHAT IT BOUNDS OF THE LOAD'S WORK (issue #178).  The
 * verifier checks a scalar vector (a shape, an operator's tensor list) in O(1)
 * and does not count it as a table, and it accepts many vector entries naming
 * one table: a 535 KB file with one tensor (a 65,000-entry shape) named by 1,900
 * tensor entries and one operator (65,000 inputs) named by 2,000 operator entries
 * passed the 16 / 4096 verifier and the #175 walk, and then cost TFLM ~1.2e8
 * shape reads and ~1.3e8 list reads per pass in AllocateTensors() -- the
 * verifier's table count bounded none of it.  So this walk reads each scalar
 * vector's LENGTH before any element and refuses one that is too long:
 *
 *   per tensor entry    one rank compare (shape()->size(); the internal tensors'
 *                       dimension VALUES are never read -- Vela's internal
 *                       tensors reach 2.2 M, a value rule would refuse them)
 *   per operator entry  three length compares, then at most
 *                       3 x NN_TFLM_MAX_OP_TENSORS index reads
 *
 * Every tensor and operator entry is one table visit of the verifier (a shared
 * subgraph's are visited once per entry naming it), so behind the 16 / 4096
 * verifier the walk reads at most 4096 x 3 x NN_TFLM_MAX_OP_TENSORS = 786,432
 * list elements, and what it passes bounds TFLM's own passes over the SAME
 * vectors -- shapes at most NN_TFLM_MAX_TENSOR_RANK per tensor entry, operator
 * lists the same 3 x 64 per operator entry, so the shape, operator-list and
 * index reads of a load are visits x a small constant.  That is not all of
 * AllocateTensors(): MarkAllocationLifetimes() walks every scratch request for
 * each operator (O(N x M), TFLM's own comment) and the greedy memory planner is
 * quadratic in its buffers, both counted in tensors and operators, so the whole
 * load is bounded by visits squared x small constants, not visits x a constant.
 *
 * [!] THE ONE EXCEPTION TO "LENGTHS FIRST": subgraph 0's two I/O lists are
 * walked once by nn_tflm_io_list_ok() BEFORE their count is judged (one compare
 * per entry, O(the list's length): at most ~128K entries in a 512 KB file, read
 * once, never multiplied by sharing).  The order is kept so that a model broken
 * in two ways stays FORMAT rather than SHAPE.
 *
 * wio-lite-ai runs no verifier: there nothing bounds the number
 * of entries but the file, the trust described above.  (What this does not bound
 * is outside it by decision: grove-vision-ai-v2's payload walk per operator entry,
 * issue #179, and the per-channel quantization copy, issue #180.)
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
 *   max_tables IS THE VERIFIER'S OWN WORK BOUND, and through the walk the load's
 *   (see "LENGTHS BEFORE ELEMENTS" above): the number of table VERIFICATION
 *   VISITS (each table start counts once per visit, not every byte the verifier
 *   reads).
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
/* RuntimeShape::kMaxSmallSize, the rank NN_TFLM_MAX_TENSOR_RANK is tied to. */
#include "tensorflow/lite/kernels/internal/runtime_shape.h"

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
 * The longest tensor list (inputs, outputs or intermediates) one operator may
 * carry (issue #178).  TFLM has no such constant (no kMaxTensorsPerOp in either
 * pinned tree), so it is derived: measured over every pinned, SDK, TFLM-bundled
 * and MLPerf model on the host, the longest list is 24 (inputs of
 * UNIDIRECTIONAL_SEQUENCE_LSTM, which no profile registers) and 9 among
 * operators a profile registers (an ethos-u operator of the SDK's yolo11); the
 * schema's largest fixed arity, BIDIRECTIONAL_SEQUENCE_LSTM, is 48 inputs.  64
 * passes all of them; a list past it is no model this runtime runs and is
 * refused as NN_MODEL_ERR_FORMAT, as the verifier's own limit is.
 */
#define NN_TFLM_MAX_OP_TENSORS     64u

/*
 * The highest rank any tensor may declare (issue #178): TFLM's
 * RuntimeShape::kMaxSmallSize (tensorflow/lite/kernels/internal/runtime_shape.h),
 * the size of the fixed dims_[] array kernels copy a shape into.  TFLM checks it
 * with TFLITE_DCHECK only, which a release build compiles out, so a rank-7 tensor
 * would overrun that array.  Only the rank is read: an internal tensor's
 * dimension values are never judged here.  Subgraph 0's I/O keeps the tighter
 * NN_MAX_DIMS, the descriptor's.
 */
#define NN_TFLM_MAX_TENSOR_RANK    6u

static_assert(NN_TFLM_MAX_TENSOR_RANK == (unsigned)tflite::RuntimeShape::kMaxSmallSize,
              "the rank bound is TFLM's RuntimeShape capacity in the tree built against");
static_assert((unsigned)NN_MAX_DIMS <= NN_TFLM_MAX_TENSOR_RANK,
              "an I/O tensor's rank bound cannot exceed every tensor's");

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

/* A subgraph's tensor vector, as the schema types it. */
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

/* An operator's tensor list (its length already within NN_TFLM_MAX_OP_TENSORS)
 * names only tensors of its subgraph -- or -1, TFLM's optional tensor, where
 * @p optional_ok (inputs). */
NN_TFLM_CHECK_INLINE bool nn_tflm_op_list_indices_ok(const flatbuffers::Vector<int32_t> *list,
                                              uint32_t ntensors, bool optional_ok)
{
	for (uint32_t j = 0; list && j < list->size(); j++) {
		const int32_t ti = list->Get(j);

		if (ti == -1 && optional_ok)
			continue;
		if (ti < 0 || (uint32_t)ti >= ntensors)
			return false;
	}
	return true;
}

/* A subgraph's operator vector, as the schema types it. */
typedef flatbuffers::Vector<flatbuffers::Offset<tflite::Operator>> nn_tflm_op_vec;

/* SubGraph::operators(), spelled through Table::GetPointer<const void *>: the
 * same read of the same field (the offset field's target, or null when absent);
 * a generated table IS a flatbuffers::Table at the same address (a private base,
 * which is how GetRoot<> reaches it too).  [!] grove-vision-ai-v2: the generated
 * accessor, called from here as well as from SubGraph::Verify(), was outlined
 * into .text (ITCM, +16 B); the void instance is one that build already outlines
 * for other callers, so the walk adds no ITCM byte (issue #178; the board's
 * README, "Cold code in SRAM"). */
NN_TFLM_CHECK_INLINE const nn_tflm_op_vec *nn_tflm_sg_operators(const tflite::SubGraph *sg)
{
	const flatbuffers::Table *t = reinterpret_cast<const flatbuffers::Table *>(sg);

	return static_cast<const nn_tflm_op_vec *>(
		t->GetPointer<const void *>(tflite::SubGraph::VT_OPERATORS));
}

/* An optional tensor list's length, 0 when it is absent. */
NN_TFLM_CHECK_INLINE uint32_t nn_tflm_list_len(const flatbuffers::Vector<int32_t> *list)
{
	return list ? list->size() : 0u;
}

/*
 * One subgraph holds together: a tensor vector, and every operator present with
 * an inputs and an outputs list, each of its three lists at most
 * NN_TFLM_MAX_OP_TENSORS long -- all three LENGTHS judged before any element is
 * read -- and then every index in range.  Per operator entry: three compares and
 * at most 3 x NN_TFLM_MAX_OP_TENSORS reads.
 */
NN_TFLM_CHECK_INLINE bool nn_tflm_subgraph_ok(const tflite::SubGraph *sg)
{
	const nn_tflm_tensor_vec *tensors = sg ? sg->tensors() : nullptr;

	if (!tensors)
		return false;

	const nn_tflm_op_vec *ops = nn_tflm_sg_operators(sg);
	const uint32_t nt = tensors->size();

	for (uint32_t i = 0; ops && i < ops->size(); i++) {
		const tflite::Operator *op = ops->Get(i);

		if (!op || !op->inputs() || !op->outputs())
			return false;
		if (op->inputs()->size() > NN_TFLM_MAX_OP_TENSORS ||
		    op->outputs()->size() > NN_TFLM_MAX_OP_TENSORS ||
		    nn_tflm_list_len(op->intermediates()) > NN_TFLM_MAX_OP_TENSORS)
			return false;
		if (!nn_tflm_op_list_indices_ok(op->inputs(), nt, true) ||
		    !nn_tflm_op_list_indices_ok(op->outputs(), nt, false) ||
		    !nn_tflm_op_list_indices_ok(op->intermediates(), nt, false))
			return false;
	}
	return true;
}

/* Every tensor of @p sg (it has passed nn_tflm_subgraph_ok()) declares at most
 * NN_TFLM_MAX_TENSOR_RANK dimensions: one length compare per tensor entry, no
 * dimension read. */
NN_TFLM_CHECK_INLINE bool nn_tflm_subgraph_rank_ok(const tflite::SubGraph *sg)
{
	const nn_tflm_tensor_vec *tensors = sg->tensors();

	for (uint32_t i = 0; i < tensors->size(); i++) {
		const tflite::Tensor *t = tensors->Get(i);
		const flatbuffers::Vector<int32_t> *shape = t ? t->shape() : nullptr;

		if (shape && shape->size() > NN_TFLM_MAX_TENSOR_RANK)
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
 * NN_MAX_DIMS dimensions -- the I/O half of NN_MODEL_ERR_TENSOR's rank rule.  A
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
 * The model, before it is planned, in this order: 0, or
 *   NN_MODEL_ERR_FORMAT  subgraph 0, its tensors or an I/O list missing, or an
 *                        I/O index outside the tensor vector; any subgraph
 *                        without tensors; an operator without an inputs or
 *                        outputs list, with a list longer than
 *                        NN_TFLM_MAX_OP_TENSORS, or naming a tensor that does not
 *                        exist (structure)
 *   NN_MODEL_ERR_SHAPE   an I/O list naming outside 1..NN_MAX_IO tensors (count)
 *   NN_MODEL_ERR_TENSOR  any tensor declaring more than NN_TFLM_MAX_TENSOR_RANK
 *                        dimensions, or an I/O tensor more than NN_MAX_DIMS (rank)
 *   NN_MODEL_ERR_DIMS    an I/O dimension outside 0..NN_TENSOR_DIM_MAX (value)
 * SHAPE and TENSOR are the codes the backends give the same failures after
 * planning (they keep those checks as the backstop); judged here they only come
 * sooner.  The order is also what bounds the work (the file header, "LENGTHS
 * BEFORE ELEMENTS"): the I/O index check is one comparison per list entry, every
 * other vector's length is judged before its elements are read, and everything
 * after the structure reads at most NN_MAX_IO x NN_MAX_DIMS values per I/O list.
 * Without the count and rank checks first, one tensor named ~65,000 times with a
 * ~65,000-entry shape -- scalar vectors, which the verifier checks in O(1) and
 * does not count as tables -- passed the verifier and cost ~4e9 iterations here
 * (issue #175 review).
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
	for (uint32_t i = 0; i < subgraphs->size(); i++)
		if (!nn_tflm_subgraph_ok(subgraphs->Get(i)))
			return NN_MODEL_ERR_FORMAT;
	if (!nn_tflm_io_list_count_ok(sg->inputs()) ||
	    !nn_tflm_io_list_count_ok(sg->outputs()))
		return NN_MODEL_ERR_SHAPE;
	if (!nn_tflm_io_list_rank_ok(sg->inputs(), tensors) ||
	    !nn_tflm_io_list_rank_ok(sg->outputs(), tensors))
		return NN_MODEL_ERR_TENSOR;
	for (uint32_t i = 0; i < subgraphs->size(); i++)
		if (!nn_tflm_subgraph_rank_ok(subgraphs->Get(i)))
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
