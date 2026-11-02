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
 *   io_many_inputs.tflite   inputs = tensor 0 named NN_MAX_IO + 1 times, its
 *                           shape 1,000 valid 1s: the count is judged before
 *                           the rank and the values                 -> SHAPE
 *                           (the shape of the walk's work bound: unchecked,
 *                           list length x shape length reads)
 *   io_rank5.tflite         input [1, 1, 1, 1, 1], rank NN_MAX_DIMS + 1 -> TENSOR
 *
 * And the verifier's work bound (NN_TFLM_VERIFY_MAX_TABLES, the same header): the
 * control model with Buffer tables added that nothing references.  The verifier
 * counts a table each time it starts one -- Model, SubGraph, two Tensors and
 * every Buffer -- so the count is 4 + buffers, and the pair below pins it from
 * both sides (an off-by-one in this arithmetic fails one of the two):
 *
 *   vt_tables_4096.tflite   exactly the limit: passes, reaches the later stages
 *   vt_tables_4097.tflite   one over                                -> verifier
 *   vt_shared_4097.tflite   ONE Buffer table named by 4093 entries: 4097 visits
 *                           of 5 distinct tables                    -> verifier
 *                           (the bound is on visits, which sharing multiplies)
 *
 * Usage: gen_model_io_fixtures <out-dir>
 */
#include "tensorflow/lite/schema/schema_generated.h"
/* TFLITE_SCHEMA_VERSION, from the runtime the gate was built against. */
#include "tensorflow/lite/micro/micro_interpreter.h"
/* NN_TFLM_VERIFY_MAX_TABLES, the limit the gate under test verifies with. */
#include "nn_tflm_model_check.h"

/* The fixtures are named for the limit and the gates' tests read those names: a
 * changed limit is a decision (issue #175, U4) that renames them, not one they
 * follow silently. */
static_assert(NN_TFLM_VERIFY_MAX_TABLES == 4096u,
              "vt_tables_4096 / _4097 are named for a 4096-table limit");

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
	uint32_t             buffers;      /* Buffer tables (0 means 1, as before) */
	bool                 shared;       /* every buffers[] entry names ONE table */
};

/* The verifier's table visits for a model built by write_model(). */
uint32_t tables_of(const spec &sp)
{
	const uint32_t nb = sp.buffers ? sp.buffers : 1u;

	return 2u /* Model, SubGraph */ + (sp.out_shape.empty() ? 1u : 2u) + nb;
}

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
	std::vector<flatbuffers::Offset<tflite::Buffer>> bv;
	const uint32_t nb = sp.buffers ? sp.buffers : 1u;
	const auto one = tflite::CreateBuffer(fbb);

	for (uint32_t i = 0; i < nb; i++)
		bv.push_back((sp.shared || i == 0) ? one : tflite::CreateBuffer(fbb));
	const auto bufs = fbb.CreateVector(bv);
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

	const uint32_t lim = NN_TFLM_VERIFY_MAX_TABLES;
	const std::vector<int32_t> long_shape(1000, 1);
	const std::vector<int32_t> many_inputs(NN_MAX_IO + 1, 0);
	const std::vector<int32_t> rank5(NN_MAX_DIMS + 1, 1);
	const spec specs[] = {
		{ "io_ok_65535.tflite",      { 1, 65535 }, { 1, 4 },  { 0 }, { 1 }, false },
		{ "io_in_dim_65536.tflite",  { 1, 65536 }, { 1, 4 },  { 0 }, { 1 }, false },
		{ "io_in_dim_70000.tflite",  { 1, 70000 }, { 1, 4 },  { 0 }, { 1 }, false },
		{ "io_out_dim_neg1.tflite",  { 1, 4 },     { 1, -1 }, { 0 }, { 1 }, false },
		{ "io_out_index_oob.tflite", { 1, 4 },     { 1, 4 },  { 0 }, { 2 }, false },
		{ "io_no_inputs.tflite",     { 1, 4 },     { 1, 4 },  { 0 }, { 1 }, true  },
		{ "io_same_70000.tflite",    { 1, 70000 }, {},        { 0 }, { 0 }, false },
		{ "io_many_inputs.tflite",   long_shape,   { 1, 4 },  many_inputs, { 1 }, false },
		{ "io_rank5.tflite",         rank5,        { 1, 4 },  { 0 }, { 1 }, false },
		/* 4 + buffers table visits: the limit, and one over it (two ways). */
		{ "vt_tables_4096.tflite",   { 1, 4 },     { 1, 4 },  { 0 }, { 1 }, false,
		  lim - 4u, false },
		{ "vt_tables_4097.tflite",   { 1, 4 },     { 1, 4 },  { 0 }, { 1 }, false,
		  lim - 3u, false },
		{ "vt_shared_4097.tflite",   { 1, 4 },     { 1, 4 },  { 0 }, { 1 }, false,
		  lim - 3u, true },
	};

	for (const spec &sp : specs) {
		if (!write_model(argv[1], sp))
			return 1;
		printf("%s: %u table visits\n", sp.file, (unsigned)tables_of(sp));
	}
	return 0;
}
