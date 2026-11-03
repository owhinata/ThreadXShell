#!/usr/bin/env python3
#
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 ThreadX Shell Project
#
"""Negative tests for the model I/O check the host gates share with the
firmware (issue #175, svc/nn/tflm/nn_tflm_model_check.h).

A board's host gate (wio-lite-ai / f746g-disco scripts/verify_tflite.cc,
grove-vision-ai-v2 scripts/verify_vela_model.cc) compiles the same
nn_tflm_model_io_check() its backend runs before planning a model.  This runs
the gate, as the configured build tree builds it, over files that are wrong in
exactly one way, and asserts the verdict AND which check gave it:

  io_ok_65535       control: the walk passes (the bound itself fits)
  io_in_dim_65536   NN_MODEL_ERR_DIMS (one past the bound)
  io_in_dim_70000   NN_MODEL_ERR_DIMS
  io_out_dim_neg1   NN_MODEL_ERR_DIMS
  io_out_index_oob  NN_MODEL_ERR_FORMAT, never DIMS
  io_no_inputs      NN_MODEL_ERR_FORMAT, never DIMS
  io_same_70000     NN_MODEL_ERR_DIMS (the device fixture: input == output)
  io_many_inputs    NN_MODEL_ERR_SHAPE: NN_MAX_IO + 1 inputs, all one tensor with
                    a 1,000-entry shape -- the count is judged first, so the
                    walk's work stays constant
  io_rank5          NN_MODEL_ERR_TENSOR: an input of rank NN_MAX_DIMS + 1
  vt_tables_4096    control: exactly NN_TFLM_VERIFY_MAX_TABLES table visits, so
                    the bounded verifier passes it and the gate reaches the walk
  vt_tables_4097    one visit over: refused AT THE VERIFIER (its sentence, with
                    the limit), never reaching the walk
  vt_shared_4097    4097 visits of 5 distinct tables (one Buffer named 4093
                    times): the bound is on visits, as the header says

And the rest of the graph (issue #178): one ADD operator and/or tensors that are
not graph I/O, at the limits NN_TFLM_MAX_OP_TENSORS (64) and
NN_TFLM_MAX_TENSOR_RANK (6):

  op_in_64 / op_out_64 / op_int_64   controls: the walk passes (each list at 64)
  op_in_neg1 / t_rank_6 / sg1_ok     controls: -1 in inputs, an internal rank 6, a
                                     second subgraph with a tensor and no operator
  op_in_65 / op_out_65 / op_int_65   NN_MODEL_ERR_FORMAT: a list one over 64
  op_in_oob / op_int_oob             NN_MODEL_ERR_FORMAT: an index == the tensor count
  op_out_neg1 / op_int_neg1          NN_MODEL_ERR_FORMAT: -1 outside inputs
  op_no_inputs / op_no_outputs /     NN_MODEL_ERR_FORMAT: a missing list / vector
  sg1_no_tensors
  t_rank_7                           NN_MODEL_ERR_TENSOR: an internal tensor's rank
  op_in_65000_shared                 NN_MODEL_ERR_FORMAT within WITNESS_SECONDS:
  t_shape_65000_shared               NN_MODEL_ERR_TENSOR within WITNESS_SECONDS:
                                     4000 entries naming one 65,000-entry vector --
                                     the walk reads lengths before elements, so a
                                     regression to element-first fails on time

And the quantization lengths (issue #180), at NN_TFLM_MAX_QUANT_CHANNELS (4096)
and NN_TFLM_MAX_QUANT_REFS (65536), all refusals NN_MODEL_ERR_FORMAT:

  q_scale_4096 / q_zp_1_of_4         controls: a scale at the cap; zero point 1 of 4
  q_scale_gt_dim / q_qd_5            controls: scale 8 on dims[qd] 4 (not compared,
                                     by decision); qd 5 on a rank-1 tensor (qd is
                                     bounded by NN_TFLM_MAX_TENSOR_RANK on rank
                                     0 / 1, not by the rank, by decision)
  q_refs_65536                       control: the operators' sum at the limit
  q_scale_4097 / q_zp_4097           a scale one over the cap; a zero point neither
                                     1 nor the scale's length
  q_zp_2_of_4                        zero point 2 of 4
  q_qd_6 / q_qd_neg                  per-channel qd 6 on rank 1; qd -1
  q_qd_rank2_1 / q_scale1_qd_100     controls: qd 1 on rank 2; qd 100 with scale 1
                                     (the qd rule is for per-channel tensors only)
  q_qd_rank2_2                       per-channel qd 2 == rank 2 (rank >= 2: qd < rank)
  q_refs_65537                       the sum one over
  q_scale_65000_shared               within WITNESS_SECONDS: 2000 tensor entries
                                     naming one 65,000-entry scale vector
  q_refs_4000_shared                 within WITNESS_SECONDS: 4000 operator entries
                                     naming one tensor of scale 32 (each under the
                                     cap, the sum 128,000 over it)

And the buffer indices and the offline plan (issue #181), all refusals
NN_MODEL_ERR_FORMAT; plan(n, k) is an OfflineMemoryAllocation buffer with
tensor count n and k offsets (12 + 4k bytes), the graph has 2 tensors:

  t_buffer_last                      control: a tensor on the last buffer (nb - 1)
  t_buffer_oob / t_buffer_oob_sg1    a tensor on buffer nb (subgraph 0 / 1)
  t_no_buffers                       no buffer vector, a tensor on buffer 0
  md_buffer_oob                      a metadata entry (not the plan) on buffer nb
  md_other_name / md_offline_name_24 controls: another name with 1 byte of data;
                                     a 24-byte name starting with the plan's 23
                                     (TFLM's compare includes the length)
  md_offline_ok / md_offline_total_3 controls: plan(2, 2); plan(3, 3) with a
                                     second subgraph -- the count is every
                                     subgraph's tensors, as TFLM compares it
  md_offline_short / _hdr_short /    plan(2, 1); 8 bytes; no data; the plan on
  _null / _buffer_oob                buffer nb
  md_offline_nbr_lt / _gt / _neg     plan(1, 1) / plan(3, 3) / plan(-1, 2)
  md_offline_off_neg2 / _off_min     plan(2, 2) with an offset of -2 / INT32_MIN
                                     (only -1 means "plan online" to TFLM)
  md_offline_off_pos                 control: plan(2, 2) with offsets 0 and 16
  md_offline_sg0_only                plan(2, 2) with a second subgraph (3 tensors)
  md_offline_second_bad              plan(2, 2), then a second plan(2, 1): TFLM
                                     reads every matching entry
  codes_null                         no operator-code vector (and no operator)

On the vela style the controls with a plan are judged by the walk's ok line like
the rest; the gate's own offline-plan report and its later steps run after it.

And the constant buffers (issue #183), all refusals NN_MODEL_ERR_FORMAT: every
tensor's type one TFLM gives a size, every tensor with data exactly element
count x element size long (INT4 packed two to a byte, 1 element for no shape),
its rank judged before its values, and no data on an operator output or on
subgraph 0's inputs / outputs:

  c_int8_eq / c_types_eq             controls: int8 [2, 3] 6 B; one [3] constant of
                                     every sized type
  c_int8_short / _long / c_int32_short  5 B / 7 B / int32 [2] 2 B (its count)
  c_scalar_null / c_scalar_empty     controls: int32, no shape / shape [], 4 B
  c_zero / c_neg                     [0, 3] 1 B / [-1, 3] 3 B
  c_int4_packed / _even              controls: int4 [3] 2 B / [4] 2 B
  c_int4_unpacked / _short           int4 [3] 3 B / [5] 2 B
  c_string / c_string_nonconst       a STRING constant / a STRING tensor without
                                     data (every tensor's type is judged)
  c_resource_nonconst                control: RESOURCE without data
  c_wrap32                           [65537, 65537] 131,073 B (the 32-bit product)
  c_rank7_eq / c_rank7_short         rank-7 constants: FORMAT, not TENSOR
  c_sparse                           a CSR-sparse [4, 4] constant, 6 B
  c_graph_in / c_graph_out / c_op_out / c_alias_in_out
                                     data on a tensor the graph writes
  c_io_oob_and_data                  an output index past the tensors and a short
                                     constant: FORMAT either way
  c_var_eq / c_empty_data            controls: a variable constant an ADD reads; a
                                     data vector 0 long
  c_sg1_short                        a short constant in a second subgraph

And, on verify_vela_model only (--style vela), Grove's payload scan (issue #179,
svc/nn/ethosu/npu_model_scan.cc), which charges each operator's payload bytes
against the file's length before walking it and refuses -1 as an ethos-u
operator's input after 0.  Its refusal is NN_MODEL_ERR_PAYLOAD with the scan's own
sentence, checked here by sentence: the vela gate already prints
NN_MODEL_ERR_PAYLOAD for the ADD fixtures above (a builtin operator), so the code
is not in CODES.  The controls pass the scan and are refused later, for having no
OfflineMemoryAllocation (they are not Vela output); what is asserted is the scan's
ok line.

  pl_share2_eq_len      control: two entries sharing a payload, sum == length
  pl_share2_len_minus1  the same one byte shorter              -> the sum sentence
  pl_share_4000         4000 entries sharing a 400 KB stream   -> the sum sentence
  pl_index_1300         1300 buffer indices naming one Buffer  -> the sum sentence
                        (both within WITNESS_SECONDS: walked per entry they took
                        0.42 s / 0.14 s)
  pl_single_whole       control: one payload, 99.9 % of the file
  pl_in1_neg1           input 1 == -1                          -> the -1 sentence
  pl_in1_ok             control: input 1 a real tensor
  pl_in_65              65 inputs                              -> the length sentence
                        (judged by the scan before its -1 check; the walk would
                        say FORMAT, so the order below matters)
  pl_in2_nodata         control: input 2 (the NPU's arena base pointer) without data
  pl_in2_data           input 2 with data (exact, so the walk passes it)
                        -> the data sentence, in BOTH orders (issue #183)
  pl_in2_oob            input 2 past the tensor vector     -> the range sentence
  pl_const_short        pl_in1_ok plus a constant 4 B short of its shape: the
                        walk's FORMAT (issue #183), run in BOTH orders -- with
                        --board-order after the scan's ok line

Every pl_ case runs with --board-order (the scan before the walk, the board's
order), and a refusal is matched as the gate's whole FAIL line.

And the operator rules (issue #185): one operator over its own tensors, each
constant exactly its shape's bytes, so only the rule can refuse it.  The answer
depends on the gate's PROFILE (the op-list define on its compile line), because
the walk compiles only the rules of the operators the profile registers:

  r0_index_last          control: opcode_index == the code vector's length - 1
  r0_index_oob           opcode_index == its length: FORMAT on every profile (on
                         the vela gate with --board-order the payload scan's own
                         sentence, which runs first there)
  r1_* .. r9_*           CONV_2D / DEPTHWISE_CONV_2D / FULLY_CONNECTED / PAD /
                         STRIDED_SLICE / TRANSPOSE / RESIZE_* / ADD, SUB, MUL /
                         CONCATENATION: the
                         case's own answer (None or FORMAT) where the profile
                         registers the operator; the gate's "did not register"
                         REJECT (rc 1, the walk's ok line, no code) where it
                         does not (on blazeface: FULLY_CONNECTED, SUB, MUL,
                         STRIDED_SLICE, TRANSPOSE, RESIZE_*, CONCATENATION); on
                         the ethos-u profile, which has no CPU rule, the walk's
                         answer without the rule (its ok line, or TENSOR for
                         r1_in_rank7 / r8_rank7) and then the payload scan's "builtin"
                         sentence, and with --board-order that sentence alone

And the depth half of the header's claim: schema_table_depth.py runs on the
schema.fbs the gate was built from (the first -I directory holding one, else the
repo's lib/tflite-micro, the tree a tflm build generates from) and fails on a
cycle or a chain deeper than NN_TFLM_VERIFY_MAX_DEPTH; its self-test runs first.

The files are written fresh each run by cmake/fixtures/model_io/
gen_model_io_fixtures.cc, compiled with the include paths read off the gate's
OWN compile line (`ninja -t commands`), so they are laid out by the tree the
gate reads them with.  The gate is brought up to date first, so this never
tests yesterday's binary.

  --style tflite   verify_tflite: the control must PASS outright (rc 0)
  --style vela     verify_vela_model: the control must print the walk's ok line
                   (a zero-operator model is refused later, by design, for
                   having no ethos-u operator -- after the walk)
"""
import argparse
import os
import shlex
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
GEN_SRC = os.path.join(HERE, "model_io", "gen_model_io_fixtures.cc")

# What each gate prints: the code names are the board's, so both styles share them.
DIMS = "NN_MODEL_ERR_DIMS"
FORMAT = "NN_MODEL_ERR_FORMAT"
SHAPE = "NN_MODEL_ERR_SHAPE"
TENSOR = "NN_MODEL_ERR_TENSOR"
CODES = (DIMS, FORMAT, SHAPE, TENSOR)
# The verifier's refusal: the same opening, and the depth limit, in both styles.
VERIFY = "the flatbuffer is malformed, or nests deeper than 16"
OK_LINE = {
    "tflite": "io dims : ok",
    "vela": "the firmware's I/O dims rule",
}

CASES = [
    # (file, expected marker or None for the control)
    ("io_ok_65535.tflite", None),
    ("io_in_dim_65536.tflite", DIMS),
    ("io_in_dim_70000.tflite", DIMS),
    ("io_out_dim_neg1.tflite", DIMS),
    ("io_out_index_oob.tflite", FORMAT),
    ("io_no_inputs.tflite", FORMAT),
    # The device fixture: the very file f746g-disco loads from its card to see the
    # refusal on the board (one tensor, input == output) -- checked here too.
    ("io_same_70000.tflite", DIMS),
    ("io_many_inputs.tflite", SHAPE),
    ("io_rank5.tflite", TENSOR),
    # The verifier's work bound (NN_TFLM_VERIFY_MAX_TABLES = 4096).
    ("vt_tables_4096.tflite", None),
    ("vt_tables_4097.tflite", VERIFY),
    ("vt_shared_4097.tflite", VERIFY),
    # The rest of the graph (issue #178): every operator's tensor lists, at most
    # NN_TFLM_MAX_OP_TENSORS (64) long and naming tensors that exist, and every
    # tensor's rank, at most NN_TFLM_MAX_TENSOR_RANK (6).
    ("op_in_64.tflite", None),
    ("op_out_64.tflite", None),
    ("op_int_64.tflite", None),
    ("op_in_65.tflite", FORMAT),
    ("op_out_65.tflite", FORMAT),
    ("op_int_65.tflite", FORMAT),
    ("op_in_neg1.tflite", None),
    ("op_in_oob.tflite", FORMAT),
    ("op_out_neg1.tflite", FORMAT),
    ("op_int_neg1.tflite", FORMAT),
    ("op_int_oob.tflite", FORMAT),
    ("op_no_inputs.tflite", FORMAT),
    ("op_no_outputs.tflite", FORMAT),
    ("t_rank_6.tflite", None),
    ("t_rank_7.tflite", TENSOR),
    ("sg1_ok.tflite", None),
    ("sg1_no_tensors.tflite", FORMAT),
    # The walk's work bound: 4000 entries naming one 65,000-entry vector.  Read
    # length-first each is refused at its first entry; read element-first each
    # costs 2.6e8 reads, which WITNESS_SECONDS turns into a failure.
    ("op_in_65000_shared.tflite", FORMAT),
    ("t_shape_65000_shared.tflite", TENSOR),
    # The quantization lengths (issue #180): a tensor's scale vector at most
    # NN_TFLM_MAX_QUANT_CHANNELS (4096), its zero point 0 / 1 / the scale's length,
    # a per-channel qd inside the rank (rank >= 2) or 0..NN_TFLM_MAX_TENSOR_RANK - 1
    # (rank 0 / 1), and the scale lengths the operators name
    # adding up to at most NN_TFLM_MAX_QUANT_REFS (65536).
    ("q_scale_4096.tflite", None),
    ("q_scale_4097.tflite", FORMAT),
    ("q_zp_4097.tflite", FORMAT),
    ("q_zp_1_of_4.tflite", None),
    ("q_zp_2_of_4.tflite", FORMAT),
    ("q_scale_gt_dim.tflite", None),
    ("q_qd_5.tflite", None),
    ("q_qd_6.tflite", FORMAT),
    ("q_qd_neg.tflite", FORMAT),
    ("q_qd_rank2_1.tflite", None),
    ("q_qd_rank2_2.tflite", FORMAT),
    ("q_scale1_qd_100.tflite", None),
    ("q_refs_65536.tflite", None),
    ("q_refs_65537.tflite", FORMAT),
    ("q_scale_65000_shared.tflite", FORMAT),
    ("q_refs_4000_shared.tflite", FORMAT),
    # The buffer indices and the offline plan (issue #181): every tensor's and
    # metadata entry's buffer below the buffer count, every OfflineMemoryAllocation
    # entry with data, a 12-byte header, a count equal to every subgraph's tensors
    # together and that many offsets, each -1 or >= 0, and an operator-code vector.
    ("t_buffer_last.tflite", None),
    ("t_buffer_oob.tflite", FORMAT),
    ("t_buffer_oob_sg1.tflite", FORMAT),
    ("t_no_buffers.tflite", FORMAT),
    ("md_buffer_oob.tflite", FORMAT),
    ("md_other_name.tflite", None),
    ("md_offline_ok.tflite", None),
    ("md_offline_name_24.tflite", None),
    ("md_offline_short.tflite", FORMAT),
    ("md_offline_hdr_short.tflite", FORMAT),
    ("md_offline_null.tflite", FORMAT),
    ("md_offline_buffer_oob.tflite", FORMAT),
    ("md_offline_nbr_lt.tflite", FORMAT),
    ("md_offline_nbr_gt.tflite", FORMAT),
    ("md_offline_nbr_neg.tflite", FORMAT),
    ("md_offline_total_3.tflite", None),
    ("md_offline_sg0_only.tflite", FORMAT),
    ("md_offline_second_bad.tflite", FORMAT),
    ("md_offline_off_neg2.tflite", FORMAT),
    ("md_offline_off_min.tflite", FORMAT),
    ("md_offline_off_pos.tflite", None),
    ("codes_null.tflite", FORMAT),
    # The constant buffers (issue #183): every tensor's type with a size, every
    # constant exactly its shape's bytes, no data on a tensor the graph writes.
    ("c_int8_eq.tflite", None),
    ("c_types_eq.tflite", None),
    ("c_int8_short.tflite", FORMAT),
    ("c_int8_long.tflite", FORMAT),
    ("c_int32_short.tflite", FORMAT),
    ("c_scalar_null.tflite", None),
    ("c_scalar_empty.tflite", None),
    ("c_zero.tflite", FORMAT),
    ("c_neg.tflite", FORMAT),
    ("c_int4_packed.tflite", None),
    ("c_int4_even.tflite", None),
    ("c_int4_unpacked.tflite", FORMAT),
    ("c_int4_short.tflite", FORMAT),
    ("c_string.tflite", FORMAT),
    ("c_string_nonconst.tflite", FORMAT),
    ("c_resource_nonconst.tflite", None),
    ("c_wrap32.tflite", FORMAT),
    ("c_rank7_eq.tflite", FORMAT),
    ("c_rank7_short.tflite", FORMAT),
    ("c_sparse.tflite", FORMAT),
    ("c_graph_in.tflite", FORMAT),
    ("c_graph_out.tflite", FORMAT),
    ("c_op_out.tflite", FORMAT),
    ("c_alias_in_out.tflite", FORMAT),
    ("c_io_oob_and_data.tflite", FORMAT),
    ("c_var_eq.tflite", None),
    ("c_sg1_short.tflite", FORMAT),
    ("c_empty_data.tflite", None),
]

# Grove's payload scan (issue #179): vela style only, judged by the scan's sentence.
PAYLOAD = "NN_MODEL_ERR_PAYLOAD"
PL_OK = "the firmware's payload scan accepts every operator"
# The scan's whole sentences (svc/nn/ethosu/npu_model_scan.cc), matched as the
# gate's whole FAIL line, so a reworded tail is a failure here too.
PL_SUM = ("the operators' payloads add up to more bytes than the model, so a "
          "command stream is shared or overlaps")
PL_NEG = ("an ethos-u operator names no tensor (-1) for an input after the command "
          "stream, which its Eval() reads unchecked")
PL_LEN = ("an ethos-u operator lists more inputs than the firmware's operator walk "
          "allows")
PL_DATA = ("an ethos-u operator's input after the weights carries data, so the "
           "NPU would write into the model")
PL_OOB = "an ethos-u operator names an input tensor that does not exist"
PL_REASONS = (PL_SUM, PL_NEG, PL_LEN, PL_DATA, PL_OOB)
PL_FAIL = "  FAIL   the firmware would refuse this model (NN_MODEL_ERR_PAYLOAD): "
# Run with the board's order: the scan before the graph walk (nn_ethosu.cc), so the
# first refusal is the one the board gives -- pl_in_65 would otherwise be the walk's.
PL_ARGS = ["--board-order"]
PL_CASES = [
    # (file, expected sentence or None for a control that passes the scan)
    ("pl_share2_eq_len.tflite", None),
    ("pl_share2_len_minus1.tflite", PL_SUM),
    ("pl_share_4000.tflite", PL_SUM),
    ("pl_index_1300.tflite", PL_SUM),
    ("pl_single_whole.tflite", None),
    ("pl_in1_neg1.tflite", PL_NEG),
    ("pl_in1_ok.tflite", None),
    ("pl_in_65.tflite", PL_LEN),
    ("pl_in2_nodata.tflite", None),
    ("pl_in2_oob.tflite", PL_OOB),
]
# Payload models the SCAN refuses after the walk passes them (issue #183): run in
# the default order too (the walk's ok line first, then the scan's sentence).
PL_SCAN_BOTH = [
    ("pl_in2_data.tflite", PL_DATA),
]
# Payload models the WALK refuses (issue #183): (file, code), each run in the
# default order (the walk first) and with --board-order (the scan first, which
# must pass it and print its ok line before the walk's refusal).
PL_WALK_CASES = [
    ("pl_const_short.tflite", FORMAT),
]

# The operator rules (issue #185): (file, answer where the profile registers the
# operator, the operator, the walk's answer without the rule).  None is a control.
OP_CASES = [
    ("r0_index_last.tflite", None, "ADD", None),
    ("r0_index_oob.tflite", FORMAT, "ADD", FORMAT),
    ("r1_ok.tflite", None, "CONV_2D", None),
    ("r1_float.tflite", None, "CONV_2D", None),
    ("r1_no_bias.tflite", None, "CONV_2D", None),
    ("r1_two_inputs.tflite", None, "CONV_2D", None),
    ("r1_i16_i64.tflite", None, "CONV_2D", None),
    ("r1_one_input.tflite", None, "CONV_2D", None),
    ("r1_filter_ne.tflite", FORMAT, "CONV_2D", None),
    ("r1_deprecated_ne.tflite", FORMAT, "CONV_2D", None),
    ("r1_bias_2.tflite", FORMAT, "CONV_2D", None),
    ("r1_bias_nodata_2.tflite", FORMAT, "CONV_2D", None),
    ("r1_bias_int8.tflite", FORMAT, "CONV_2D", None),
    ("r1_bias_f32_i8.tflite", FORMAT, "CONV_2D", None),
    ("r1_in_rank3.tflite", FORMAT, "CONV_2D", None),
    ("r1_in_rank7.tflite", FORMAT, "CONV_2D", TENSOR),
    ("r2_dm1.tflite", None, "DEPTHWISE_CONV_2D", None),
    ("r2_dm4.tflite", None, "DEPTHWISE_CONV_2D", None),
    ("r2_dm2.tflite", None, "DEPTHWISE_CONV_2D", None),
    ("r2_i16_i64.tflite", None, "DEPTHWISE_CONV_2D", None),
    ("r2_filter_ne.tflite", FORMAT, "DEPTHWISE_CONV_2D", None),
    ("r2_dm_ne.tflite", FORMAT, "DEPTHWISE_CONV_2D", None),
    ("r2_dm0.tflite", FORMAT, "DEPTHWISE_CONV_2D", None),
    ("r2_opts_other.tflite", FORMAT, "DEPTHWISE_CONV_2D", None),
    ("r2_bias_2.tflite", FORMAT, "DEPTHWISE_CONV_2D", None),
    ("r2_i16_i32.tflite", FORMAT, "DEPTHWISE_CONV_2D", None),
    ("r3_ok.tflite", None, "FULLY_CONNECTED", None),
    ("r3_out_rank4.tflite", None, "FULLY_CONNECTED", None),
    ("r3_no_bias.tflite", None, "FULLY_CONNECTED", None),
    ("r3_w_nonconst.tflite", None, "FULLY_CONNECTED", None),
    ("r3_issue.tflite", FORMAT, "FULLY_CONNECTED", None),
    ("r3_w0_ne.tflite", FORMAT, "FULLY_CONNECTED", None),
    ("r3_w_rank3.tflite", FORMAT, "FULLY_CONNECTED", None),
    ("r3_out_rank1.tflite", FORMAT, "FULLY_CONNECTED", None),
    ("r3_out_rank5.tflite", FORMAT, "FULLY_CONNECTED", None),
    ("r3_in_count_ne.tflite", FORMAT, "FULLY_CONNECTED", None),
    ("r3_saturate.tflite", FORMAT, "FULLY_CONNECTED", None),
    ("r3_out_last0.tflite", FORMAT, "FULLY_CONNECTED", None),
    ("r3_i16_i32.tflite", FORMAT, "FULLY_CONNECTED", None),
    ("r4_ok.tflite", None, "PAD", None),
    ("r4_rank_ne.tflite", FORMAT, "PAD", None),
    ("r4_pad_int8.tflite", FORMAT, "PAD", None),
    ("r5_ok.tflite", None, "STRIDED_SLICE", None),
    ("r5_begin_3.tflite", FORMAT, "STRIDED_SLICE", None),
    ("r5_strides_int8.tflite", FORMAT, "STRIDED_SLICE", None),
    ("r6_ok.tflite", None, "TRANSPOSE", None),
    ("r6_perm_int8.tflite", FORMAT, "TRANSPOSE", None),
    ("r7_ok.tflite", None, "RESIZE_BILINEAR", None),
    ("r7_nn_ok.tflite", None, "RESIZE_NEAREST_NEIGHBOR", None),
    # A size without data: the walk does not compare values (the board's
    # Prepare() refuses it as ARENA, the gates pass it -- a known difference).
    ("r7_nodata.tflite", None, "RESIZE_BILINEAR", None),
    ("r7_size_1.tflite", FORMAT, "RESIZE_BILINEAR", None),
    ("r7_size_3.tflite", FORMAT, "RESIZE_BILINEAR", None),
    ("r7_size_int8.tflite", FORMAT, "RESIZE_BILINEAR", None),
    ("r7_value_ne.tflite", FORMAT, "RESIZE_BILINEAR", None),
    ("r7_out_rank3.tflite", FORMAT, "RESIZE_BILINEAR", None),
    ("r7_size_rank2.tflite", FORMAT, "RESIZE_BILINEAR", None),
    ("r8_broadcast.tflite", None, "ADD", None),
    ("r8_same.tflite", None, "ADD", None),
    ("r8_const.tflite", None, "ADD", None),
    ("r8_rank6.tflite", None, "ADD", None),
    ("r8_dim_2_of_3.tflite", FORMAT, "ADD", None),
    ("r8_sub_dim_2_of_3.tflite", FORMAT, "SUB", None),
    ("r8_mul_dim_2_of_3.tflite", FORMAT, "MUL", None),
    ("r8_same_out_ne.tflite", FORMAT, "ADD", None),
    ("r8_ones_to_2.tflite", FORMAT, "ADD", None),
    ("r8_in_rank_gt.tflite", FORMAT, "ADD", None),
    ("r8_rank7.tflite", FORMAT, "ADD", TENSOR),
    ("r9_axis3.tflite", None, "CONCATENATION", None),
    ("r9_axis_neg1.tflite", None, "CONCATENATION", None),
    ("r9_other_dim_ne.tflite", FORMAT, "CONCATENATION", None),
    ("r9_sum_ne.tflite", FORMAT, "CONCATENATION", None),
    ("r9_rank_ne.tflite", FORMAT, "CONCATENATION", None),
    ("r9_axis_oob.tflite", FORMAT, "CONCATENATION", None),
]
# What each profile registers of the operators above (svc/nn/tflm/nn_tflm_ops.h;
# a profile changing there must change here, or these cases fail).
PROFILE_DEFINES = {
    "NN_TFLM_OPS_BLAZEFACE_PROFILE": "blazeface",
    "NN_TFLM_OPS_MLPERF_PROFILE": "mlperf",
    "NN_TFLM_OPS_EXTENDED": "extended",
    "NN_TFLM_OPS_ETHOSU_PROFILE": "ethosu",
}
_BASE_OPS = {"ADD", "CONV_2D", "DEPTHWISE_CONV_2D", "PAD"}
PROFILE_OPS = {
    "blazeface": _BASE_OPS,
    "mlperf": _BASE_OPS | {"FULLY_CONNECTED"},
    "extended": _BASE_OPS | {"FULLY_CONNECTED", "SUB", "MUL", "STRIDED_SLICE",
                             "TRANSPOSE", "RESIZE_BILINEAR",
                             "RESIZE_NEAREST_NEIGHBOR", "CONCATENATION"},
    "ethosu": set(),
}
# The tflite gates' refusal of an operator the profile does not register.
UNREG = "operator(s) this build did not"
# The payload scan's sentences for a builtin operator and a code index past the
# vector (vela gate only).
PL_BUILTIN = "an operator is a builtin; this port registers only ethos-u"
PL_CODE_OOB = "an operator names an operator code that does not exist"


def gate_profile(flags):
    """The NN_TFLM_OPS profile the gate was compiled for, from its -D flags."""
    found = []
    i = 0
    while i < len(flags):
        f = flags[i]
        if f == "-D":
            f = "-D" + flags[i + 1]
            i += 1
        name = f[2:].split("=")[0] if f.startswith("-D") else None
        if name in PROFILE_DEFINES:
            found.append(PROFILE_DEFINES[name])
        i += 1
    if len(found) != 1:
        sys.exit(f"run_model_io_gate_tests: the gate's compile line names {found} "
                 "as its NN_TFLM_OPS profile, not exactly one")
    return found[0]


def op_case_runs(style, profile):
    """(file, kind, want, extra args) for each OP_CASES run on this gate.
    kind: "code" -- want is a code or None, judged like CASES;
          "unreg" -- the tflite gate's unregistered-operator REJECT;
          "scan" -- the vela gate: the walk's answer (want[0], a code or None)
                    and then the scan's sentence want[1], or with --board-order
                    the sentence alone (want[0] is then not judged)."""
    runs = []
    for name, want, op, bare in OP_CASES:
        if style == "vela":
            sentence = PL_CODE_OOB if name == "r0_index_oob.tflite" else PL_BUILTIN
            if bare is not None:
                runs.append((name, "code", bare, []))
            else:
                runs.append((name, "scan", (None, sentence), []))
            runs.append((name, "scan", (None, sentence), PL_ARGS))
        elif op in PROFILE_OPS[profile]:
            runs.append((name, "code", want, []))
        else:
            runs.append((name, "unreg", None, []))
    return runs


# The wall-time ceiling on the two "_shared" witnesses: the fastest of
# WITNESS_RUNS gate runs (the file read and the verifier included), so one run
# slowed by a loaded host does not fail it.  Length-first they take 1..5 ms on the
# host; the operator witness read element-first took ~0.27 s (2.6e8 reads).  A
# loop the host compiler vectorizes can stay under any ceiling (measured: the
# shape witness read element-first stayed ~5 ms), so the time is a backstop for
# the order; the verdicts above are the rule.
WITNESSES = ("op_in_65000_shared.tflite", "t_shape_65000_shared.tflite",
             "q_scale_65000_shared.tflite", "q_refs_4000_shared.tflite",
             "pl_share_4000.tflite", "pl_index_1300.tflite")
WITNESS_SECONDS = 0.1
WITNESS_RUNS = 3

REPO = os.path.dirname(os.path.dirname(HERE))
DEPTH_TOOL = os.path.join(HERE, "model_io", "schema_table_depth.py")
CHECK_HDR = os.path.join(REPO, "svc", "nn", "tflm", "nn_tflm_model_check.h")
SCHEMA_REL = os.path.join("tensorflow", "compiler", "mlir", "lite", "schema", "schema.fbs")


def gate_compile_flags(build_dir, gate):
    """The -I / -D flags of the gate's own compile line."""
    out = subprocess.run(["ninja", "-C", build_dir, "-t", "commands", gate],
                         check=True, capture_output=True, text=True).stdout
    lines = [l for l in out.splitlines() if l.strip()]
    if not lines:
        sys.exit(f"run_model_io_gate_tests: no command builds {gate} in {build_dir}")
    argv = shlex.split(lines[-1])
    flags = []
    i = 0
    while i < len(argv):
        a = argv[i]
        if a in ("-I", "-D"):
            flags += [a, argv[i + 1]]
            i += 2
            continue
        if a.startswith("-I") or a.startswith("-D"):
            flags.append(a)
        i += 1
    if not any(f == "-I" or f.startswith("-I") for f in flags):
        sys.exit(f"run_model_io_gate_tests: no include path on {gate}'s compile line")
    return flags


def schema_depth_check(flags):
    """0 on success: the gate's schema is acyclic and within the depth limit."""
    import re
    m = re.search(r"#define\s+NN_TFLM_VERIFY_MAX_DEPTH\s+(\d+)u", open(CHECK_HDR).read())
    if not m:
        print(f"  FAIL no NN_TFLM_VERIFY_MAX_DEPTH in {CHECK_HDR}")
        return 1
    limit = int(m.group(1))
    st = subprocess.run([sys.executable, DEPTH_TOOL, "--self-test"],
                        capture_output=True, text=True)
    if st.returncode != 0:
        print(f"  FAIL schema_table_depth self-test:\n{st.stdout}{st.stderr}")
        return 1
    dirs = []
    i = 0
    while i < len(flags):
        if flags[i] == "-I":
            dirs.append(flags[i + 1])
            i += 2
            continue
        if flags[i].startswith("-I"):
            dirs.append(flags[i][2:])
        i += 1
    cands = [os.path.join(d, SCHEMA_REL) for d in dirs]
    cands.append(os.path.join(REPO, "lib", "tflite-micro", SCHEMA_REL))
    schema = next((c for c in cands if os.path.isfile(c)), None)
    if schema is None:
        print("  FAIL no schema.fbs found for the gate")
        return 1
    p = subprocess.run([sys.executable, DEPTH_TOOL, schema], capture_output=True, text=True)
    m = re.search(r"max table nesting from Model: (\d+)", p.stdout)
    if p.returncode != 0 or not m or int(m.group(1)) > limit:
        print(f"  FAIL schema depth of {schema} (limit {limit}):\n{p.stdout}{p.stderr}")
        return 1
    print(f"  ok   schema {os.path.relpath(schema, REPO)}: acyclic, table depth "
          f"{m.group(1)} <= {limit}")
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--build-dir", required=True)
    ap.add_argument("--gate", required=True,
                    help="the gate's output file name in the build tree")
    ap.add_argument("--style", required=True, choices=sorted(OK_LINE))
    ap.add_argument("--cxx", default="g++")
    args = ap.parse_args()

    gate_path = os.path.join(args.build_dir, args.gate)
    up = subprocess.run(["ninja", "-C", args.build_dir, args.gate],
                        capture_output=True, text=True)
    if up.returncode != 0:
        sys.stderr.write(up.stdout[-4000:] + up.stderr[-4000:])
        sys.exit(f"run_model_io_gate_tests: FAIL -- could not build {gate_path}")

    flags = gate_compile_flags(args.build_dir, args.gate)
    failures = 0
    with tempfile.TemporaryDirectory() as tmp:
        gen = os.path.join(tmp, "gen_model_io_fixtures")
        subprocess.run([args.cxx, "-std=c++17", "-O1", "-w", *flags, GEN_SRC,
                        "-o", gen], check=True)
        subprocess.run([gen, tmp], check=True)

        cases = [(n, w, None) for n, w in CASES]
        if args.style == "vela":
            cases += [(n, w, None) for n, w in PL_CASES]
            for n, w in PL_WALK_CASES:
                cases += [(n, w, []), (n, w, PL_ARGS)]
            for n, w in PL_SCAN_BOTH:
                cases += [(n, ("scan", w), []), (n, ("scan", w), PL_ARGS)]
        for name, want, args_override in cases:
            took = None
            for _ in range(WITNESS_RUNS if name in WITNESSES else 1):
                t0 = time.monotonic()
                extra = args_override if args_override is not None else \
                    (PL_ARGS if name.startswith("pl_") else [])
                p = subprocess.run([gate_path, os.path.join(tmp, name), *extra],
                                   capture_output=True, text=True)
                dt = time.monotonic() - t0
                took = dt if took is None else min(took, dt)
            text = p.stdout + p.stderr
            if args_override is not None and isinstance(want, tuple):
                want = want[1]
                lines = p.stdout.splitlines()
                ok = p.returncode == 1 and (PL_FAIL + want) in lines \
                    and not any(x in text for x in PL_REASONS if x != want) \
                    and PL_OK not in text \
                    and not any(c in text for c in CODES)
                if not args_override:
                    ok = ok and OK_LINE[args.style] in text
                name = f"{name} ({' '.join(args_override) or 'default order'})"
                why = f"rc 1 with the whole line {PAYLOAD}: \"{want}\"" + \
                    ("" if args_override else ", after the walk's ok line")
            elif args_override is not None:
                others = [c for c in CODES if c != want]
                ok = p.returncode == 1 and want in text \
                    and not any(c in text for c in others) \
                    and PAYLOAD not in text \
                    and OK_LINE[args.style] not in text
                if args_override:
                    ok = ok and PL_OK in text
                name = f"{name} ({' '.join(args_override) or 'default order'})"
                why = f"rc 1 with {want} (and no other code, no {PAYLOAD})" + \
                    (", after the payload scan's ok line" if args_override else "")
            elif name.startswith("pl_"):
                if want is None:
                    ok = PL_OK in text and PAYLOAD not in text \
                        and not any(c in text for c in CODES)
                    why = "the payload scan's ok line, no NN_MODEL_ERR_PAYLOAD"
                else:
                    lines = p.stdout.splitlines()
                    ok = p.returncode == 1 and (PL_FAIL + want) in lines \
                        and not any(x in text for x in PL_REASONS if x != want) \
                        and PL_OK not in text \
                        and not any(c in text for c in CODES)
                    why = f"rc 1 with the whole line {PAYLOAD}: \"{want}\""
            elif want is None:
                ok = OK_LINE[args.style] in text \
                    and not any(c in text for c in CODES)
                if args.style == "tflite":
                    ok = ok and p.returncode == 0
                why = "the walk's ok line" + (" and rc 0" if args.style == "tflite" else "")
            elif want == VERIFY:
                ok = p.returncode == 1 and VERIFY in text \
                    and not any(c in text for c in CODES) \
                    and OK_LINE[args.style] not in text
                why = "rc 1 at the verifier (its sentence, not the walk's)"
            else:
                others = [c for c in CODES if c != want]
                ok = p.returncode == 1 and want in text \
                    and not any(c in text for c in others) \
                    and OK_LINE[args.style] not in text
                why = f"rc 1 with {want} (and no other code)"
            if name in WITNESSES:
                ok = ok and took < WITNESS_SECONDS
                why += (f" within {WITNESS_SECONDS} s (fastest of {WITNESS_RUNS}: "
                        f"{took:.3f} s)")
            if ok:
                print(f"  ok   {args.gate} {name}: {why}")
            else:
                failures += 1
                print(f"  FAIL {args.gate} {name}: wanted {why}, got rc "
                      f"{p.returncode}:\n{text}")

        # The operator rules (issue #185), by the gate's profile.
        profile = gate_profile(flags)
        op_runs = op_case_runs(args.style, profile)
        for name, kind, want, extra in op_runs:
            p = subprocess.run([gate_path, os.path.join(tmp, name), *extra],
                               capture_output=True, text=True)
            text = p.stdout + p.stderr
            label = f"{name} ({profile}{', ' + ' '.join(extra) if extra else ''})"
            if kind == "unreg":
                ok = p.returncode == 1 and UNREG in text \
                    and OK_LINE[args.style] in text \
                    and not any(c in text for c in CODES)
                why = "rc 1 with the unregistered-operator REJECT after the walk's ok line"
            elif kind == "scan":
                sentence = want[1]
                ok = p.returncode == 1 and (PL_FAIL + sentence) in p.stdout.splitlines() \
                    and not any(c in text for c in CODES)
                if extra:
                    ok = ok and OK_LINE[args.style] not in text
                    why = f"rc 1 with the payload scan's line \"{sentence}\" first"
                else:
                    ok = ok and OK_LINE[args.style] in text
                    why = (f"the walk's ok line (no rule on this profile), then the "
                           f"payload scan's line \"{sentence}\"")
            elif want is None:
                ok = OK_LINE[args.style] in text \
                    and not any(c in text for c in CODES) and UNREG not in text
                if args.style == "tflite":
                    ok = ok and p.returncode == 0
                why = "the walk's ok line" + (" and rc 0" if args.style == "tflite" else "")
            else:
                others = [c for c in CODES if c != want]
                ok = p.returncode == 1 and want in text \
                    and not any(c in text for c in others) \
                    and OK_LINE[args.style] not in text
                why = f"rc 1 with {want} (and no other code)"
            if ok:
                print(f"  ok   {args.gate} {label}: {why}")
            else:
                failures += 1
                print(f"  FAIL {args.gate} {label}: wanted {why}, got rc "
                      f"{p.returncode}:\n{text}")

    failures += schema_depth_check(flags)

    if failures:
        sys.exit(f"run_model_io_gate_tests: {failures} case(s) failed for {gate_path}")
    n = len(CASES) + (len(PL_CASES) + 2 * len(PL_WALK_CASES) + 2 * len(PL_SCAN_BOTH)
                      if args.style == "vela" else 0) + len(op_runs)
    print(f"run_model_io_gate_tests: OK ({n} cases + schema depth, {gate_path})")


if __name__ == "__main__":
    main()
