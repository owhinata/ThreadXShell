/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    npu_model_scan.cc
 * @brief   Locate and vet an Ethos-U model's driver payload.  See the header.
 *
 * Split out of npu_tflm.cc in issue #45 so that the host gate and the firmware
 * share one answer to "which bytes does the driver parse" -- not just to "is
 * this byte sequence safe", which npu_payload.c already shared.
 *
 * Nothing here allocates, and nothing here has state.  It reads a flatbuffer
 * that may be sitting in flash and returns a verdict.
 */
#include "npu_model_scan.h"
#include "npu_payload.h"

#include <string.h>

#include "tensorflow/lite/schema/schema_utils.h"   /* GetBuiltinCode() */

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

const char *npu_model_payload_refusal(const tflite::Model *model)
{
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
		if (!npu_payload_is_single_final_command_stream(data, bytes))
			return "the payload is not exactly one COMMAND_STREAM as its last "
			       "action, so a launch could be abandoned with the arena "
			       "still owned by the NPU";
	}
	return nullptr;
}
