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

Every pl_ case runs with --board-order (the scan before the walk, the board's
order), and a refusal is matched as the gate's whole FAIL line.

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
PL_REASONS = (PL_SUM, PL_NEG, PL_LEN)
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
]

# The wall-time ceiling on the two "_shared" witnesses: the fastest of
# WITNESS_RUNS gate runs (the file read and the verifier included), so one run
# slowed by a loaded host does not fail it.  Length-first they take 1..5 ms on the
# host; the operator witness read element-first took ~0.27 s (2.6e8 reads).  A
# loop the host compiler vectorizes can stay under any ceiling (measured: the
# shape witness read element-first stayed ~5 ms), so the time is a backstop for
# the order; the verdicts above are the rule.
WITNESSES = ("op_in_65000_shared.tflite", "t_shape_65000_shared.tflite",
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

        cases = CASES + (PL_CASES if args.style == "vela" else [])
        for name, want in cases:
            took = None
            for _ in range(WITNESS_RUNS if name in WITNESSES else 1):
                t0 = time.monotonic()
                extra = PL_ARGS if name.startswith("pl_") else []
                p = subprocess.run([gate_path, os.path.join(tmp, name), *extra],
                                   capture_output=True, text=True)
                dt = time.monotonic() - t0
                took = dt if took is None else min(took, dt)
            text = p.stdout + p.stderr
            if name.startswith("pl_"):
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

    failures += schema_depth_check(flags)

    if failures:
        sys.exit(f"run_model_io_gate_tests: {failures} case(s) failed for {gate_path}")
    n = len(CASES) + (len(PL_CASES) if args.style == "vela" else 0)
    print(f"run_model_io_gate_tests: OK ({n} cases + schema depth, {gate_path})")


if __name__ == "__main__":
    main()
