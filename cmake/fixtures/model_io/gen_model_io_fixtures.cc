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
 * Each model below has zero operators and two int8 tensors (input t0, output t1) over an
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
 * And the rest of the graph (issue #178): the same two I/O tensors, plus one ADD
 * operator (in every tflm profile; the Ethos-U gate judges the walk before the
 * operator) and/or more tensors that are not graph I/O.  The limits are in the
 * names, NN_TFLM_MAX_OP_TENSORS (64) and NN_TFLM_MAX_TENSOR_RANK (6):
 *
 *   op_in_64.tflite         control: ADD with 64 inputs (all t0)  -- the limit
 *   op_out_64.tflite        control: 64 outputs (all t1)
 *   op_int_64.tflite        control: 64 intermediates (all t1)
 *   op_in_65.tflite         65 inputs                               -> FORMAT
 *   op_out_65.tflite        65 outputs                              -> FORMAT
 *   op_int_65.tflite        65 intermediates                        -> FORMAT
 *   op_in_neg1.tflite       control: inputs [0, -1], TFLM's optional tensor
 *   op_in_oob.tflite        inputs [0, 2], 2 == the tensor count    -> FORMAT
 *   op_out_neg1.tflite      outputs [-1] (optional is inputs only)  -> FORMAT
 *   op_int_neg1.tflite      intermediates [-1] (likewise)           -> FORMAT
 *   op_int_oob.tflite       intermediates [2], 2 == the tensor count -> FORMAT
 *   op_no_inputs.tflite     ADD with no inputs vector               -> FORMAT
 *   op_no_outputs.tflite    ADD with no outputs vector              -> FORMAT
 *   t_rank_6.tflite         control: a third tensor, not I/O, of rank 6
 *   t_rank_7.tflite         that tensor of rank 7                   -> TENSOR
 *   sg1_ok.tflite           control: a second subgraph with a tensor and no
 *                           operators (its I/O lists are not walked, see the
 *                           header)
 *   sg1_no_tensors.tflite   a second subgraph with no tensor vector -> FORMAT
 *
 * And two witnesses of the walk's WORK bound, built the way that defeated the
 * verifier's (a scalar vector is checked in O(1) and not counted, and many
 * entries may name one table):
 *
 *   op_in_65000_shared.tflite  4000 operator entries naming ONE ADD with 65,000
 *                              inputs (all valid)                   -> FORMAT
 *   t_shape_65000_shared.tflite 4000 tensor entries naming ONE tensor whose
 *                              shape has 65,000 dimensions (all 1)  -> TENSOR
 *
 * Read element-first, each costs 2.6e8 reads; read length-first, one compare.
 *
 * And Grove's payload scan (issue #179, svc/nn/ethosu/npu_model_scan.cc), which
 * only verify_vela_model runs: one tensor [1, 4] as graph I/O, the CUSTOM
 * "ethos-u" operator code, and UINT8 1-D payload tensors over a 16-byte aligned
 * constant buffer that npu_payload.c accepts (COP1, NOP actions, one final
 * COMMAND_STREAM of length 0).  The operator's inputs are {payload, t0} and its
 * outputs {t0}.  The file is padded at its tail to an exact length where the
 * name says so (the verifier accepts trailing bytes):
 *
 *   pl_share2_eq_len.tflite     control: two operator entries naming ONE
 *                               operator over one 4,096 B payload, file length
 *                               exactly 2 x 4,096 -- the sum may equal it
 *   pl_share2_len_minus1.tflite the same, file length 2 x 4,096 - 1 -> PAYLOAD
 *   pl_share_4000.tflite        4,000 entries naming one operator over one
 *                               400,000 B payload of NOPs        -> PAYLOAD
 *   pl_index_1300.tflite        1,300 operators, each with its own tensor and
 *                               buffer INDEX, every buffers[] entry naming ONE
 *                               Buffer table of 380,000 B         -> PAYLOAD
 *   pl_single_whole.tflite      control: one operator, a payload 99.9 % of the
 *                               file
 *   pl_in1_neg1.tflite          inputs {payload, -1}             -> PAYLOAD
 *   pl_in1_ok.tflite            control: inputs {payload, t0}
 *   pl_in_65.tflite             inputs {payload, t0 x 64}, 65 in all: the scan
 *                               judges the length before the -1 check, because
 *                               the board runs it before the walk -> PAYLOAD
 *
 * The two "pl_share_4000" / "pl_index_1300" are the witnesses: walked once per
 * entry they cost 4e8 / 1.2e8 payload words; charged before the walk the second
 * entry is refused.
 *
 * Every file stays under 512 KB (checked here) and within the 4096 visits.
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
static_assert(NN_TFLM_MAX_OP_TENSORS == 64u,
              "op_in_64 / op_*_65 are named for a 64-entry operator list limit");
static_assert(NN_TFLM_MAX_TENSOR_RANK == 6u,
              "t_rank_6 / t_rank_7 are named for a rank-6 limit");

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
	/* issue #178 -- all zero / empty: the zero-operator model above. */
	uint32_t             op_entries;   /* operator entries, all naming ONE ADD */
	std::vector<int32_t> op_in;
	std::vector<int32_t> op_out;
	std::vector<int32_t> op_int;       /* empty: no intermediates vector */
	bool                 op_no_inputs;
	bool                 op_no_outputs;
	uint32_t             extra_entries; /* tensor entries naming ONE extra tensor */
	std::vector<int32_t> extra_shape;
	bool                 sg1_no_tensors; /* a second subgraph, no tensor vector */
	bool                 sg1_ok;         /* a second subgraph, one tensor, no ops */
};

/* The verifier's table visits for a model built by write_model(): every table
 * start, once per entry naming it. */
uint32_t tables_of(const spec &sp)
{
	const uint32_t nb = sp.buffers ? sp.buffers : 1u;

	return 2u /* Model, SubGraph */ + (sp.out_shape.empty() ? 1u : 2u) + nb +
	       sp.extra_entries + sp.op_entries +
	       (sp.op_entries ? 1u : 0u) /* the ADD OperatorCode */ +
	       (sp.sg1_no_tensors ? 1u : 0u) /* the second SubGraph */ +
	       (sp.sg1_ok ? 2u : 0u) /* the second SubGraph and its Tensor */;
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

	if (sp.extra_entries) {
		/* ONE table, named by every entry: the verifier visits it each time. */
		const auto extra = tflite::CreateTensor(
			fbb, fbb.CreateVector(sp.extra_shape), tflite::TensorType_INT8, 0,
			fbb.CreateString("internal"));

		for (uint32_t i = 0; i < sp.extra_entries; i++)
			tensors.push_back(extra);
	}

	typedef flatbuffers::Offset<flatbuffers::Vector<int32_t>> ivec;
	const auto tv   = fbb.CreateVector(tensors);
	const auto inv  = sp.no_inputs ? ivec() : fbb.CreateVector(sp.inputs);
	const auto outv = fbb.CreateVector(sp.outputs);
	std::vector<flatbuffers::Offset<tflite::Operator>> opv;
	std::vector<flatbuffers::Offset<tflite::OperatorCode>> codev;

	if (sp.op_entries) {
		const auto op = tflite::CreateOperator(
			fbb, 0 /* the ADD code below */,
			sp.op_no_inputs ? ivec() : fbb.CreateVector(sp.op_in),
			sp.op_no_outputs ? ivec() : fbb.CreateVector(sp.op_out),
			tflite::BuiltinOptions_NONE, 0, 0,
			tflite::CustomOptionsFormat_FLEXBUFFERS, 0,
			sp.op_int.empty() ? ivec() : fbb.CreateVector(sp.op_int));

		for (uint32_t i = 0; i < sp.op_entries; i++)
			opv.push_back(op);
		codev.push_back(tflite::CreateOperatorCode(
			fbb, (int8_t)tflite::BuiltinOperator_ADD, 0, 1, tflite::BuiltinOperator_ADD));
	}

	const auto ops  = fbb.CreateVector(opv);
	const auto sg   = tflite::CreateSubGraph(fbb, tv, inv, outv, ops, fbb.CreateString("main"));
	std::vector<flatbuffers::Offset<tflite::SubGraph>> sgv{ sg };

	if (sp.sg1_ok) {
		const auto t = tflite::CreateTensor(fbb, fbb.CreateVector(std::vector<int32_t>{ 1, 4 }),
		                                    tflite::TensorType_INT8, 0,
		                                    fbb.CreateString("sg1"));

		sgv.push_back(tflite::CreateSubGraph(
			fbb, fbb.CreateVector(std::vector<flatbuffers::Offset<tflite::Tensor>>{ t }),
			fbb.CreateVector(std::vector<int32_t>{ 0 }),
			fbb.CreateVector(std::vector<int32_t>{ 0 })));
	}
	if (sp.sg1_no_tensors)
		sgv.push_back(tflite::CreateSubGraph(fbb, 0, fbb.CreateVector(std::vector<int32_t>{}),
		                                     fbb.CreateVector(std::vector<int32_t>{})));
	const auto sgs  = fbb.CreateVector(sgv);
	const auto codes = fbb.CreateVector(codev);
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

	/* The largest file a gate takes is 512 KB: a witness above it would be
	 * refused for its length and prove nothing about the walk. */
	if (fbb.GetSize() > 512u * 1024u) {
		fprintf(stderr, "gen_model_io_fixtures: %s is %u bytes, over 512 KB\n",
		        sp.file, (unsigned)fbb.GetSize());
		return false;
	}
	FILE *f = fopen(path.c_str(), "wb");

	if (!f) {
		fprintf(stderr, "gen_model_io_fixtures: cannot write %s\n", path.c_str());
		return false;
	}
	const bool ok = fwrite(fbb.GetBufferPointer(), 1, fbb.GetSize(), f) == fbb.GetSize();
	return (fclose(f) == 0) && ok;
}

/* issue #179: an ethos-u model for Grove's payload scan (the list at the top). */
struct pl_spec {
	const char *file;
	uint32_t    entries;    /* operator entries */
	bool        distinct;   /* each entry its own operator, tensor and buffer
	                         * index; every buffers[] entry names ONE table */
	uint32_t    payload;    /* payload bytes, a multiple of 4, >= 8 */
	int32_t     in1;        /* the operator's input 1 */
	uint32_t    length;     /* the file's exact length (0: as built) */
	uint32_t    n_inputs;   /* inputs in all, the rest t0 (0: just the two) */
};

/* The verifier's table visits: Model, SubGraph, the I/O tensor, the payload
 * tensors, every operator entry, the OperatorCode, the empty Buffer 0 and every
 * payload buffers[] entry. */
uint32_t tables_of(const pl_spec &sp)
{
	const uint32_t k = sp.distinct ? sp.entries : 1u;

	return 2u + 1u + k + sp.entries + 1u + 1u + k;
}

bool write_payload_model(const std::string &dir, const pl_spec &sp)
{
	flatbuffers::DefaultAllocator alloc;   /* see write_model() */
	flatbuffers::FlatBufferBuilder fbb(1024, &alloc);

	/* COP1, NOPs, then one COMMAND_STREAM of length 0 as the last word. */
	std::vector<uint32_t> w(sp.payload / 4u, 5u /* NOP */);

	w.front() = 0x31504F43u;   /* "COP1" */
	w.back()  = 2u;            /* COMMAND_STREAM, length 0 */
	fbb.ForceVectorAlignment(sp.payload, 1, 16);
	const auto data = fbb.CreateVector(reinterpret_cast<const uint8_t *>(w.data()),
	                                   sp.payload);
	const auto payload_buf = tflite::CreateBuffer(fbb, data);
	const auto empty = tflite::CreateBuffer(fbb);
	const uint32_t k = sp.distinct ? sp.entries : 1u;
	std::vector<flatbuffers::Offset<tflite::Tensor>> tv;

	tv.push_back(tflite::CreateTensor(fbb, fbb.CreateVector(std::vector<int32_t>{ 1, 4 }),
	                                  tflite::TensorType_INT8, 0, fbb.CreateString("io")));
	const auto pshape = fbb.CreateVector(std::vector<int32_t>{ (int32_t)sp.payload });

	for (uint32_t i = 0; i < k; i++)
		tv.push_back(tflite::CreateTensor(fbb, pshape, tflite::TensorType_UINT8,
		                                  1u + i /* buffer index */));

	const auto outs = fbb.CreateVector(std::vector<int32_t>{ 0 });
	const auto marker = fbb.CreateVector(std::vector<uint8_t>{ 0 });
	auto op_for = [&](int32_t payload_tensor) {
		std::vector<int32_t> in{ payload_tensor, sp.in1 };

		while (in.size() < sp.n_inputs)
			in.push_back(0);
		return tflite::CreateOperator(fbb, 0, fbb.CreateVector(in), outs,
		                              tflite::BuiltinOptions_NONE, 0, marker);
	};
	std::vector<flatbuffers::Offset<tflite::Operator>> ov;

	if (sp.distinct) {
		for (uint32_t i = 0; i < sp.entries; i++)
			ov.push_back(op_for((int32_t)(1u + i)));
	} else {
		const auto op = op_for(1);

		for (uint32_t i = 0; i < sp.entries; i++)
			ov.push_back(op);
	}

	const auto sg = tflite::CreateSubGraph(
		fbb, fbb.CreateVector(tv), fbb.CreateVector(std::vector<int32_t>{ 0 }),
		fbb.CreateVector(std::vector<int32_t>{ 0 }), fbb.CreateVector(ov),
		fbb.CreateString("main"));
	std::vector<flatbuffers::Offset<tflite::SubGraph>> sgv{ sg };
	std::vector<flatbuffers::Offset<tflite::OperatorCode>> cv{ tflite::CreateOperatorCode(
		fbb, (int8_t)tflite::BuiltinOperator_CUSTOM, fbb.CreateString("ethos-u"), 1,
		tflite::BuiltinOperator_CUSTOM) };
	std::vector<flatbuffers::Offset<tflite::Buffer>> bv{ empty };

	for (uint32_t i = 0; i < k; i++)
		bv.push_back(payload_buf);   /* distinct: k indices, ONE Buffer table */
	const auto model = tflite::CreateModel(fbb, TFLITE_SCHEMA_VERSION, fbb.CreateVector(cv),
	                                       fbb.CreateVector(sgv),
	                                       fbb.CreateString("issue #179 fixture"),
	                                       fbb.CreateVector(bv));

	tflite::FinishModelBuffer(fbb, model);

	std::vector<uint8_t> file(fbb.GetBufferPointer(), fbb.GetBufferPointer() + fbb.GetSize());

	if (sp.length != 0u) {
		if (file.size() > sp.length) {
			fprintf(stderr, "gen_model_io_fixtures: %s is %zu bytes, over the %u "
			        "it is named for\n", sp.file, file.size(), (unsigned)sp.length);
			return false;
		}
		file.resize(sp.length, 0u);
	}
	if (file.size() > 512u * 1024u) {
		fprintf(stderr, "gen_model_io_fixtures: %s is %zu bytes, over 512 KB\n",
		        sp.file, file.size());
		return false;
	}

	const std::string path = dir + "/" + sp.file;
	FILE *f = fopen(path.c_str(), "wb");

	if (!f) {
		fprintf(stderr, "gen_model_io_fixtures: cannot write %s\n", path.c_str());
		return false;
	}
	const bool ok = fwrite(file.data(), 1, file.size(), f) == file.size();
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

	/* issue #178: the I/O of the control model, plus an operator and/or tensors. */
	const uint32_t opl = NN_TFLM_MAX_OP_TENSORS;
	const uint32_t rank = NN_TFLM_MAX_TENSOR_RANK;
	const uint32_t witness_entries = 4000u;   /* within the 4096 visits, see tables_of */
	auto graph = [](const char *file) {
		spec sp = { file, { 1, 4 }, { 1, 4 }, { 0 }, { 1 }, false };
		return sp;
	};
	auto with_op = [&](const char *file, std::vector<int32_t> in,
	                   std::vector<int32_t> out, std::vector<int32_t> inter) {
		spec sp = graph(file);
		sp.op_entries = 1u;
		sp.op_in = in;
		sp.op_out = out;
		sp.op_int = inter;
		return sp;
	};
	auto with_tensor = [&](const char *file, std::vector<int32_t> shape, uint32_t entries) {
		spec sp = graph(file);
		sp.extra_shape = shape;
		sp.extra_entries = entries;
		return sp;
	};
	std::vector<spec> graph_specs = {
		with_op("op_in_64.tflite",   std::vector<int32_t>(opl, 0), { 1 }, {}),
		with_op("op_out_64.tflite",  { 0 }, std::vector<int32_t>(opl, 1), {}),
		with_op("op_int_64.tflite",  { 0 }, { 1 }, std::vector<int32_t>(opl, 1)),
		with_op("op_in_65.tflite",   std::vector<int32_t>(opl + 1u, 0), { 1 }, {}),
		with_op("op_out_65.tflite",  { 0 }, std::vector<int32_t>(opl + 1u, 1), {}),
		with_op("op_int_65.tflite",  { 0 }, { 1 }, std::vector<int32_t>(opl + 1u, 1)),
		with_op("op_in_neg1.tflite", { 0, -1 }, { 1 }, {}),
		with_op("op_in_oob.tflite",  { 0, 2 }, { 1 }, {}),
		with_op("op_out_neg1.tflite", { 0 }, { -1 }, {}),
		with_op("op_int_neg1.tflite", { 0 }, { 1 }, { -1 }),
		with_op("op_int_oob.tflite", { 0 }, { 1 }, { 2 }),
		with_op("op_no_inputs.tflite", {}, { 1 }, {}),
		with_op("op_no_outputs.tflite", { 0 }, {}, {}),
		with_op("op_in_65000_shared.tflite", std::vector<int32_t>(65000, 0), { 1 }, {}),
		with_tensor("t_rank_6.tflite", std::vector<int32_t>(rank, 1), 1u),
		with_tensor("t_rank_7.tflite", std::vector<int32_t>(rank + 1u, 1), 1u),
		with_tensor("t_shape_65000_shared.tflite", std::vector<int32_t>(65000, 1),
		            witness_entries),
		graph("sg1_ok.tflite"),
		graph("sg1_no_tensors.tflite"),
	};
	/* The flags the constructors above do not take, set by name. */
	for (spec &sp : graph_specs) {
		const std::string f = sp.file;

		sp.op_no_inputs   = f == "op_no_inputs.tflite";
		sp.op_no_outputs  = f == "op_no_outputs.tflite";
		sp.sg1_ok         = f == "sg1_ok.tflite";
		sp.sg1_no_tensors = f == "sg1_no_tensors.tflite";
		if (f == "op_in_65000_shared.tflite")
			sp.op_entries = witness_entries;
	}

	for (const spec &sp : specs) {
		if (!write_model(argv[1], sp))
			return 1;
		printf("%s: %u table visits\n", sp.file, (unsigned)tables_of(sp));
	}
	for (const spec &sp : graph_specs) {
		if (tables_of(sp) > lim) {
			fprintf(stderr, "gen_model_io_fixtures: %s needs %u table visits, over %u\n",
			        sp.file, (unsigned)tables_of(sp), (unsigned)lim);
			return 1;
		}
		if (!write_model(argv[1], sp))
			return 1;
		printf("%s: %u table visits\n", sp.file, (unsigned)tables_of(sp));
	}

	/* issue #179: Grove's payload scan.  2 x 4,096 B leaves room for the rest of
	 * the file inside one payload's length, so the exact lengths are reachable. */
	const uint32_t p2 = 4096u;
	const pl_spec pl_specs[] = {
		{ "pl_share2_eq_len.tflite",     2u,    false, p2,       0, 2u * p2 },
		{ "pl_share2_len_minus1.tflite", 2u,    false, p2,       0, 2u * p2 - 1u },
		{ "pl_share_4000.tflite",        witness_entries, false, 400000u, 0, 0u },
		{ "pl_index_1300.tflite",        1300u, true,  380000u,  0, 0u },
		{ "pl_single_whole.tflite",      1u,    false, 399600u,  0, 400000u },
		{ "pl_in1_neg1.tflite",          1u,    false, p2,       -1, 0u },
		{ "pl_in1_ok.tflite",            1u,    false, p2,       0, 0u },
		{ "pl_in_65.tflite",             1u,    false, p2,       0, 0u, opl + 1u },
	};

	for (const pl_spec &sp : pl_specs) {
		if (tables_of(sp) > lim) {
			fprintf(stderr, "gen_model_io_fixtures: %s needs %u table visits, over %u\n",
			        sp.file, (unsigned)tables_of(sp), (unsigned)lim);
			return 1;
		}
		if (!write_payload_model(argv[1], sp))
			return 1;
		printf("%s: %u table visits\n", sp.file, (unsigned)tables_of(sp));
	}
	return 0;
}
