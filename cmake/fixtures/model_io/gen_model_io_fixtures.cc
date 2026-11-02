/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    gen_model_io_fixtures.cc
 * @brief   Write the small .tflite files the model gates' negative tests run on
 *          (issue #175, svc/nn/tflm/nn_tflm_model_check.h).
 *
 * A HOST tool, built by cmake/fixtures/run_model_io_gate_tests.py against the SAME
 * TFLM tree the gate under test was built from (the include paths are read off
 * that gate's own compile line), so the bytes are laid out by that tree's
 * flatbuffers and schema.  Nothing is checked in as a binary: every run writes
 * the files again from this source.
 *
 * Each model has zero operators and two int8 tensors (input t0, output t1) over an
 * empty buffer 0 -- the smallest graph that is a well-formed flatbuffer, passes
 * the verifier and the schema version, and has graph I/O.  Exactly one thing is
 * wrong in each negative file:
 *
 *   io_ok_65535.tflite      control: input [1, 65535] -- the bound itself fits
 *   io_in_dim_65536.tflite  input  [1, 65536], one past the bound   -> DIMS
 *   io_in_dim_70000.tflite  input  [1, 70000]                       -> DIMS
 *   io_out_dim_neg1.tflite  output [1, -1]                          -> DIMS
 *   io_out_index_oob.tflite outputs = [2], past the tensor vector   -> FORMAT
 *   io_no_inputs.tflite     no inputs vector at all                 -> FORMAT
 *   io_same_70000.tflite    one tensor [1, 70000], input == output  -> DIMS
 *                           (the device fixture: what f746g-disco loads from
 *                           its card to see the refusal on the board)
 *
 * Usage: gen_model_io_fixtures <out-dir>
 */
#include "tensorflow/lite/schema/schema_generated.h"
/* TFLITE_SCHEMA_VERSION, from the runtime the gate was built against. */
#include "tensorflow/lite/micro/micro_interpreter.h"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace {

struct spec {
	const char          *file;
	std::vector<int32_t> in_shape;
	std::vector<int32_t> out_shape;   /* empty: input == output, one tensor */
	std::vector<int32_t> inputs;
	std::vector<int32_t> outputs;
	bool                 no_inputs;
};

bool write_model(const std::string &dir, const spec &sp)
{
	/* [!] AN EXPLICIT ALLOCATOR.  TFLM's copy of flatbuffers treats a null
	 * allocator as a real pointer (no default is substituted, so a firmware
	 * cannot pull in operator new by accident); a builder without one faults on
	 * its first allocation. */
	flatbuffers::DefaultAllocator alloc;
	flatbuffers::FlatBufferBuilder fbb(1024, &alloc);
	std::vector<flatbuffers::Offset<tflite::Tensor>> tensors;

	tensors.push_back(tflite::CreateTensor(
		fbb, fbb.CreateVector(sp.in_shape), tflite::TensorType_INT8, 0,
		fbb.CreateString("in")));
	if (!sp.out_shape.empty())
		tensors.push_back(tflite::CreateTensor(
			fbb, fbb.CreateVector(sp.out_shape), tflite::TensorType_INT8, 0,
			fbb.CreateString("out")));

	const auto tv   = fbb.CreateVector(tensors);
	const auto inv  = sp.no_inputs ? flatbuffers::Offset<flatbuffers::Vector<int32_t>>()
	                               : fbb.CreateVector(sp.inputs);
	const auto outv = fbb.CreateVector(sp.outputs);
	const auto ops  = fbb.CreateVector(std::vector<flatbuffers::Offset<tflite::Operator>>());
	const auto sg   = tflite::CreateSubGraph(fbb, tv, inv, outv, ops, fbb.CreateString("main"));
	const auto sgs  = fbb.CreateVector(std::vector<flatbuffers::Offset<tflite::SubGraph>>{ sg });
	const auto codes =
		fbb.CreateVector(std::vector<flatbuffers::Offset<tflite::OperatorCode>>());
	const auto bufs = fbb.CreateVector(
		std::vector<flatbuffers::Offset<tflite::Buffer>>{ tflite::CreateBuffer(fbb) });
	const auto model = tflite::CreateModel(fbb, TFLITE_SCHEMA_VERSION, codes, sgs,
	                                       fbb.CreateString("issue #175 fixture"), bufs);

	tflite::FinishModelBuffer(fbb, model);

	const std::string path = dir + "/" + sp.file;
	FILE *f = fopen(path.c_str(), "wb");

	if (!f) {
		fprintf(stderr, "gen_model_io_fixtures: cannot write %s\n", path.c_str());
		return false;
	}
	const bool ok = fwrite(fbb.GetBufferPointer(), 1, fbb.GetSize(), f) == fbb.GetSize();
	return (fclose(f) == 0) && ok;
}

}  /* namespace */

int main(int argc, char **argv)
{
	if (argc != 2) {
		fprintf(stderr, "usage: gen_model_io_fixtures <out-dir>\n");
		return 2;
	}

	const spec specs[] = {
		{ "io_ok_65535.tflite",      { 1, 65535 }, { 1, 4 },  { 0 }, { 1 }, false },
		{ "io_in_dim_65536.tflite",  { 1, 65536 }, { 1, 4 },  { 0 }, { 1 }, false },
		{ "io_in_dim_70000.tflite",  { 1, 70000 }, { 1, 4 },  { 0 }, { 1 }, false },
		{ "io_out_dim_neg1.tflite",  { 1, 4 },     { 1, -1 }, { 0 }, { 1 }, false },
		{ "io_out_index_oob.tflite", { 1, 4 },     { 1, 4 },  { 0 }, { 2 }, false },
		{ "io_no_inputs.tflite",     { 1, 4 },     { 1, 4 },  { 0 }, { 1 }, true  },
		{ "io_same_70000.tflite",    { 1, 70000 }, {},        { 0 }, { 0 }, false },
	};

	for (const spec &sp : specs)
		if (!write_model(argv[1], sp))
			return 1;
	return 0;
}
