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
 * And the quantization lengths (issue #180), at NN_TFLM_MAX_QUANT_CHANNELS (4096)
 * and NN_TFLM_MAX_QUANT_REFS (65536): the control model plus quantized tensors
 * (t2, t3, ... after the I/O; a QuantizationParameters table is one more visit, so
 * a quantized tensor entry costs two) and ADD operators naming them.  Unless the
 * line says otherwise one tensor t2 of shape [4096] (or [4] for the short ones),
 * qd 0, named by one ADD {t2} -> {t1}:
 *
 *   q_scale_4096.tflite     control: scale 4096 long, zero point 1 -- the cap
 *   q_scale_4097.tflite     scale 4097 long                         -> FORMAT
 *   q_zp_4097.tflite        scale 4096, zero point 4097 long        -> FORMAT
 *   q_zp_1_of_4.tflite      control: scale 4, zero point 1 (the weights' form)
 *   q_zp_2_of_4.tflite      scale 4, zero point 2                   -> FORMAT
 *   q_scale_gt_dim.tflite   control: shape [4], scale and zero point 8 long --
 *                           the walk does NOT compare the length with dims[qd]
 *                           (the kernels do; a decision, pinned here)
 *   q_qd_5.tflite           control: shape [4] (rank 1), scale 4, qd 5 -- on a
 *                           rank 0 / 1 tensor qd is bounded by
 *                           NN_TFLM_MAX_TENSOR_RANK, not by the rank (TFLM's
 *                           person_detect has rank-1 per-channel biases with qd
 *                           3; a decision, pinned)
 *   q_qd_6.tflite           the same with qd 6                      -> FORMAT
 *   q_qd_neg.tflite         the same with qd -1                     -> FORMAT
 *   q_qd_rank2_1.tflite     control: shape [1, 4] (rank 2), scale 4, qd 1
 *   q_qd_rank2_2.tflite     the same with qd 2 == the rank          -> FORMAT
 *   q_scale1_qd_100.tflite  control: shape [4], scale 1 long, qd 100 -- the qd
 *                           rule applies to per-channel tensors only
 *   q_refs_65536.tflite     control: t2 (scale 4096) named twice by each of 8
 *                           ADD entries -- the sum exactly 65536 -- and one ADD
 *                           {t0} -> {t1} (t0 unquantized); t3 has scale 1
 *   q_refs_65537.tflite     the last ADD is {t3} -> {t1}: one more  -> FORMAT
 *
 * And two witnesses of the per-channel work bound, built the way that defeated the
 * verifier and the #178 walk (scalar vectors, and entries sharing one table):
 *
 *   q_scale_65000_shared.tflite  2000 tensor entries naming ONE tensor whose
 *                                scale vector is 65,000 long        -> FORMAT
 *   q_refs_4000_shared.tflite    4000 ADD entries naming one tensor of scale 32
 *                                (each passes the cap, the sum 128,000 does not)
 *                                                                   -> FORMAT
 *
 * And the buffer indices and the offline plan (issue #181): the control model
 * (buffer 0 empty, both tensors on it) with one thing changed.  A metadata entry
 * gets a Buffer of its own appended after the model's (a Metadata table and its
 * Buffer are two more visits); "plan(n, k)" is an OfflineMemoryAllocation
 * buffer of words {0, 1, n, k offsets of -1}, 12 + 4k bytes, and the graph has 2
 * tensors (3 where a second subgraph is added):
 *
 *   t_buffer_last.tflite         control: 2 buffers, t0 on buffer 1 (nb - 1)
 *   t_buffer_oob.tflite          t0 on buffer 2 == nb                -> FORMAT
 *   t_buffer_oob_sg1.tflite      a second subgraph's tensor on buffer nb
 *                                                                    -> FORMAT
 *   t_no_buffers.tflite          no buffer vector, t0 on buffer 0    -> FORMAT
 *   md_buffer_oob.tflite         "min_runtime_version" on buffer nb  -> FORMAT
 *   md_other_name.tflite         control: "min_runtime_version", 1 byte of data
 *                                (only the plan's name is parsed)
 *   md_offline_ok.tflite         control: plan(2, 2), 20 bytes
 *   md_offline_name_24.tflite    control: "OfflineMemoryAllocationX", 1 byte
 *                                (TFLM compares the length too; so does the walk)
 *   md_offline_short.tflite      plan(2, 1), 16 bytes                -> FORMAT
 *   md_offline_hdr_short.tflite  8 bytes, no word [2]                -> FORMAT
 *   md_offline_null.tflite       the plan's buffer has no data       -> FORMAT
 *   md_offline_buffer_oob.tflite the plan on buffer nb               -> FORMAT
 *   md_offline_nbr_lt.tflite     plan(1, 1)                          -> FORMAT
 *   md_offline_nbr_gt.tflite     plan(3, 3)                          -> FORMAT
 *   md_offline_nbr_neg.tflite    plan(-1, 2)                         -> FORMAT
 *                                (the walk's negative guard is redundant here:
 *                                the 64-bit compare with the tensor count
 *                                refuses -1 too; the guard is kept for clarity)
 *   md_offline_off_neg2.tflite   plan(2, 2) with offset 0 = -2: TFLM's planner
 *                                takes only -1 as "online"         -> FORMAT
 *   md_offline_off_min.tflite    plan(2, 2) with offset 1 = INT32_MIN -> FORMAT
 *   md_offline_off_pos.tflite    control: plan(2, 2) with offsets 0, 16
 *                                (md_offline_ok's are both -1)
 *   md_offline_total_3.tflite    control: a second subgraph (1 tensor), plan(3, 3)
 *                                -- TFLM compares with every subgraph's tensors
 *   md_offline_sg0_only.tflite   the same with plan(2, 2), subgraph 0's count
 *                                                                    -> FORMAT
 *   md_offline_second_bad.tflite plan(2, 2), then a second plan(2, 1): TFLM reads
 *                                every matching entry                -> FORMAT
 *   codes_null.tflite            no operator-code vector (and no operator, so
 *                                nothing else is wrong)              -> FORMAT
 *
 * And the constant buffers (issue #183): the control model with one tensor
 * added (or t0 / t1 changed), each added tensor on a Buffer of its own whose data
 * is 16-byte aligned and N bytes long ("int8 [2, 3] 6 B"), and, where it says
 * so, named by an ADD operator; all refusals FORMAT:
 *
 *   c_int8_eq.tflite             control: int8 [2, 3] 6 B
 *   c_types_eq.tflite            control: one [3] constant of every type with a
 *                                size (FLOAT16 .. COMPLEX128, BOOL, RESOURCE)
 *   c_int8_short / _long         int8 [2, 3] 5 B / 7 B (the equality is a policy)
 *   c_int32_short.tflite         int32 [2] 2 B: the element size is multiplied
 *                                (2 B is the element COUNT, so a rule that
 *                                forgot the size would pass it)
 *   c_scalar_null / _empty       controls: int32 with no shape / shape [], 4 B
 *   c_zero.tflite                int8 [0, 3] 1 B (refused by the equality)
 *   c_neg.tflite                 int8 [-1, 3] 3 B (the walk's negative guard is
 *                                redundant below 4 GB of data: as unsigned, -1 is
 *                                past every cap; the guard is kept for clarity)
 *   c_int4_packed / _even        controls: int4 [3] 2 B / [4] 2 B
 *   c_int4_unpacked / _short     int4 [3] 3 B / [5] 2 B
 *   c_string.tflite              a STRING [1] constant, 1 B (no size)
 *   c_string_nonconst.tflite     a STRING [1] tensor without data: every
 *                                tensor's type is judged
 *   c_resource_nonconst.tflite   control: RESOURCE [1] without data
 *   c_wrap32.tflite              int8 [65537, 65537] 131,073 B: the product
 *                                wraps to exactly that in 32 bits
 *   c_rank7_eq / _short          int8 of rank 7, 2 B / 1 B: a constant's rank is
 *                                judged before its values (FORMAT, not TENSOR)
 *   c_sparse.tflite              int8 [4, 4] with a CSR sparsity table, 6 B
 *                                compressed: TFLM micro reads it dense
 *   c_graph_in / _graph_out      t0 / t1 with 4 B of data (equal)
 *   c_op_out.tflite              an ADD whose output has 4 B of data (equal)
 *   c_alias_in_out.tflite        that tensor is also the graph's input
 *   c_io_oob_and_data.tflite     outputs = [99] and a constant 1 B short: two
 *                                faults, both FORMAT, whatever the order
 *   c_var_eq.tflite              control: an is_variable constant (equal) an
 *                                ADD reads
 *   c_sg1_short.tflite           sg1_ok's second-subgraph tensor 1 B short
 *   c_empty_data.tflite          control: int8 [2, 3] on a Buffer whose data
 *                                vector is 0 long (TFLM: not a constant)
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
 *   pl_const_short.tflite       pl_in1_ok plus an int8 [8] constant of 4 B: the
 *                               scan passes it, the walk refuses -> FORMAT
 *                               (issue #183, in either order)
 *   pl_in2_nodata.tflite        control: inputs {payload, t0, t0} -- input 2
 *                               (the NPU's arena base pointer) has no data
 *   pl_in2_data.tflite          input 2 an int8 [4] tensor with 4 B of data
 *                               (exact, so the walk passes it) -> PAYLOAD
 *                               (issue #183, in either order: the NPU writes
 *                               through it, into the model)
 *   pl_in2_oob.tflite           input 2 names tensor 999      -> PAYLOAD
 *                               (the scan runs before the walk on the board)
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
              "t_rank_6 / t_rank_7 and q_qd_5 / q_qd_6 are named for a rank-6 limit");
static_assert(NN_TFLM_MAX_QUANT_CHANNELS == 4096u,
              "q_scale_4096 / q_scale_4097 / q_zp_4097 are named for a 4096 cap");
static_assert(NN_TFLM_MAX_QUANT_REFS == 65536u,
              "q_refs_65536 / q_refs_65537 are named for a 65536 sum");

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace {

/* issue #180: a quantized tensor, named by `entries` tensor entries (one table). */
struct qtensor {
	std::vector<int32_t> shape;
	uint32_t             scale_len;
	uint32_t             zp_len;
	int32_t              qd;
	uint32_t             entries;
};

/* issue #181: one metadata entry.  Unless `oob`, it names a Buffer of its own
 * (with `data`, or with no data vector when `no_data`); `oob` names buffer nb. */
struct mddef {
	std::string          name;
	std::vector<uint8_t> data;
	bool                 no_data;
	bool                 oob;
};

/* issue #181: an OfflineMemoryAllocation buffer, words {0, 1, n, k x -1}, the
 * first offsets replaced by @p first. */
std::vector<uint8_t> plan(int32_t n, uint32_t k, std::vector<int32_t> first = {})
{
	std::vector<int32_t> w{ 0, 1, n };

	w.resize(3u + k, -1);
	for (size_t i = 0; i < first.size() && 3u + i < w.size(); i++)
		w[3u + i] = first[i];
	return std::vector<uint8_t>(reinterpret_cast<const uint8_t *>(w.data()),
	                            reinterpret_cast<const uint8_t *>(w.data() + w.size()));
}

/* issue #183: one tensor added after the others.  data_len > 0 gives it a
 * Buffer of its own with that many bytes; empty_data a Buffer whose data vector
 * is 0 long; neither, buffer 0 (no data).  op_in / op_out add an ADD reading /
 * writing it, graph_in makes it subgraph 0's only input. */
struct ctensor {
	tflite::TensorType   type;
	std::vector<int32_t> shape;
	bool                 no_shape;
	uint32_t             data_len;
	bool                 empty_data;
	bool                 op_in;
	bool                 op_out;
	bool                 graph_in;
	bool                 is_variable;
	bool                 sparse;
};

/* issue #180: one more ADD operator table, named by `entries` operator entries. */
struct opdef {
	std::vector<int32_t> in;
	std::vector<int32_t> out;
	uint32_t             entries;
};

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
	/* issue #180 -- after the extra tensor entries, and after the ADD above. */
	std::vector<qtensor> qt;
	std::vector<opdef>   more_ops;
	/* issue #181 -- all zero / empty: buffer 0 everywhere, no metadata. */
	uint32_t             t0_buffer;     /* the input tensor's buffer index */
	uint32_t             sg1_buffer;    /* the sg1_ok tensor's (UINT32_MAX: nb) */
	bool                 no_buffers;    /* no buffer vector at all */
	bool                 no_codes;      /* no operator-code vector at all */
	std::vector<mddef>   md;
	/* issue #183 -- all zero / empty: no data anywhere. */
	std::vector<ctensor> ct;            /* after the quantized tensors */
	uint32_t             t0_data;       /* t0 / t1 / the sg1_ok tensor on a */
	uint32_t             t1_data;       /* Buffer of their own with this */
	uint32_t             sg1_data;      /* many bytes (0: buffer 0) */
};

/* issue #183: the index of the first added constant. */
uint32_t ct_base(const spec &sp)
{
	uint32_t n = (sp.out_shape.empty() ? 1u : 2u) + sp.extra_entries;

	for (const qtensor &t : sp.qt)
		n += t.entries;
	return n;
}

/* issue #183: Buffers of their own (a data vector, maybe 0 long). */
uint32_t ct_buffers_of(const spec &sp)
{
	uint32_t n = (sp.t0_data ? 1u : 0u) + (sp.t1_data ? 1u : 0u) + (sp.sg1_data ? 1u : 0u);

	for (const ctensor &c : sp.ct)
		n += (c.data_len || c.empty_data) ? 1u : 0u;
	return n;
}

/* issue #183: the sparsity tables (SparsityParameters, 2 DimensionMetadata, 2
 * Int32Vector). */
uint32_t ct_sparse_tables_of(const spec &sp)
{
	uint32_t n = 0;

	for (const ctensor &c : sp.ct)
		n += c.sparse ? 5u : 0u;
	return n;
}

/* issue #181: the metadata entries that bring a Buffer of their own. */
uint32_t md_buffers_of(const spec &sp)
{
	uint32_t n = 0;

	for (const mddef &m : sp.md)
		n += m.oob ? 0u : 1u;
	return n;
}

/* Operator entries in all, of any operator table. */
uint32_t op_entries_of(const spec &sp)
{
	uint32_t n = sp.op_entries;

	for (const opdef &o : sp.more_ops)
		n += o.entries;
	return n;
}

/* The verifier's table visits for a model built by write_model(): every table
 * start, once per entry naming it. */
uint32_t tables_of(const spec &sp)
{
	const uint32_t nb = sp.no_buffers ? 0u : (sp.buffers ? sp.buffers : 1u);

	uint32_t q = 0;

	for (const qtensor &t : sp.qt)
		q += 2u * t.entries;   /* the Tensor and its QuantizationParameters */
	return 2u /* Model, SubGraph */ + (sp.out_shape.empty() ? 1u : 2u) + nb +
	       sp.extra_entries + op_entries_of(sp) + q +
	       (op_entries_of(sp) ? 1u : 0u) /* the ADD OperatorCode */ +
	       (sp.sg1_no_tensors ? 1u : 0u) /* the second SubGraph */ +
	       (sp.sg1_ok ? 2u : 0u) /* the second SubGraph and its Tensor */ +
	       (uint32_t)sp.md.size() + md_buffers_of(sp) /* Metadata, own Buffer */ +
	       (uint32_t)sp.ct.size() + ct_buffers_of(sp) + ct_sparse_tables_of(sp);
}

/* issue #183: a data vector of @p n bytes, 16-byte aligned like a converter's. */
flatbuffers::Offset<flatbuffers::Vector<uint8_t>> const_data(flatbuffers::FlatBufferBuilder &fbb,
                                                             uint32_t n)
{
	fbb.ForceVectorAlignment(n, 1, 16);
	return fbb.CreateVector(std::vector<uint8_t>(n, 0x11u));
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

	/* issue #183: the Buffers of their own come after the model's base buffers
	 * and before the metadata's, in this order: t0, t1, sg1, then each added
	 * tensor with data. */
	const uint32_t nb_base = sp.no_buffers ? 0u : (sp.buffers ? sp.buffers : 1u);
	std::vector<flatbuffers::Offset<tflite::Buffer>> own;
	auto own_buffer = [&](uint32_t n, bool empty) {
		const auto d = empty ? fbb.CreateVector(std::vector<uint8_t>{}) : const_data(fbb, n);

		own.push_back(tflite::CreateBuffer(fbb, d));
		return nb_base + (uint32_t)own.size() - 1u;
	};
	const uint32_t t0_buf = sp.t0_data ? own_buffer(sp.t0_data, false) : sp.t0_buffer;

	tensors.push_back(tflite::CreateTensor(
		fbb, fbb.CreateVector(sp.in_shape), tflite::TensorType_INT8, t0_buf,
		fbb.CreateString("in")));
	if (!sp.out_shape.empty()) {
		const uint32_t t1_buf = sp.t1_data ? own_buffer(sp.t1_data, false) : 0u;

		tensors.push_back(tflite::CreateTensor(
			fbb, fbb.CreateVector(sp.out_shape), tflite::TensorType_INT8, t1_buf,
			fbb.CreateString("out")));
	}
	const uint32_t sg1_buf = sp.sg1_data ? own_buffer(sp.sg1_data, false) : UINT32_MAX - 1u;

	if (sp.extra_entries) {
		/* ONE table, named by every entry: the verifier visits it each time. */
		const auto extra = tflite::CreateTensor(
			fbb, fbb.CreateVector(sp.extra_shape), tflite::TensorType_INT8, 0,
			fbb.CreateString("internal"));

		for (uint32_t i = 0; i < sp.extra_entries; i++)
			tensors.push_back(extra);
	}

	for (const qtensor &qt : sp.qt) {
		/* ONE Tensor table (and so one scale vector) for all its entries. */
		const auto q = tflite::CreateQuantizationParameters(
			fbb, 0, 0, fbb.CreateVector(std::vector<float>(qt.scale_len, 1.0f)),
			fbb.CreateVector(std::vector<int64_t>(qt.zp_len, 0)),
			tflite::QuantizationDetails_NONE, 0, qt.qd);
		const auto t = tflite::CreateTensor(
			fbb, fbb.CreateVector(qt.shape), tflite::TensorType_INT8, 0,
			fbb.CreateString("quantized"), q);

		for (uint32_t i = 0; i < qt.entries; i++)
			tensors.push_back(t);
	}

	for (const ctensor &c : sp.ct) {
		typedef flatbuffers::Offset<flatbuffers::Vector<int32_t>> svec;
		const uint32_t bi = (c.data_len || c.empty_data) ? own_buffer(c.data_len, c.empty_data)
		                                                 : 0u;
		flatbuffers::Offset<tflite::SparsityParameters> sparsity;

		if (c.sparse) {
			/* [4, 4] as CSR: row 0 dense, column indices of 6 non-zeros. */
			const auto seg = tflite::CreateInt32Vector(
				fbb, fbb.CreateVector(std::vector<int32_t>{ 0, 2, 3, 5, 6 }));
			const auto idx = tflite::CreateInt32Vector(
				fbb, fbb.CreateVector(std::vector<int32_t>{ 0, 1, 2, 0, 3, 1 }));
			const std::vector<flatbuffers::Offset<tflite::DimensionMetadata>> dm{
				tflite::CreateDimensionMetadata(fbb, tflite::DimensionType_DENSE, 4),
				tflite::CreateDimensionMetadata(
					fbb, tflite::DimensionType_SPARSE_CSR, 0,
					tflite::SparseIndexVector_Int32Vector, seg.Union(),
					tflite::SparseIndexVector_Int32Vector, idx.Union()) };

			sparsity = tflite::CreateSparsityParameters(
				fbb, fbb.CreateVector(std::vector<int32_t>{ 0, 1 }), 0,
				fbb.CreateVector(dm));
		}
		tensors.push_back(tflite::CreateTensor(
			fbb, c.no_shape ? svec() : fbb.CreateVector(c.shape), c.type, bi,
			fbb.CreateString("const"), 0, c.is_variable, sparsity));
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
	}
	for (const opdef &o : sp.more_ops) {
		const auto op = tflite::CreateOperator(fbb, 0 /* ADD */, fbb.CreateVector(o.in),
		                                       fbb.CreateVector(o.out));

		for (uint32_t i = 0; i < o.entries; i++)
			opv.push_back(op);
	}
	if (op_entries_of(sp))
		codev.push_back(tflite::CreateOperatorCode(
			fbb, (int8_t)tflite::BuiltinOperator_ADD, 0, 1, tflite::BuiltinOperator_ADD));

	const auto ops  = fbb.CreateVector(opv);
	const auto sg   = tflite::CreateSubGraph(fbb, tv, inv, outv, ops, fbb.CreateString("main"));
	std::vector<flatbuffers::Offset<tflite::SubGraph>> sgv{ sg };

	/* The model's buffer count: the base buffers, the tensors' own (issue #183),
	 * then one per metadata entry that brings its own (issue #181). */
	const uint32_t nb = nb_base;
	const uint32_t nb_all = nb + (uint32_t)own.size() + md_buffers_of(sp);

	if (sp.sg1_ok) {
		const uint32_t b1 = sp.sg1_buffer == UINT32_MAX ? nb_all
		                    : sp.sg1_data ? sg1_buf : sp.sg1_buffer;
		const auto t = tflite::CreateTensor(fbb, fbb.CreateVector(std::vector<int32_t>{ 1, 4 }),
		                                    tflite::TensorType_INT8, b1,
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
	typedef flatbuffers::Offset<flatbuffers::Vector<flatbuffers::Offset<tflite::OperatorCode>>>
		cvec;
	const auto codes = sp.no_codes ? cvec() : fbb.CreateVector(codev);
	std::vector<flatbuffers::Offset<tflite::Buffer>> bv;
	const auto one = tflite::CreateBuffer(fbb);

	for (uint32_t i = 0; i < nb; i++)
		bv.push_back((sp.shared || i == 0) ? one : tflite::CreateBuffer(fbb));
	bv.insert(bv.end(), own.begin(), own.end());

	std::vector<flatbuffers::Offset<tflite::Metadata>> mdv;

	for (const mddef &m : sp.md) {
		uint32_t idx = nb_all;   /* oob: one past the last buffer */

		if (!m.oob) {
			flatbuffers::Offset<flatbuffers::Vector<uint8_t>> d;

			if (!m.no_data) {
				fbb.ForceVectorAlignment(m.data.size(), 1, 16);
				d = fbb.CreateVector(m.data);
			}
			idx = (uint32_t)bv.size();
			bv.push_back(tflite::CreateBuffer(fbb, d));
		}
		mdv.push_back(tflite::CreateMetadata(fbb, fbb.CreateString(m.name), idx));
	}

	typedef flatbuffers::Offset<flatbuffers::Vector<flatbuffers::Offset<tflite::Buffer>>> bvec;
	typedef flatbuffers::Offset<flatbuffers::Vector<flatbuffers::Offset<tflite::Metadata>>>
		mvec;
	const auto bufs = sp.no_buffers ? bvec() : fbb.CreateVector(bv);
	const auto mds  = sp.md.empty() ? mvec() : fbb.CreateVector(mdv);
	const auto model = tflite::CreateModel(fbb, TFLITE_SCHEMA_VERSION, codes, sgs,
	                                       fbb.CreateString("issue #175 fixture"), bufs,
	                                       0, mds);

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
	uint32_t    const_len;  /* issue #183: an int8 [8] constant of this many
	                         * bytes, named by no operator (0: none) */
	int         in2;        /* issue #183: input 2 -- 0 none, 1 t0 (no data),
	                         * 2 an int8 [4] tensor with 4 B of data, 3 a
	                         * tensor index past the vector */
};

/* The verifier's table visits: Model, SubGraph, the I/O tensor, the payload
 * tensors, every operator entry, the OperatorCode, the empty Buffer 0 and every
 * payload buffers[] entry. */
uint32_t tables_of(const pl_spec &sp)
{
	const uint32_t k = sp.distinct ? sp.entries : 1u;

	return 2u + 1u + k + sp.entries + 1u + 1u + k + (sp.const_len ? 2u : 0u) +
	       (sp.in2 == 2 ? 2u : 0u);
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
	flatbuffers::Offset<tflite::Buffer> const_buf;

	if (sp.const_len) {
		const_buf = tflite::CreateBuffer(fbb, const_data(fbb, sp.const_len));
		tv.push_back(tflite::CreateTensor(fbb, fbb.CreateVector(std::vector<int32_t>{ 8 }),
		                                  tflite::TensorType_INT8, 1u + k,
		                                  fbb.CreateString("const")));
	}
	flatbuffers::Offset<tflite::Buffer> in2_buf;
	int32_t in2_index = 0;

	if (sp.in2 == 2) {
		in2_buf = tflite::CreateBuffer(fbb, const_data(fbb, 4u));
		in2_index = (int32_t)tv.size();
		tv.push_back(tflite::CreateTensor(fbb, fbb.CreateVector(std::vector<int32_t>{ 4 }),
		                                  tflite::TensorType_INT8,
		                                  1u + k + (sp.const_len ? 1u : 0u),
		                                  fbb.CreateString("scratch")));
	} else if (sp.in2 == 3) {
		in2_index = 999;
	}

	const auto outs = fbb.CreateVector(std::vector<int32_t>{ 0 });
	const auto marker = fbb.CreateVector(std::vector<uint8_t>{ 0 });
	auto op_for = [&](int32_t payload_tensor) {
		std::vector<int32_t> in{ payload_tensor, sp.in1 };

		if (sp.in2)
			in.push_back(in2_index);   /* 1: t0 */

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
	if (sp.const_len)
		bv.push_back(const_buf);
	if (sp.in2 == 2)
		bv.push_back(in2_buf);
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
	/* issue #180: quantized tensors t2 (and t3) named by ADD operators. */
	const uint32_t qcap = NN_TFLM_MAX_QUANT_CHANNELS;
	const uint32_t qrefs = NN_TFLM_MAX_QUANT_REFS;
	auto quant = [&](const char *file, std::vector<int32_t> shape, uint32_t sl,
	                 uint32_t zl, int32_t qd) {
		spec sp = graph(file);
		sp.qt = { { shape, sl, zl, qd, 1u } };
		sp.more_ops = { { { 2 }, { 1 }, 1u } };
		return sp;
	};
	/* 8 entries x {t2, t2} x 4096 == the sum's limit; the last ADD adds t3's 1 or
	 * t0's nothing. */
	auto refs = [&](const char *file, int32_t last_in) {
		spec sp = graph(file);
		sp.qt = { { { (int32_t)qcap }, qcap, 1u, 0, 1u },
		          { { 1 }, 1u, 1u, 0, 1u } };
		sp.more_ops = { { { 2, 2 }, { 1 }, qrefs / (2u * qcap) },
		                { { last_in }, { 1 }, 1u } };
		return sp;
	};
	const std::vector<int32_t> q4096 = { (int32_t)qcap };
	const std::vector<spec> quant_specs = {
		quant("q_scale_4096.tflite",   q4096, qcap,      1u,        0),
		quant("q_scale_4097.tflite",   q4096, qcap + 1u, 1u,        0),
		quant("q_zp_4097.tflite",      q4096, qcap,      qcap + 1u, 0),
		quant("q_zp_1_of_4.tflite",    { 4 }, 4u,        1u,        0),
		quant("q_zp_2_of_4.tflite",    { 4 }, 4u,        2u,        0),
		quant("q_scale_gt_dim.tflite", { 4 }, 8u,        8u,        0),
		quant("q_qd_5.tflite",         { 4 }, 4u,        4u,        (int32_t)rank - 1),
		quant("q_qd_6.tflite",         { 4 }, 4u,        4u,        (int32_t)rank),
		quant("q_qd_neg.tflite",       { 4 }, 4u,        4u,        -1),
		quant("q_qd_rank2_1.tflite",   { 1, 4 }, 4u,     4u,        1),
		quant("q_qd_rank2_2.tflite",   { 1, 4 }, 4u,     4u,        2),
		quant("q_scale1_qd_100.tflite", { 4 }, 1u,       1u,        100),
		refs("q_refs_65536.tflite", 0),
		refs("q_refs_65537.tflite", 3),
	};
	graph_specs.insert(graph_specs.end(), quant_specs.begin(), quant_specs.end());
	{
		spec sp = graph("q_scale_65000_shared.tflite");
		sp.qt = { { { 4 }, 65000u, 1u, 0, 2000u } };
		graph_specs.push_back(sp);
	}
	{
		spec sp = graph("q_refs_4000_shared.tflite");
		sp.qt = { { { 32 }, 32u, 1u, 0, 1u } };
		sp.more_ops = { { { 2 }, { 1 }, witness_entries } };
		static_assert(4000u * 32u > NN_TFLM_MAX_QUANT_REFS &&
		              32u <= NN_TFLM_MAX_QUANT_CHANNELS,
		              "q_refs_4000_shared: each tensor passes the cap, the sum does not");
		graph_specs.push_back(sp);
	}

	/* issue #181: buffer indices and the offline plan. */
	{
		const std::string off = "OfflineMemoryAllocation";
		const std::string mrv = "min_runtime_version";
		auto md = [&](const char *file, std::vector<mddef> entries) {
			spec sp = graph(file);
			sp.md = entries;
			return sp;
		};
		spec sp = graph("t_buffer_last.tflite");
		sp.buffers = 2u;
		sp.t0_buffer = 1u;
		graph_specs.push_back(sp);
		sp = graph("t_buffer_oob.tflite");
		sp.buffers = 2u;
		sp.t0_buffer = 2u;
		graph_specs.push_back(sp);
		sp = graph("t_buffer_oob_sg1.tflite");
		sp.sg1_buffer = UINT32_MAX;
		graph_specs.push_back(sp);
		sp = graph("t_no_buffers.tflite");
		sp.no_buffers = true;
		graph_specs.push_back(sp);
		graph_specs.push_back(md("md_buffer_oob.tflite", { { mrv, {}, false, true } }));
		graph_specs.push_back(md("md_other_name.tflite", { { mrv, { 1 }, false, false } }));
		graph_specs.push_back(md("md_offline_ok.tflite", { { off, plan(2, 2u), false, false } }));
		graph_specs.push_back(md("md_offline_name_24.tflite",
		                         { { off + "X", { 1 }, false, false } }));
		graph_specs.push_back(md("md_offline_short.tflite",
		                         { { off, plan(2, 1u), false, false } }));
		graph_specs.push_back(md("md_offline_hdr_short.tflite",
		                         { { off, { 0, 0, 0, 0, 1, 0, 0, 0 }, false, false } }));
		graph_specs.push_back(md("md_offline_null.tflite", { { off, {}, true, false } }));
		graph_specs.push_back(md("md_offline_buffer_oob.tflite", { { off, {}, false, true } }));
		graph_specs.push_back(md("md_offline_nbr_lt.tflite",
		                         { { off, plan(1, 1u), false, false } }));
		graph_specs.push_back(md("md_offline_nbr_gt.tflite",
		                         { { off, plan(3, 3u), false, false } }));
		graph_specs.push_back(md("md_offline_nbr_neg.tflite",
		                         { { off, plan(-1, 2u), false, false } }));
		graph_specs.push_back(md("md_offline_total_3.tflite",
		                         { { off, plan(3, 3u), false, false } }));
		graph_specs.push_back(md("md_offline_sg0_only.tflite",
		                         { { off, plan(2, 2u), false, false } }));
		graph_specs.push_back(md("md_offline_second_bad.tflite",
		                         { { off, plan(2, 2u), false, false },
		                           { off, plan(2, 1u), false, false } }));
		graph_specs.push_back(md("md_offline_off_neg2.tflite",
		                         { { off, plan(2, 2u, { -2 }), false, false } }));
		graph_specs.push_back(md("md_offline_off_min.tflite",
		                         { { off, plan(2, 2u, { -1, INT32_MIN }), false, false } }));
		graph_specs.push_back(md("md_offline_off_pos.tflite",
		                         { { off, plan(2, 2u, { 0, 16 }), false, false } }));
		sp = graph("codes_null.tflite");
		sp.no_codes = true;
		graph_specs.push_back(sp);
	}

	/* issue #183: constant buffers.  c(type, shape, bytes) is one added tensor
	 * (on a Buffer of its own when bytes > 0), read by an ADD into t1. */
	{
		auto c = [](tflite::TensorType type, std::vector<int32_t> shape, uint32_t bytes) {
			ctensor t = { type, shape, false, bytes, false, true, false, false, false,
			              false };
			return t;
		};
		auto one = [&](const char *file, ctensor t) {
			spec sp = graph(file);
			sp.ct = { t };
			return sp;
		};
		const tflite::TensorType I8 = tflite::TensorType_INT8;
		const tflite::TensorType I4 = tflite::TensorType_INT4;
		const std::vector<int32_t> r7 = { 1, 1, 1, 1, 1, 1, 2 };
		static_assert(NN_TFLM_MAX_TENSOR_RANK + 1u == 7u, "c_rank7_* are rank 7");

		graph_specs.push_back(one("c_int8_eq.tflite", c(I8, { 2, 3 }, 6u)));
		{
			/* Every type with a size, [3] each: bytes = 3 x its size. */
			const struct { tflite::TensorType t; uint32_t sz; } ty[] = {
				{ tflite::TensorType_FLOAT16, 2 },  { tflite::TensorType_BFLOAT16, 2 },
				{ tflite::TensorType_INT16, 2 },    { tflite::TensorType_UINT16, 2 },
				{ tflite::TensorType_FLOAT32, 4 },  { tflite::TensorType_INT32, 4 },
				{ tflite::TensorType_UINT32, 4 },   { tflite::TensorType_RESOURCE, 4 },
				{ tflite::TensorType_FLOAT64, 8 },  { tflite::TensorType_INT64, 8 },
				{ tflite::TensorType_UINT64, 8 },   { tflite::TensorType_COMPLEX64, 8 },
				{ tflite::TensorType_COMPLEX128, 16 }, { tflite::TensorType_UINT8, 1 },
				{ tflite::TensorType_INT8, 1 },     { tflite::TensorType_BOOL, 1 },
			};
			spec sp = graph("c_types_eq.tflite");

			for (const auto &e : ty) {
				ctensor t = c(e.t, { 3 }, 3u * e.sz);

				t.op_in = false;   /* no ADD takes a COMPLEX128 */
				sp.ct.push_back(t);
			}
			graph_specs.push_back(sp);
		}
		graph_specs.push_back(one("c_int8_short.tflite", c(I8, { 2, 3 }, 5u)));
		graph_specs.push_back(one("c_int8_long.tflite", c(I8, { 2, 3 }, 7u)));
		graph_specs.push_back(one("c_int32_short.tflite",
		                          c(tflite::TensorType_INT32, { 2 }, 2u)));
		{
			ctensor t = c(tflite::TensorType_INT32, {}, 4u);

			t.no_shape = true;
			graph_specs.push_back(one("c_scalar_null.tflite", t));
		}
		graph_specs.push_back(one("c_scalar_empty.tflite",
		                          c(tflite::TensorType_INT32, {}, 4u)));
		graph_specs.push_back(one("c_zero.tflite", c(I8, { 0, 3 }, 1u)));
		graph_specs.push_back(one("c_neg.tflite", c(I8, { -1, 3 }, 3u)));
		graph_specs.push_back(one("c_int4_packed.tflite", c(I4, { 3 }, 2u)));
		graph_specs.push_back(one("c_int4_even.tflite", c(I4, { 4 }, 2u)));
		graph_specs.push_back(one("c_int4_unpacked.tflite", c(I4, { 3 }, 3u)));
		graph_specs.push_back(one("c_int4_short.tflite", c(I4, { 5 }, 2u)));
		graph_specs.push_back(one("c_string.tflite", c(tflite::TensorType_STRING, { 1 }, 1u)));
		{
			ctensor t = c(tflite::TensorType_STRING, { 1 }, 0u);

			t.op_in = false;
			graph_specs.push_back(one("c_string_nonconst.tflite", t));
			t.type = tflite::TensorType_RESOURCE;
			graph_specs.push_back(one("c_resource_nonconst.tflite", t));
		}
		graph_specs.push_back(one("c_wrap32.tflite", c(I8, { 65537, 65537 }, 131073u)));
		static_assert((uint32_t)(65537u * 65537u) == 131073u,
		              "c_wrap32: the 32-bit product is the data length");
		graph_specs.push_back(one("c_rank7_eq.tflite", c(I8, r7, 2u)));
		graph_specs.push_back(one("c_rank7_short.tflite", c(I8, r7, 1u)));
		{
			ctensor t = c(I8, { 4, 4 }, 6u);

			t.sparse = true;
			graph_specs.push_back(one("c_sparse.tflite", t));
		}
		{
			spec sp = graph("c_graph_in.tflite");
			sp.t0_data = 4u;
			graph_specs.push_back(sp);
			sp = graph("c_graph_out.tflite");
			sp.t1_data = 4u;
			graph_specs.push_back(sp);
		}
		{
			ctensor t = c(I8, { 1, 4 }, 4u);

			t.op_in = false;
			t.op_out = true;
			graph_specs.push_back(one("c_op_out.tflite", t));
			t.graph_in = true;
			graph_specs.push_back(one("c_alias_in_out.tflite", t));
		}
		{
			spec sp = one("c_io_oob_and_data.tflite", c(I8, { 2, 3 }, 5u));
			sp.outputs = { 99 };
			graph_specs.push_back(sp);
		}
		{
			ctensor t = c(I8, { 1, 4 }, 4u);

			t.is_variable = true;
			graph_specs.push_back(one("c_var_eq.tflite", t));
		}
		{
			spec sp = graph("c_sg1_short.tflite");
			sp.sg1_data = 3u;   /* the tensor is int8 [1, 4] */
			graph_specs.push_back(sp);
		}
		{
			ctensor t = c(I8, { 2, 3 }, 0u);

			t.empty_data = true;
			graph_specs.push_back(one("c_empty_data.tflite", t));
		}
		/* The ADDs and the graph input the added tensors ask for. */
		for (spec &sp : graph_specs) {
			const uint32_t base = ct_base(sp);

			for (uint32_t k = 0; k < sp.ct.size(); k++) {
				const int32_t idx = (int32_t)(base + k);

				if (sp.ct[k].op_in)
					sp.more_ops.push_back({ { idx }, { 1 }, 1u });
				if (sp.ct[k].op_out)
					sp.more_ops.push_back({ { 0 }, { idx }, 1u });
				if (sp.ct[k].graph_in)
					sp.inputs = { idx };
			}
		}
	}

	/* The flags the constructors above do not take, set by name. */
	for (spec &sp : graph_specs) {
		const std::string f = sp.file;

		sp.op_no_inputs   = f == "op_no_inputs.tflite";
		sp.op_no_outputs  = f == "op_no_outputs.tflite";
		sp.sg1_ok         = f == "sg1_ok.tflite" || f == "t_buffer_oob_sg1.tflite" ||
		                    f == "md_offline_total_3.tflite" ||
		                    f == "md_offline_sg0_only.tflite" ||
		                    f == "c_sg1_short.tflite";
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
		{ "pl_const_short.tflite",       1u,    false, p2,       0, 0u, 0u, 4u },
		{ "pl_in2_nodata.tflite",        1u,    false, p2,       0, 0u, 0u, 0u, 1 },
		{ "pl_in2_data.tflite",          1u,    false, p2,       0, 0u, 0u, 0u, 2 },
		{ "pl_in2_oob.tflite",           1u,    false, p2,       0, 0u, 0u, 0u, 3 },
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
