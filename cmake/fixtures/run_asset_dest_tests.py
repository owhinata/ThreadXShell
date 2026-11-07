#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 ThreadX Shell Project
"""Negative tests for an asset's destination and receipt (issues #159, #195).

An asset goes to a store slot, named by --slot with the board's --slot-table
(the file destination #159 added was removed in #195), and the receipt prints
one text for every board, the shared `blob` command's (#195).  add_asset()'s configure-time
refusals are in run_add_plugin_arg_tests.py; this covers the two scripts
behind it:

  - cmake/build_asset.py refuses a missing --slot or --slot-table, and the
    removed --stage-elf / --stage-symbol, BEFORE anything is packed (it used
    to skip the fit check on a call with neither -- a fail-open);
  - cmake/asset_receipt.py prints EXACTLY the receipt below (byte for byte),
    for a one-digit and a two-digit slot, and refuses a missing or
    non-numeric slot and every removed option (--file, --step, --then,
    --transfer-label, --transfer, --verify-label, --verify);
  - build_asset.py, run to the end with stand-ins for the packer and the two
    verifiers, publishes a container that fits its slot exactly and refuses
    one a byte over, a slot the table does not hold, a missing --slot or
    --slot-table, a slot table that is missing, empty-named or not JSON, an
    empty or non-numeric slot, and the removed file options -- each refusal
    leaving no .nnc, no receipt JSON and no private working directory behind
    (issue #195: until then nothing exercised the fit check itself).

[!] EVERY REFUSAL IS ASSERTED ON ITS OWN SENTENCE, and each control passes, so
a case that fails for some other reason is a test failure, not a pass.
"""
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


# issue #205 -- the last line every negative-test runner prints when it passes:
# this file's path from the repository root and how many cases this run decided.
# The count goes up where a verdict is reached, not from the length of a list.
decided = 0


def negative_tests_ok():
    me = os.path.realpath(__file__)
    root = os.path.dirname(os.path.dirname(os.path.dirname(me)))
    print("negative-tests: %s OK (%d cases)"
          % (os.path.relpath(me, root), decided))


def judge(name, ok, detail=""):
    global bad, decided
    decided += 1
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
    ("dest_none", [],
     "the following arguments are required: --slot, --slot-table"),
    ("dest_half_slot", ["--slot", "5"],
     "the following arguments are required: --slot-table"),
    ("dest_half_table", ["--slot-table", "t"],
     "the following arguments are required: --slot"),
    # Issue #195: the file destination's options are gone, and a call that
    # still passes one is refused by name, even beside a whole slot.
    ("dest_stage_elf_gone", ["--slot", "5", "--slot-table", "t",
                             "--stage-elf", "e"],
     "unrecognized arguments: --stage-elf e"),
    ("dest_stage_symbol_gone", ["--slot", "5", "--slot-table", "t",
                                "--stage-symbol", "s"],
     "unrecognized arguments: --stage-symbol s"),
    # A whole slot gets PAST the check: the next refusal is a missing tool,
    # which proves the destination was not what stopped it.
    ("dest_slot_passes", ["--slot", "5", "--slot-table", "t"],
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
            "the following arguments are required: --slot")
    refused("fit_no_slot_table", FIT_CAP, ["--slot", "%d" % FIT_SLOT],
            "the following arguments are required: --slot-table")
    refused("fit_slot_table_absent", FIT_CAP,
            ["--slot", "%d" % FIT_SLOT, "--slot-table",
             os.path.join(tools, "no_such_table.json")], "slot table missing")
    refused("fit_slot_table_empty_name", FIT_CAP,
            ["--slot", "%d" % FIT_SLOT, "--slot-table", ""],
            "slot table missing")
    refused("fit_slot_table_not_json", FIT_CAP,
            ["--slot", "%d" % FIT_SLOT, "--slot-table", model],
            "cannot read the slot table")
    # [!] The table's rows are checked, all of them, before the fit (issue
    # #195).  Each table below would let a 5,000 B container through a check
    # that only looked up the declared row; the last breaks a row that is NOT
    # the declared one.  NaN / Infinity are what Python's json accepts.
    big = 5 * FIT_CAP
    for tname, text, use_slot, want in (
            ("fit_table_nan", '{"slots": [{"index": 5, "payload_max": NaN}]}',
             "5", "is not whole numbers"),
            ("fit_table_infinity",
             '{"slots": [{"index": 5, "payload_max": Infinity}]}', "5",
             "is not whole numbers"),
            ("fit_table_index_twice",
             '{"slots": [{"index": 5, "payload_max": 100000}, '
             '{"index": 5, "payload_max": 1000}]}', "5", "names slot 5 twice"),
            ("fit_table_index_bool",
             '{"slots": [{"index": true, "payload_max": 100000}]}', "1",
             "is not whole numbers"),
            ("fit_table_no_payload_key", '{"slots": [{"index": 5}]}', "5",
             "is not whole numbers"),
            ("fit_table_payload_string",
             '{"slots": [{"index": 5, "payload_max": "100000"}]}', "5",
             "is not whole numbers"),
            ("fit_table_payload_negative",
             '{"slots": [{"index": 5, "payload_max": -1}]}', "5",
             "is not whole numbers"),
            ("fit_table_slots_not_list", '{"slots": {"5": 100000}}', "5",
             "has no list of slots"),
            ("fit_table_slots_empty", '{"slots": []}', "5",
             "has no list of slots"),
            ("fit_table_other_row_broken",
             '{"slots": [{"index": 0, "payload_max": NaN}, '
             '{"index": 5, "payload_max": 100000}]}', "5",
             "is not whole numbers")):
        path = put(tname + ".json", text)
        refused(tname, big, ["--slot", use_slot, "--slot-table", path], want)
    for opt in ("--stage-elf", "--stage-symbol"):
        refused("fit" + opt.replace("-", "_")[1:] + "_gone", FIT_CAP,
                slot + [opt, plugin], "unrecognized arguments: " + opt)
    for name, bad_slot in (("fit_slot_empty", ""), ("fit_slot_letters", "5x"),
                           ("fit_slot_space", " 5"), ("fit_slot_negative", "-1")):
        refused(name, FIT_CAP, ["--slot", bad_slot, "--slot-table", table],
                "--slot %r is not a slot number" % bad_slot)


# --- asset_receipt.py --------------------------------------------------------
# [!] THE RECEIPT, WRITTEN OUT IN FULL (issue #195) -- not built from the same
# template the script uses, so a changed word, a lost line or a slot other than
# the one passed shows here.  Two cases: Grove's cifar10 in slot 1 and the
# blazeface wio and f746 send to slot 12; only path, size and CRC are the
# fixture's.  The refusal lines follow shell/cmds/cmd_blob.c: another blob in
# the slot, or the same name in another slot, is refused before any erase.
RECEIPT_WANT = {
    ("blazeface", "12"): """
  asset blazeface    {path}
  slot 12            {n} B   crc32 {crc}

  on the board:  blob write blazeface 12   (erases the slot, then waits for the file)
                 refused if slot 12 holds another name, or blazeface is
                 already in another slot: `blob list` shows which, and
                 `blob erase <that slot>` clears it on purpose
  in picocom:    C-a C-s, then the path above
  afterwards:    `blob list` must show crc32 {crc} -- that is how you know
                 the bytes that were built are the bytes that were stored.
  then:          nn model load blazeface

""",
    ("cifar10", "1"): """
  asset cifar10      {path}
  slot 1             {n} B   crc32 {crc}

  on the board:  blob write cifar10 1   (erases the slot, then waits for the file)
                 refused if slot 1 holds another name, or cifar10 is
                 already in another slot: `blob list` shows which, and
                 `blob erase <that slot>` clears it on purpose
  in picocom:    C-a C-s, then the path above
  afterwards:    `blob list` must show crc32 {crc} -- that is how you know
                 the bytes that were built are the bytes that were stored.
  then:          nn model load cifar10

""",
}
# Each removed option, with a value, beside a whole call.
RECEIPT_GONE = ["--file", "--step", "--then", "--transfer-label", "--transfer",
                "--verify-label", "--verify"]


def receipt_cases(work):
    def fixture(name, body):
        # [!] The file is NOT named after the asset, so a receipt that took
        # its name from the path instead of the JSON's "name" shows (#195).
        nnc = os.path.join(work, "built-" + name + ".bin")
        with open(nnc, "wb") as fh:
            fh.write(body)
        crc = "%08X" % (zlib.crc32(body) & 0xFFFFFFFF)
        with open(nnc + ".json", "w") as fh:
            json.dump({"name": name, "bytes": len(body), "crc32": crc,
                       "path": nnc}, fh)
        return nnc, crc

    def run(rec, args):
        r = subprocess.run([sys.executable, RECEIPT, rec] + args,
                           capture_output=True, text=True)
        return r.returncode, r.stdout, r.stderr

    for (name, slot), text in sorted(RECEIPT_WANT.items()):
        body = ("fixture container bytes for %s" % name).encode()
        nnc, crc = fixture(name, body)
        rc, out, err = run(nnc + ".json", [slot])
        want = text.format(path=nnc, n=len(body), crc=crc)
        judge("receipt_%s_slot_%s" % (name, slot), rc == 0 and out == want,
              "got:\n%s\nwant:\n%s%s" % (out, want, err))
    # [!] THE CRC IS RECOMPUTED FROM THE FILE (issue #195): a receipt whose
    # crc32 is not the file's, a file changed after its receipt, and a receipt
    # pointing at no file are refused in a sentence, with no instruction
    # printed -- the CRC is the operator's only "built = stored" check.
    for name, how in (("receipt_crc_mismatch", "json"),
                      ("receipt_file_changed", "file"),
                      ("receipt_file_missing", "gone")):
        nnc, crc = fixture(name, b"fixture container bytes for " +
                           name.encode())
        if how == "json":
            bad_crc = ("0" if crc[0] != "0" else "1") + crc[1:]
            with open(nnc + ".json", "w") as fh:
                json.dump({"name": name, "bytes": os.path.getsize(nnc),
                           "crc32": bad_crc, "path": nnc}, fh)
            want = "does not match its receipt"
        elif how == "file":
            data = bytearray(open(nnc, "rb").read())
            data[0] ^= 0x01
            with open(nnc, "wb") as fh:
                fh.write(data)
            want = "does not match its receipt"
        else:
            os.unlink(nnc)
            want = "%s is missing" % nnc
        rc, out, err = run(nnc + ".json", ["12"])
        judge(name, rc != 0 and want in err and "Traceback" not in err and
              "on the board:" not in out, out + err)
    rec = os.path.join(work, "built-blazeface.bin.json")
    rc, out, err = run(rec, [])
    judge("receipt_no_slot", rc != 0 and out == "" and
          "the following arguments are required: slot" in err, out + err)
    for name, bad_slot in (("receipt_slot_empty", ""),
                           ("receipt_slot_letters", "5x")):
        rc, out, err = run(rec, [bad_slot])
        judge(name, rc != 0 and out == "" and
              ("the slot must be a slot number, got %r" % bad_slot) in err,
              out + err)
    # Issue #195: the file destination's --file and the board's own receipt
    # lines are gone, each refused by name.
    for opt in RECEIPT_GONE:
        rc, out, err = run(rec, ["12", opt, "x"])
        judge("receipt%s_gone" % opt.replace("-", "_")[1:], rc != 0 and
              out == "" and ("unrecognized arguments: %s x" % opt) in err,
              out + err)


def main():
    print("run_asset_dest_tests:")
    dest_cases()
    with tempfile.TemporaryDirectory() as work:
        fit_cases(work)
        receipt_cases(work)
    if bad:
        print("run_asset_dest_tests: %d case(s) FAILED" % bad, file=sys.stderr)
        return 1
    negative_tests_ok()
    return 0


if __name__ == "__main__":
    sys.exit(main())
