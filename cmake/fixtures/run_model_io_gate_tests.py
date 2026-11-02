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
]


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
                ok = OK_LINE[args.style] in text and DIMS not in text \
                    and FORMAT not in text
                if args.style == "tflite":
                    ok = ok and p.returncode == 0
                why = "the walk's ok line" + (" and rc 0" if args.style == "tflite" else "")
            else:
                other = FORMAT if want == DIMS else DIMS
                ok = p.returncode == 1 and want in text and other not in text \
                    and OK_LINE[args.style] not in text
                why = f"rc 1 with {want} (and not {other})"
            if ok:
                print(f"  ok   {args.gate} {name}: {why}")
            else:
                failures += 1
                print(f"  FAIL {args.gate} {name}: wanted {why}, got rc "
                      f"{p.returncode}:\n{text}")

    if failures:
        sys.exit(f"run_model_io_gate_tests: {failures} case(s) failed for {gate_path}")
    print(f"run_model_io_gate_tests: OK ({len(CASES)} cases, {gate_path})")


if __name__ == "__main__":
    main()
