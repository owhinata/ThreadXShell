#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 ThreadX Shell Project
"""Negative tests for an asset's destination and receipt (issue #159).

Since #159 an asset names EXACTLY ONE destination -- a store slot (Grove, wio)
or a file on the board's card (f746) -- and the receipt prints the board's own
transfer and verify lines.  add_asset()'s configure-time refusals are in
run_add_plugin_arg_tests.py; this covers the two scripts behind it:

  - cmake/build_asset.py refuses no destination, both, and half of either,
    BEFORE anything is packed (it used to skip the fit check on a call with
    neither -- a fail-open);
  - cmake/asset_receipt.py prints, for a slot board given the text Grove and
    wio pass, EXACTLY the receipt it printed before (byte for byte), a file
    destination's own lines, and refuses none / both;
  - build_asset.py, run to the end with stand-ins for the packer and the two
    verifiers, publishes a container that fits its slot exactly and refuses
    one a byte over, a slot the table does not hold, a missing --slot or
    --slot-table, and an empty or non-numeric slot -- each refusal leaving
    no .nnc, no receipt JSON and no private working directory behind
    (issue #195: until then nothing exercised the fit check itself);
  - with --cc (the pinned arm-none-eabi-gcc): build_asset.stage_capacity()
    reads a staging capacity out of a real ARM ELF, and refuses a missing
    symbol, a writable one, a zero and a value that is not a uint32_t.

[!] EVERY REFUSAL IS ASSERTED ON ITS OWN SENTENCE, and each control passes, so
a case that fails for some other reason is a test failure, not a pass.
"""
import argparse
import json
import os
import subprocess
import sys
import tempfile
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
CMAKE_DIR = os.path.dirname(HERE)
BUILD_ASSET = os.path.join(CMAKE_DIR, "build_asset.py")
RECEIPT = os.path.join(CMAKE_DIR, "asset_receipt.py")
sys.dont_write_bytecode = True

bad = 0


def judge(name, ok, detail=""):
    global bad
    print("  %s %-28s %s" % ("ok  " if ok else "FAIL", name, detail if not ok else ""))
    if not ok:
        bad += 1


# --- build_asset.py: exactly one destination --------------------------------
BASE = ["--name", "x", "--model", "m", "--model-sha256=", "--plugin-elf", "p",
        "--plugin-stacks", "s", "--packer", "k", "--layout", "l",
        "--model-verifier", "v", "--container-verifier", "c",
        "--build-id", "b", "--target-id", "0x1101", "--link-addr", "0",
        "--capacity", "1", "--veneer-cost", "1", "--out", "/nonexistent/o"]
DEST_CASES = [
    ("dest_none", [], "no destination: --slot with --slot-table"),
    ("dest_both", ["--slot", "5", "--slot-table", "t", "--stage-elf", "e",
                   "--stage-symbol", "s"], "both a slot destination"),
    ("dest_half_slot", ["--slot", "5"], "--slot and --slot-table go together"),
    ("dest_half_table", ["--slot-table", "t"],
     "--slot and --slot-table go together"),
    ("dest_half_file", ["--stage-elf", "e"],
     "--stage-elf and --stage-symbol go together"),
    # Whole destinations get PAST the check: the next refusal is a missing
    # tool, which proves the destination was not what stopped them.
    ("dest_slot_passes", ["--slot", "5", "--slot-table", "t"],
     "model verifier missing"),
    ("dest_file_passes", ["--stage-elf", "e", "--stage-symbol", "s"],
     "model verifier missing"),
]


def dest_cases():
    for name, extra, want in DEST_CASES:
        r = subprocess.run([sys.executable, BUILD_ASSET] + BASE + extra,
                           capture_output=True, text=True)
        out = r.stdout + r.stderr
        judge(name, r.returncode != 0 and want in out, out.strip()[:200])


# --- build_asset.py, run to the end: the fit check and what it publishes ----
# [!] THE ONLY CASES THAT GET PAST THE PACKER (issue #195).  The ones above stop
# at the destination check or at the first missing tool, so none of them would
# notice a fit check that was removed, loosened, made conditional, or moved
# after the publication.  Here the packer and both verifiers are stand-ins:
# the packer writes a container of exactly FIT_SIZE bytes (one model section,
# the layout below), the verifiers accept, and the slot table is this
# fixture's.  What is under test is only build_asset.py's own order: pack,
# verify, check the fit, and only then publish.
FIT_SLOT, FIT_CAP = 5, 1000
FIT_TABLE = {"unit": 4096, "slots": [
    {"index": 0, "base": 0, "payload_max": 4096},
    {"index": FIT_SLOT, "base": 65536, "payload_max": FIT_CAP}]}
# Only the keys build_asset.py reads: the section table for extract_model(),
# the slot index for --policy-stack, and the accounting version.
FIT_LAYOUT = {"hdr": {"section_count": 0, "sections": 4}, "section_size": 12,
              "section": {"type": 0, "offset": 4, "length": 8},
              "section_type": {"model": 2}, "slot": {"entry": 0},
              "stack_accounting": 2}
FIT_PACKER = r"""
import os, struct, sys
a = sys.argv[1:]
out = a[a.index("--out") + 1]
model = open(a[a.index("--model") + 1], "rb").read()
size = int(os.environ["ASSET_TEST_SIZE"])
head = struct.pack("<IIII", 1, 2, 16, len(model)) + model
assert size >= len(head)
open(out, "wb").write(head + b"\0" * (size - len(head)))
"""
FIT_ACCEPT = "#!%s\nimport sys\nsys.exit(0)\n" % sys.executable


def fit_cases(work):
    tools = os.path.join(work, "fit-tools")
    os.makedirs(tools)

    def put(name, text, mode=0o644):
        path = os.path.join(tools, name)
        with open(path, "w") as fh:
            fh.write(text)
        os.chmod(path, mode)
        return path

    packer = put("packer.py", FIT_PACKER)
    model_v = put("model_verifier", FIT_ACCEPT, 0o755)
    cont_v = put("container_verifier", FIT_ACCEPT, 0o755)
    layout = put("layout.json", json.dumps(FIT_LAYOUT))
    table = put("slot_table.json", json.dumps(FIT_TABLE))
    model = put("model.tflite", "TFL3mdl")
    stacks = put("plugin.stacks.json", json.dumps(
        {"accounting": 2, "sink": 0, "entries": {}}))
    plugin = put("plugin.elf", "not an ELF; only its existence is checked")
    env = dict(os.environ, ASSET_NM="nm", ASSET_OBJCOPY="objcopy")

    def build(name, size, dest):
        out_dir = os.path.join(work, "fit-out-" + name)
        os.makedirs(out_dir)
        out = os.path.join(out_dir, "x.nnc")
        cmd = [sys.executable, BUILD_ASSET, "--name", "x", "--model", model,
               "--model-sha256=", "--plugin-elf", plugin,
               "--plugin-stacks", stacks, "--packer", packer,
               "--layout", layout, "--model-verifier", model_v,
               "--container-verifier", cont_v, "--build-id", "b",
               "--target-id", "0x1101", "--link-addr", "0", "--capacity", "1",
               "--policy-stack", "entry=100", "--veneer-cost", "1",
               "--out", out] + dest
        r = subprocess.run(cmd, capture_output=True, text=True,
                           env=dict(env, ASSET_TEST_SIZE="%d" % size))
        return r.returncode, r.stdout + r.stderr, out, sorted(os.listdir(out_dir))

    def refused(name, size, dest, want):
        rc, out, nnc, left = build(name, size, dest)
        # Its own sentence, not a traceback that happens to exit non-zero, and
        # nothing in the output directory: no .nnc, no receipt, no work dir.
        judge(name, rc != 0 and want in out and "Traceback" not in out
              and left == [], "rc=%d left=%r\n%s" % (rc, left, out.strip()[:400]))

    slot = ["--slot", "%d" % FIT_SLOT, "--slot-table", table]
    rc, out, nnc, left = build("fit_exact_publishes", FIT_CAP, slot)
    ok = rc == 0 and left == ["x.nnc", "x.nnc.json"]
    if ok:
        data = open(nnc, "rb").read()
        rec = json.load(open(nnc + ".json"))
        ok = (len(data) == FIT_CAP and rec["bytes"] == FIT_CAP and
              rec["crc32"] == "%08X" % (zlib.crc32(data) & 0xFFFFFFFF) and
              rec["path"] == os.path.abspath(nnc) and
              (os.stat(nnc).st_mode & 0o777) == 0o444)
    judge("fit_exact_publishes", ok, "rc=%d left=%r\n%s" % (rc, left, out))
    refused("fit_one_byte_over", FIT_CAP + 1, slot,
            "x is %d B and slot %d holds %d B" % (FIT_CAP + 1, FIT_SLOT, FIT_CAP))
    refused("fit_slot_not_in_table", FIT_CAP,
            ["--slot", "7", "--slot-table", table], "slot 7 is not in the map")
    refused("fit_no_slot", FIT_CAP, ["--slot-table", table],
            "--slot and --slot-table go together")
    refused("fit_no_slot_table", FIT_CAP, ["--slot", "%d" % FIT_SLOT],
            "--slot and --slot-table go together")
    for name, bad_slot in (("fit_slot_empty", ""), ("fit_slot_letters", "5x"),
                           ("fit_slot_space", " 5"), ("fit_slot_negative", "-1")):
        refused(name, FIT_CAP, ["--slot", bad_slot, "--slot-table", table],
                "--slot %r is not a slot number" % bad_slot)


# --- asset_receipt.py --------------------------------------------------------
# What the receipt printed before #159 for wio's blazeface (the text is wio's
# and Grove's; only the path, size and CRC are this fixture's).
SLOT_WANT = """
  asset blazeface    {path}
  slot 5             {n} B   crc32 {crc}

  on the board:  blob write 5   (erases the slot, then waits for the file)
  in picocom:    C-a C-s, then the path above
  afterwards:    `blob list` must show crc32 {crc} -- that is how you know
                 the bytes that were built are the bytes that were stored.
  then:          nn model load --slot 5

"""
SLOT_ARGS = ["--step", "blob write {slot}   (erases the slot, then waits for the file)",
             "--then", "nn model load --slot {slot}",
             "--transfer-label", "in picocom:",
             "--transfer", "C-a C-s, then the path above",
             "--verify-label", "afterwards:",
             "--verify", "`blob list` must show crc32 {crc} -- that is how you know",
             "--verify", "the bytes that were built are the bytes that were stored."]
FILE_ARGS = ["--file", "blazeface.nnc",
             "--step", "nn model unload   (optional)",
             "--transfer-label", "on the PC:",
             "--transfer", "copy the path above to the card as /{file}",
             "--verify-label", "afterwards:",
             "--verify", "`nn model load --path /{file}` must print crc32 {crc}"]


def receipt_cases(work):
    nnc = os.path.join(work, "blazeface.nnc")
    with open(nnc, "wb") as fh:
        fh.write(b"fixture container bytes")
    data = open(nnc, "rb").read()
    crc = "%08X" % (zlib.crc32(data) & 0xFFFFFFFF)
    rec = nnc + ".json"
    with open(rec, "w") as fh:
        json.dump({"name": "blazeface", "bytes": len(data), "crc32": crc,
                   "path": nnc}, fh)

    def run(args):
        r = subprocess.run([sys.executable, RECEIPT, rec] + args,
                           capture_output=True, text=True)
        return r.returncode, r.stdout, r.stderr

    rc, out, err = run(["5"] + SLOT_ARGS)
    want = SLOT_WANT.format(path=nnc, n=len(data), crc=crc)
    judge("receipt_slot_unchanged", rc == 0 and out == want,
          "got:\n%s\nwant:\n%s%s" % (out, want, err))
    rc, out, err = run(FILE_ARGS)
    judge("receipt_file", rc == 0 and
          "  file blazeface.nnc" in out and
          "  on the PC:     copy the path above to the card as /blazeface.nnc"
          in out and ("must print crc32 %s" % crc) in out and
          "picocom" not in out and "blob list" not in out, out + err)
    rc, out, err = run(SLOT_ARGS)
    judge("receipt_no_destination", rc != 0 and "exactly one destination" in
          out + err, out + err)
    rc, out, err = run(["5"] + FILE_ARGS)
    judge("receipt_both", rc != 0 and "exactly one destination" in out + err,
          out + err)
    rc, out, err = run(["5", "--step", "s", "--verify-label", "a",
                        "--verify", "v"])
    judge("receipt_no_transfer", rc != 0 and "--transfer" in err, err)


# --- stage_capacity() on a real ARM image ------------------------------------
CAP_CASES = [
    ("cap_ok", "const unsigned int nn_stage_capacity = 524288u;", 524288, None),
    ("cap_missing", "const unsigned int other_name = 524288u;", None,
     "0 symbols named nn_stage_capacity"),
    ("cap_writable", "unsigned int nn_stage_capacity = 524288u;", None,
     "a writable section"),
    ("cap_zero", "const unsigned int nn_stage_capacity = 0u;", None,
     "not a capacity"),
    ("cap_u16", "const unsigned short nn_stage_capacity = 4096u;", None,
     "is 2 B, not a uint32_t"),
]


def cap_cases(cc, work):
    sys.path.insert(0, CMAKE_DIR)
    import build_asset                       # noqa: E402 -- path set above
    for name, src, want, refusal in CAP_CASES:
        c = os.path.join(work, name + ".c")
        elf = os.path.join(work, name + ".elf")
        with open(c, "w") as fh:
            fh.write(src + "\nint start(void) { return 0; }\n")
        r = subprocess.run([cc, "-mcpu=cortex-m7", "-mthumb", "-nostdlib",
                            "-nostartfiles", "-Wl,-e,start",
                            "-Wl,--no-gc-sections", "-Wl,-Ttext=0x08000000",
                            "-Wl,-Tdata=0x20010000", c, "-o", elf],
                           capture_output=True, text=True)
        if r.returncode != 0:
            judge(name, False, "did not link: " + r.stderr[:200])
            continue
        try:
            got = build_asset.stage_capacity(elf, "nn_stage_capacity")
            judge(name, want is not None and got == want,
                  "got %r, want %r" % (got, want))
        except SystemExit as exc:
            msg = str(exc)
            judge(name, refusal is not None and refusal in msg, msg)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cc", help="arm-none-eabi-gcc, for the ELF read-back")
    args = ap.parse_args()
    print("run_asset_dest_tests:")
    dest_cases()
    with tempfile.TemporaryDirectory() as work:
        fit_cases(work)
        receipt_cases(work)
        if args.cc:
            cap_cases(args.cc, work)
        else:
            print("  SKIPPED the staging-capacity read-back: no --cc "
                  "(boards/f746g-disco/test/host_tests.sh runs it with the "
                  "pinned toolchain)", file=sys.stderr)
    if bad:
        print("run_asset_dest_tests: %d case(s) FAILED" % bad, file=sys.stderr)
        return 1
    print("run_asset_dest_tests: all cases behave as specified")
    return 0


if __name__ == "__main__":
    sys.exit(main())
