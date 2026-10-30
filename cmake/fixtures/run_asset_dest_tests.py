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
