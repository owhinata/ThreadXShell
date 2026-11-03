/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    npu_model_scan.cc
 * @brief   Locate and vet an Ethos-U model's driver payload.  See the header.
 *
 * Split out of the board's interpreter glue in issue #45 so that the host gate and the firmware
 * share one answer to "which bytes does the driver parse" -- not just to "is
 * this byte sequence safe", which npu_payload.c already shared.
 *
 * Nothing here allocates, and nothing here has state.  It reads a flatbuffer
 * that may be sitting in flash and returns a verdict.
 */
#include "npu_model_scan.h"
#include "npu_payload.h"
#include "npu_cmdstream.h"   /* the command stream walk (issue #184) */

#include <string.h>

#include "tensorflow/lite/schema/schema_utils.h"   /* GetBuiltinCode() */
#include "nn_tflm_model_check.h"   /* NN_TFLM_MAX_OP_TENSORS, the walk's readers */

const uint8_t *npu_model_command_stream(const tflite::Model *model,
                                        const tflite::SubGraph *sub,
                                        const tflite::Operator *op,
                                        size_t *bytes, const char **why)
{
	*bytes = 0;

	const auto *inputs = op->inputs();

	if (inputs == nullptr || inputs->size() == 0u) {
		*why = "the operator has no inputs";
		return nullptr;
	}

	const int32_t ti = inputs->Get(0);
	const auto *tensors = sub->tensors();

	if (ti < 0 || tensors == nullptr ||
	    static_cast<uint32_t>(ti) >= tensors->size()) {
		*why = "input tensor 0 is out of range";
		return nullptr;
	}

	const auto *t = tensors->Get(static_cast<uint32_t>(ti));

	if (t == nullptr) {
		*why = "input tensor 0 is missing";
		return nullptr;
	}
	if (t->is_variable()) {
		*why = "input tensor 0 is variable, so AllocateVariables() would "
		       "replace the buffer the driver is given";
		return nullptr;
	}
	if (t->type() != tflite::TensorType_UINT8 &&
	    t->type() != tflite::TensorType_INT8) {
		*why = "input tensor 0 does not have one-byte elements";
		return nullptr;
	}

	const auto *shape = t->shape();

	if (shape == nullptr || shape->size() != 1u) {
		*why = "input tensor 0 is not one-dimensional";
		return nullptr;
	}

	const int32_t extent = shape->Get(0);

	if (extent <= 0) {
		*why = "input tensor 0 is empty";
		return nullptr;
	}

	const auto *buffers = model->buffers();

	if (buffers == nullptr || t->buffer() >= buffers->size()) {
		*why = "input tensor 0 names no buffer";
		return nullptr;
	}

	const auto *buf  = buffers->Get(t->buffer());
	const auto *data = buf != nullptr ? buf->data() : nullptr;

	/* Constant, and exactly as long as the size the driver will be given.
	 * With a serialized buffer and is_variable() false, TFLM keeps the tensor
	 * kTfLiteMmapRo and the planner leaves it alone, so the pointer Eval()
	 * passes the driver is this buffer -- the bytes walked by the caller. */
	if (data == nullptr || data->size() != static_cast<uint32_t>(extent)) {
		*why = "input tensor 0's constant buffer is not exactly as long as "
		       "its shape";
		return nullptr;
	}

	*bytes = data->size();
	return data->data();
}

const char *npu_model_payload_refusal(const tflite::Model *model,
                                      size_t model_bytes)
{
	/* Payload bytes charged so far, against model_bytes (issue #179, the header). */
	size_t charged = 0;

	const auto *subgraphs = model->subgraphs();

	if (subgraphs == nullptr || subgraphs->size() != 1u)
		return "the model does not have exactly one subgraph";

	const auto *sub   = subgraphs->Get(0);
	const auto *codes = model->operator_codes();

	if (sub == nullptr)
		return "the subgraph is missing";

	const auto *ops = sub->operators();

	if (ops == nullptr || ops->size() == 0u)
		return "the subgraph has no operators";
	if (codes == nullptr)
		return "the model declares no operator codes";

	for (unsigned n = 0; n < ops->size(); n++) {
		const auto *op = ops->Get(n);

		if (op == nullptr)
			return "an operator is missing";

		const uint32_t ci = op->opcode_index();

		if (ci >= codes->size())
			return "an operator names an operator code that does not exist";

		const auto *code = codes->Get(ci);

		if (code == nullptr ||
		    tflite::GetBuiltinCode(code) != tflite::BuiltinOperator_CUSTOM)
			return "an operator is a builtin; this port registers only ethos-u";

		const auto *custom = code->custom_code();

		if (custom == nullptr || strcmp(custom->c_str(), "ethos-u") != 0)
			return "an operator is a custom operator other than ethos-u";

		size_t bytes = 0;
		const char *why = nullptr;
		const uint8_t *data =
			npu_model_command_stream(model, sub, op, &bytes, &why);

		if (data == nullptr)
			return why;

		/* [!] LENGTH BEFORE ELEMENTS, then no -1 after input 0.  The kernel's
		 * Eval() hands every input after 0 to GetEvalTensor() unchecked (an
		 * address for the driver's base pointers), so -1 -- TFLM's optional
		 * tensor, which the shared walk lets through because other kernels take
		 * it -- reads eval_tensors[-1].  Vela never emits it here.  This runs
		 * before the walk that bounds the list (nn_tflm_model_io_check()), so
		 * the list's length is judged here first: otherwise one shared operator
		 * with 65,000 inputs named by 4,000 entries is 2.6e8 reads. */
		const auto *inputs = op->inputs();   /* non-empty: located above */

		if (inputs->size() > NN_TFLM_MAX_OP_TENSORS)
			return "an ethos-u operator lists more inputs than the firmware's "
			       "operator walk allows";
		for (uint32_t i = 1u; i < inputs->size(); i++) {
			if (inputs->Get(i) < 0)
				return "an ethos-u operator names no tensor (-1) for an input "
				       "after the command stream, which its Eval() reads "
				       "unchecked";
		}

		/* [!] NO DATA FROM INPUT 2 ON (issue #183).  The kernel hands inputs
		 * 1..n to the driver as base pointers 0..n-1, and Vela's command stream
		 * WRITES through base pointers 1 and 2 -- input 2 is the TFLM arena
		 * region, input 3 the fast scratch.  A tensor with data (the shared
		 * walk's test: a Buffer with a data vector of size > 0) is not planned
		 * into the arena (micro_allocation_info.cc), so its pointer is the
		 * model -- the XIP window -- and the NPU would write there; the walk's
		 * written-tensor rule covers operator outputs and graph I/O only.
		 * Input 1 is the weights, constant by design.  Every Vela model
		 * measured carries data on inputs 0 and 1 only.  The walk has not yet
		 * proven the indices here (the board runs this scan first), so the
		 * tensor index and the buffer index are compared before either is
		 * read.  [!] Read through the walk's forced-inline helpers (the buffer
		 * vector, a Tensor's buffer field, a vector-of-tables entry) and
		 * Buffer::data() spelled as Table::GetPointer<const void *>, the same
		 * test as the walk's nn_tflm_buffer_data(): the generated accessors
		 * called from here were outlined into .text, ITCM on this board (+104 B
		 * with four veneers; Buffer::data() alone +16 B). */
		const auto *tensors = sub->tensors();   /* present: located above */
		const nn_tflm_buffer_vec *buffers = static_cast<const nn_tflm_buffer_vec *>(
			nn_tflm_model_field(model, tflite::Model::VT_BUFFERS));   /* present: likewise */

		for (uint32_t i = 2u; i < inputs->size(); i++) {
			const uint32_t ti = static_cast<uint32_t>(inputs->Get(i));   /* >= 0 above */

			if (ti >= tensors->size())
				return "an ethos-u operator names an input tensor that does "
				       "not exist";

			const auto *t = tensors->Get(ti);

			if (t == nullptr || nn_tflm_tensor_buffer(t) >= buffers->size())
				return "an ethos-u operator's input names no buffer";
			const auto *b = nn_tflm_vec_table(buffers, nn_tflm_tensor_buffer(t));
			const auto *d = b == nullptr ? nullptr
			                : static_cast<const flatbuffers::Vector<uint8_t> *>(
			                          reinterpret_cast<const flatbuffers::Table *>(b)
			                                  ->GetPointer<const void *>(
			                                          tflite::Buffer::VT_DATA));

			if (d != nullptr && d->size() != 0u)
				return "an ethos-u operator's input after the weights carries "
				       "data, so the NPU would write into the model";
		}

		/* [!] CHARGED BEFORE IT IS WALKED (issue #179).  Operator entries may
		 * name one Operator table, and distinct tensors may name one Buffer
		 * table; the verifier accepts both, so without this 4,000 entries
		 * naming one 400 KB stream are 4,000 walks of it.  An honest model's
		 * payloads are disjoint byte runs inside the file, so their sum is at
		 * most the file's length: a STRUCTURAL bound, not a measured knob.
		 * Refusing the payload that would take the sum past model_bytes, before
		 * walking it, keeps the whole scan at <= model_bytes / 4 words -- and
		 * the driver's own parse at every Invoke() to the same. */
		if (bytes > model_bytes - charged)
			return "the operators' payloads add up to more bytes than the "
			       "model, so a command stream is shared or overlaps";
		charged += bytes;

		if (!npu_payload_is_single_final_command_stream(data, bytes,
		                                                nullptr, nullptr))
			return "the payload is not exactly one COMMAND_STREAM as its last "
			       "action, so a launch could be abandoned with the arena "
			       "still owned by the NPU";
	}
	return nullptr;
}

namespace {

/*
 * Region @p ti of the table npu_model_cmdstream_refusal() builds (issue #184):
 * the byte size of tensor @p ti and whether the NPU may write it.  Read through
 * the walk's forced-inline helpers (nn_tflm_model_check.h) and Buffer::data()
 * spelled as Table::GetPointer, for the reason the input-2 loop above gives:
 * the generated accessors called from this file are outlined into .text, ITCM
 * on grove-vision-ai-v2.  Anything the walk would have refused -- an index out
 * of range, a rank above NN_TFLM_MAX_TENSOR_RANK, a negative dimension, a size
 * past 32 bits, a type with no size -- gives 0 bytes and not writable, so a
 * stream touching it is refused rather than a bound guessed.
 */
__attribute__((always_inline)) inline npu_cs_region
cs_region_of(const nn_tflm_tensor_vec *tensors, const nn_tflm_buffer_vec *buffers,
             int32_t ti)
{
	npu_cs_region r = { 0u, false };

	if (ti < 0 || static_cast<uint32_t>(ti) >= tensors->size())
		return r;

	const tflite::Tensor *t = nn_tflm_vec_table(tensors, static_cast<uint32_t>(ti));
	const uint32_t eb2 = t != nullptr ? nn_tflm_elem_bytes2(nn_tflm_tensor_type(t)) : 0u;

	if (eb2 == 0u)
		return r;

	const uint32_t bi = nn_tflm_tensor_buffer(t);

	if (bi >= buffers->size())
		return r;

	const auto *b = nn_tflm_vec_table(buffers, bi);
	const auto *d = b == nullptr ? nullptr
	                : static_cast<const flatbuffers::Vector<uint8_t> *>(
	                          reinterpret_cast<const flatbuffers::Table *>(b)
	                                  ->GetPointer<const void *>(tflite::Buffer::VT_DATA));
	const flatbuffers::Vector<int32_t> *shape = nn_tflm_tensor_shape(t);
	const uint32_t rank = shape != nullptr ? shape->size() : 0u;

	if (rank > NN_TFLM_MAX_TENSOR_RANK)
		return r;

	/* Elements, then bytes: eb2 is the size in HALF bytes (INT4 1), so
	 * ceil(n * eb2 / 2) -- a constant's exact length (the walk's equality) and
	 * at most TFLM's allocation (TfLiteTypeSizeOf(), INT4 1 byte).  Every
	 * product bounded before it is formed; 32-bit divisions only. */
	uint32_t n = 1u;

	for (uint32_t k = 0; k < rank; k++) {
		const int32_t v = shape->Get(k);

		if (v < 0 || (v != 0 && n > UINT32_MAX / static_cast<uint32_t>(v)))
			return r;
		n *= static_cast<uint32_t>(v);
	}
	if (eb2 == 1u) {
		r.bytes = n / 2u + (n & 1u);
	} else {
		if (n > UINT32_MAX / (eb2 / 2u))
			return r;
		r.bytes = n * (eb2 / 2u);
	}
	r.writable = d == nullptr || d->size() == 0u;
	return r;
}

}  /* namespace */

const char *npu_model_cmdstream_refusal(const tflite::Model *model,
                                        uint32_t shram_bytes)
{
	const auto *subgraphs = model->subgraphs();

	if (subgraphs == nullptr || subgraphs->size() != 1u)
		return "the model does not have exactly one subgraph";

	const auto *sub = subgraphs->Get(0);
	/* nn_tflm_sg_operators(), not SubGraph::operators(): called from here the
	 * generated accessor was outlined into .text (ITCM, +20 B). */
	const nn_tflm_op_vec *ops = sub != nullptr ? nn_tflm_sg_operators(sub) : nullptr;
	const auto *tensors = sub != nullptr ? sub->tensors() : nullptr;
	const nn_tflm_buffer_vec *buffers = static_cast<const nn_tflm_buffer_vec *>(
		nn_tflm_model_field(model, tflite::Model::VT_BUFFERS));

	if (ops == nullptr || tensors == nullptr || buffers == nullptr)
		return "the subgraph has no operators, tensors or buffers";

	for (uint32_t n = 0; n < ops->size(); n++) {
		const auto *op = ops->Get(n);

		if (op == nullptr)
			return "an operator is missing";

		size_t bytes = 0;
		const char *why = nullptr;
		const uint8_t *data =
			npu_model_command_stream(model, sub, op, &bytes, &why);

		if (data == nullptr)
			return why;

		uint32_t first = 0u;
		uint32_t count = 0u;

		if (!npu_payload_is_single_final_command_stream(data, bytes, &first,
		                                                &count))
			return "the payload is not exactly one COMMAND_STREAM as its last "
			       "action, so a launch could be abandoned with the arena "
			       "still owned by the NPU";

		/* The base pointers, in the kernel's order: inputs 1.. then
		 * outputs, at most NPU_CS_MAX_REGIONS (ethosu.cc Eval(): num_tensors
		 * = min(num_tensors, 8)). */
		npu_cs_region regions[NPU_CS_MAX_REGIONS];
		uint32_t nreg = 0u;
		const auto *inputs = op->inputs();   /* non-empty: located above */
		const auto *outputs = op->outputs();

		const uint32_t nin = inputs->size() - 1u;
		const uint32_t nout = outputs != nullptr ? outputs->size() : 0u;

		/* One loop over both lists, so the forced-inline reader is expanded
		 * once.  The walk bounds each list at NN_TFLM_MAX_OP_TENSORS. */
		for (uint32_t k = 0u; k < nin + nout && nreg < NPU_CS_MAX_REGIONS; k++)
			regions[nreg++] = cs_region_of(tensors, buffers,
			                               k < nin ? inputs->Get(k + 1u)
			                                       : outputs->Get(k - nin));

		const enum npu_cs_refusal r = npu_cmdstream_check(
			reinterpret_cast<const uint32_t *>(data) + first, count,
			regions, nreg, shram_bytes, nullptr);

		if (r != NPU_CS_OK)
			return npu_cmdstream_refusal_text(r);
	}
	return nullptr;
}
