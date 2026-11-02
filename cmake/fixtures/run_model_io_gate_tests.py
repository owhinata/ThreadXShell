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
]

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

        for name, want in CASES:
            p = subprocess.run([gate_path, os.path.join(tmp, name)],
                               capture_output=True, text=True)
            text = p.stdout + p.stderr
            if want is None:
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
            if ok:
                print(f"  ok   {args.gate} {name}: {why}")
            else:
                failures += 1
                print(f"  FAIL {args.gate} {name}: wanted {why}, got rc "
                      f"{p.returncode}:\n{text}")

    failures += schema_depth_check(flags)

    if failures:
        sys.exit(f"run_model_io_gate_tests: {failures} case(s) failed for {gate_path}")
    print(f"run_model_io_gate_tests: OK ({len(CASES)} cases + schema depth, {gate_path})")


if __name__ == "__main__":
    main()
