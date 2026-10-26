#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 ThreadX Shell Project
"""Negative tests for the pin's re-check at its first consumer (issue #132).

cmake/fetch_model.cmake verifies a pinned model when it publishes it, and the
first consumer of that file checks the pin again on the bytes it reads: the
packer (asset/tools/pack_container.py --model-sha256) when the model is packed
as fetched, or cmake/run_with_pinned_input.py in front of an ingest tool.  Both
halves are tested here, with no toolchain and no board:

  - the packer's pure check, on in-memory bytes: match, a one-byte flip, empty,
    malformed;
  - the packer end to end: a mismatch exits non-zero with the mismatch sentence
    and writes NO output, and a match gets past the check (and then fails on the
    deliberately absent layout -- a different sentence, which is the proof the
    hash check let it through);
  - the wrapper: a mismatch runs nothing and writes nothing, an empty hash is
    refused, a match runs the command and its exit code is propagated;
  - cmake/build_asset.py's pass-through, against a fake packer that records its
    argv: a hash reaches the packer as `--model-sha256 <hex>`, an empty one
    reaches it as NO option at all, and a missing option is refused.

[!] EVERY REFUSAL IS PAIRED WITH THE SAME CALL PASSING BY ONE CHANGE, so a
harness that could not run the tool at all would not look like a working gate.
"""
import hashlib
import importlib.util
import json
import os
import subprocess
import sys
import tempfile

sys.dont_write_bytecode = True   # loads the tools from the source tree
HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
PACKER = os.path.join(REPO, "asset", "tools", "pack_container.py")
WRAPPER = os.path.join(REPO, "cmake", "run_with_pinned_input.py")
BUILD_ASSET = os.path.join(REPO, "cmake", "build_asset.py")

MODEL = bytes(range(256)) * 4
GOOD = hashlib.sha256(MODEL).hexdigest()
FLIPPED = bytes([MODEL[0] ^ 1]) + MODEL[1:]


def load(path, name):
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


bad = 0


def check(name, ok, why):
    global bad
    print("  %s %-28s %s" % ("ok  " if ok else "FAIL", name, why))
    if not ok:
        bad += 1


def pure_cases():
    pc = load(PACKER, "pack_container")
    check("pack_pure_match", pc.model_sha256_error(MODEL, GOOD) is None,
          "the pinned bytes pass")
    e = pc.model_sha256_error(FLIPPED, GOOD) or ""
    check("pack_pure_flip", "sha256 mismatch" in e and GOOD in e,
          "one flipped byte is a mismatch naming the pin")
    e = pc.model_sha256_error(MODEL, "") or ""
    check("pack_pure_empty", "is empty" in e,
          "an empty hash is refused, not 'no check'")
    e = pc.model_sha256_error(MODEL, GOOD.upper()) or ""
    check("pack_pure_malformed", "64 lowercase hex" in e,
          "a malformed hash is refused")
    w = load(WRAPPER, "run_with_pinned_input")
    with tempfile.TemporaryDirectory() as d:
        f = os.path.join(d, "m.tflite")
        open(f, "wb").write(MODEL)
        check("wrap_pure_match", w.sha256_error(f, GOOD) is None,
              "the pinned file passes")
        open(f, "wb").write(FLIPPED)
        e = w.sha256_error(f, GOOD) or ""
        check("wrap_pure_flip", ("pinned input %s: sha256 mismatch" % f) in e,
              "one flipped byte is a mismatch")
        check("wrap_pure_missing",
              "cannot read" in (w.sha256_error(f + ".x", GOOD) or ""),
              "a missing file is a refusal")


def packer_cmd(d, model, sha):
    return [sys.executable, PACKER, "--layout", os.path.join(d, "no.json"),
            "--elf", "x", "--model", model, "--name", "n", "--build-id", "b",
            "--out", os.path.join(d, "out", "a.nnc"), "--nm", "nm",
            "--objcopy", "objcopy", "--target-id", "1", "--sink", "0",
            "--accounting", "1", "--model-sha256=" + sha]


def packer_cases():
    with tempfile.TemporaryDirectory() as d:
        m = os.path.join(d, "m.tflite")
        out = os.path.join(d, "out")
        for name, data, sha, want in (
                ("pack_e2e_flip", FLIPPED, GOOD, "sha256 mismatch"),
                ("pack_e2e_empty", MODEL, "", "is empty")):
            open(m, "wb").write(data)
            r = subprocess.run(packer_cmd(d, m, sha), capture_output=True,
                               text=True)
            check(name, r.returncode != 0 and want in r.stderr
                  and not os.path.exists(out),
                  "exits %d, says %r, and writes nothing" % (r.returncode, want))
        open(m, "wb").write(MODEL)
        r = subprocess.run(packer_cmd(d, m, GOOD), capture_output=True,
                           text=True)
        # Fails later, on the absent layout: past the check, for another reason.
        check("pack_e2e_match", r.returncode != 0
              and "sha256" not in r.stderr and "no.json" in r.stderr,
              "the pinned bytes get past the check")


def wrapper_cases():
    with tempfile.TemporaryDirectory() as d:
        f = os.path.join(d, "m.tflite")
        mark = os.path.join(d, "ran")
        # The consumer: records that it ran, exits 7.
        cmd = [sys.executable, "-c",
               "import sys; open(sys.argv[1], 'w').write('x'); sys.exit(7)",
               mark]

        def wrap(sha):
            if os.path.exists(mark):
                os.unlink(mark)
            return subprocess.run([sys.executable, WRAPPER, "--sha256", sha,
                                   "--file", f, "--"] + cmd,
                                  capture_output=True, text=True)

        open(f, "wb").write(FLIPPED)
        r = wrap(GOOD)
        check("wrap_e2e_flip", r.returncode != 0 and not os.path.exists(mark)
              and ("pinned input %s: sha256 mismatch" % f) in r.stderr,
              "mismatch: non-zero, the command did not run")
        open(f, "wb").write(MODEL)
        r = wrap("")
        check("wrap_e2e_empty", r.returncode != 0 and not os.path.exists(mark)
              and "--sha256 is empty" in r.stderr,
              "an empty hash: refused, the command did not run")
        r = wrap(GOOD)
        check("wrap_e2e_match", r.returncode == 7 and os.path.exists(mark),
              "match: the command ran and its exit code 7 came back")


def build_asset_cases():
    with tempfile.TemporaryDirectory() as d:
        def put(name, text):
            path = os.path.join(d, name)
            with open(path, "w") as fh:
                fh.write(text)
            return path
        argv_log = os.path.join(d, "packer.argv")
        # Records its argv and refuses, so build_asset stops right after the
        # packer call -- the only thing under test is what it was given.
        packer = put("packer.py",
                     "import json, sys\n"
                     "json.dump(sys.argv[1:], open(%r, 'w'))\n"
                     "sys.exit(1)\n" % argv_log)
        layout = put("layout.json", '{"slot": {"entry": 0}}')
        stacks = put("stacks.json",
                     '{"accounting": 1, "sink": 0, "entries": {}}')
        model = put("m.tflite", "m")
        tool = put("tool", "")
        env = dict(os.environ, ASSET_NM="nm", ASSET_OBJCOPY="objcopy")

        def run(sha_args):
            if os.path.exists(argv_log):
                os.unlink(argv_log)
            r = subprocess.run(
                [sys.executable, BUILD_ASSET, "--name", "n", "--model", model,
                 "--plugin-elf", tool, "--plugin-stacks", stacks,
                 "--packer", packer, "--layout", layout,
                 "--model-verifier", tool, "--container-verifier", tool,
                 "--build-id", "b", "--target-id", "1", "--link-addr", "0",
                 "--capacity", "1", "--policy-stack", "entry=8",
                 "--veneer-cost", "0", "--out", os.path.join(d, "o", "a.nnc")]
                + sha_args, capture_output=True, text=True, env=env)
            got = None
            if os.path.exists(argv_log):
                with open(argv_log) as fh:
                    got = json.load(fh)
            return r, got

        r, got = run(["--model-sha256=" + GOOD])
        ok = got is not None and any(
            got[i:i + 2] == ["--model-sha256", GOOD] for i in range(len(got)))
        check("asset_passes_hash", ok,
              "a hash reaches the packer as --model-sha256 <hex>")
        r, got = run(["--model-sha256="])
        check("asset_empty_omits_option", got is not None and not any(
              a.startswith("--model-sha256") for a in got),
              "empty: the packer is given no --model-sha256 at all")
        r, got = run([])
        check("asset_missing_refused", r.returncode != 0 and got is None
              and "--model-sha256" in r.stderr,
              "absent: refused by argparse, the packer never runs")


def main():
    pure_cases()
    packer_cases()
    wrapper_cases()
    build_asset_cases()
    if bad:
        print("run_pinned_input_tests: FAILED", file=sys.stderr)
        return 1
    print("run_pinned_input_tests: all cases behave as specified")
    return 0


if __name__ == "__main__":
    sys.exit(main())
