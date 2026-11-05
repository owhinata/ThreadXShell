#!/usr/bin/env python3
"""Tests for cmake/check_blob_staging.py (issue #193; shared by wio-lite-ai
and f746g-disco since issue #194, run from each board's test/host_tests.sh with
--board-dir).

Two halves:

  1. THE NUMBERS THE GATE IS GIVEN OR RESTATES ARE THE FIRMWARE'S.  The gate
     subtracts the --header-bytes the board's board.cmake passes from a slot's
     size and reads the table in ENTRY_BYTES strides.  Both are facts of the C
     side (the board's src/blob.h BLOB_ERASE_UNIT and sizeof(struct blob_slot),
     base first), so the host compiler is asked for them through the real
     headers and each is compared with what the board passes and with the
     gate's.  Change one side alone and this fails -- which is what keeps a
     smaller header from quietly loosening the gate.  The backend lists the
     board passes must also cover every CONFIG_NN_BACKEND it offers.  Needs only
     the host gcc, so it always runs.

  2. SYNTHETIC IMAGES, one rule broken in each (needs --cc, the pinned
     arm-none-eabi-gcc).  The control PASSES; every refusal is asserted on its
     own diagnostic, so a case that starts failing for another reason is a test
     failure rather than a pass.  The boundary is walked on both sides: a slot
     whose payload is exactly the staging capacity passes, one unit more fails.
"""

import argparse
import os
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
CMAKE_DIR = os.path.dirname(HERE)
REPO = os.path.dirname(CMAKE_DIR)
GATE = os.path.join(CMAKE_DIR, "check_blob_staging.py")
sys.path.insert(0, CMAKE_DIR)
import check_blob_staging  # noqa: E402

bad = 0


def judge(name, ok, detail=""):
    global bad
    print("  %s %-24s %s" % ("ok  " if ok else "FAIL", name,
                             "" if ok else detail))
    if not ok:
        bad += 1


# --- 1. the restated constants ----------------------------------------------
CONSTS_SRC = r'''
#include <stddef.h>
#include <stdio.h>
#include "blob.h"
int main(void)
{
	printf("%u %u %u %u\n", (unsigned)BLOB_ERASE_UNIT,
	       (unsigned)sizeof(struct blob_slot),
	       (unsigned)offsetof(struct blob_slot, base),
	       (unsigned)offsetof(struct blob_slot, size));
	return 0;
}
'''


def board_gate_args(board):
    """What the board's board.cmake passes the gate: (header, staged, unstaged,
    offered backends).  [!] Read from the one call: a board.cmake that calls the
    gate twice, or not at all, or passes no --header-bytes, is a failure."""
    with open(os.path.join(board, "board.cmake")) as fh:
        text = fh.read()
    calls = [m.start() for m in re.finditer(
        r"\$\{CMAKE_SOURCE_DIR\}/cmake/check_blob_staging\.py\"", text)]
    if len(calls) != 1:
        return None, "board.cmake calls check_blob_staging.py %d times" % len(
            calls)
    # The call's arguments run to the end of its add_custom_command(...).
    end = text.find("VERBATIM", calls[0])
    call = text[calls[0]:end]
    hdr = re.findall(r"--header-bytes\s+(\d+)", call)
    staged = re.findall(r"--staged-backend\s+([A-Za-z0-9_]+)", call)
    unstaged = re.findall(r"--unstaged-backend\s+([A-Za-z0-9_]+)", call)
    offered = re.search(r"set_property\(CACHE CONFIG_NN_BACKEND PROPERTY "
                        r"STRINGS([^)]*)\)", text)
    if len(hdr) != 1:
        return None, "the gate call passes --header-bytes %d times" % len(hdr)
    if offered is None:
        return None, "board.cmake declares no CONFIG_NN_BACKEND STRINGS"
    return (int(hdr[0]), staged, unstaged, offered.group(1).split()), ""


def const_cases(work, host_cc, board, includes):
    c = os.path.join(work, "consts.c")
    exe = os.path.join(work, "consts")
    with open(c, "w") as fh:
        fh.write(CONSTS_SRC)
    inc = ["-I", os.path.join(board, "src")]
    for d in includes:
        inc += ["-I", os.path.join(board, d)]
    r = subprocess.run([host_cc, "-std=c11"] + inc +
                       ["-I", os.path.join(REPO, "svc"),
                        "-I", os.path.join(REPO, "svc", "blob"),
                        c, "-o", exe], capture_output=True, text=True)
    if r.returncode != 0:
        judge("consts_compile", False, r.stderr[:300])
        return
    unit, entry, off_base, off_size = (int(x) for x in subprocess.run(
        [exe], capture_output=True, text=True, check=True).stdout.split())
    judge("entry_is_struct_size", entry == check_blob_staging.ENTRY_BYTES,
          "sizeof(struct blob_slot) is %d, the gate's ENTRY_BYTES %d"
          % (entry, check_blob_staging.ENTRY_BYTES))
    # The gate unpacks each entry as <base, size>; a swapped struct would be
    # the same size and read every base as a size.
    judge("entry_is_base_then_size", (off_base, off_size) == (0, 4),
          "struct blob_slot has base at %d and size at %d, the gate reads "
          "them at 0 and 4" % (off_base, off_size))
    got, why = board_gate_args(board)
    judge("board_gate_call", got is not None, why)
    if got is None:
        return
    header, staged, unstaged, offered = got
    judge("header_is_erase_unit", unit == header,
          "src/blob.h BLOB_ERASE_UNIT is %d, board.cmake passes "
          "--header-bytes %d" % (unit, header))
    judge("every_backend_listed",
          set(offered) == set(staged) | set(unstaged) and
          not set(staged) & set(unstaged) and bool(staged),
          "the board offers %s; the gate is told staged %s, unstaged %s"
          % (offered, staged, unstaged))


# --- 2. synthetic images ----------------------------------------------------
MB = 0x100000
# Written out, not taken from the gate: a fixture sized from the gate's own
# constant would move with it and agree with any value it was given.
HDR = 4096


def table(sizes, count=None, const="const", name="blob_map_slots",
          cname="blob_map_slots_count"):
    base, rows = 0x100000, []
    for s in sizes:
        rows.append("{ 0x%08xu, 0x%08xu }" % (base, s))
        base += s
    n = len(sizes) if count is None else count
    out = ("struct blob_slot { unsigned base, size; };\n"
           "%s struct blob_slot %s[] = { %s };\n" % (const, name,
                                                     ", ".join(rows)))
    if cname:
        out += "const unsigned %s = %du;\n" % (cname, n)
    return out


def staging(cap=MB, buf=MB, capname="nn_tflm_stage_cap",
            bufname="nn_tflm_model_buf"):
    out = ""
    if capname:
        out += "const unsigned int %s = %du;\n" % (capname, cap)
    if bufname:
        out += "unsigned char %s[%d];\n" % (bufname, buf)
    return out


STD = [MB, MB, 512 * 1024, 256 * 1024]

# What every image case tells the gate unless it says otherwise: the header
# unit and the two backend lists, written out (not read from any board).
STD_ARGS = ["--header-bytes", str(HDR), "--staged-backend", "tflm",
            "--unstaged-backend", "null"]
RELOC_ARGS = STD_ARGS + ["--unstaged-backend", "stedgeai_reloc"]

# (name, backend, source, want rc, substring the output must carry[, args])
CASES = [
    ("control_tflm", "tflm", table(STD) + staging(), 0, "OK -- 4 slots"),
    ("edge_exact_fit", "tflm", table([MB + HDR]) + staging(), 0,
     "0 B to spare"),
    ("slot_too_big", "tflm", table([MB + 2 * HDR]) + staging(), 1,
     "could never be loaded"),
    # The same slot as edge_exact_fit against a buffer ONE BYTE smaller: slot
    # sizes move in header units, so only the capacity can walk the boundary
    # byte by byte, and a comparison loosened by one would pass every case
    # above.
    ("cap_one_byte_short", "tflm",
     table([MB + HDR]) + staging(cap=MB - 1, buf=MB - 1), 1,
     "could never be loaded"),
    ("big_slot_last", "tflm", table(STD + [2 * MB]) + staging(), 1,
     "slot 4 (0x003c0000"),
    ("table_missing", "tflm", table(STD, name="other_table") + staging(), 1,
     "0 symbols named blob_map_slots;"),
    ("count_missing", "tflm", table(STD, cname=None) + staging(), 1,
     "0 symbols named blob_map_slots_count"),
    ("count_mismatch", "tflm", table(STD, count=3) + staging(), 1,
     "says 3 slots but blob_map_slots is 32 B"),
    ("count_zero", "tflm", table(STD, count=0) + staging(), 1,
     "a table with no slots"),
    ("table_writable", "tflm", table(STD, const="") + staging(), 1,
     "a writable section"),
    ("cap_missing", "tflm", table(STD) + staging(capname=None), 1,
     "0 symbols named nn_tflm_stage_cap"),
    ("buf_missing", "tflm", table(STD) + staging(bufname=None), 1,
     "0 symbols named nn_tflm_model_buf"),
    ("buf_smaller_than_cap", "tflm", table(STD) + staging(buf=MB // 2), 1,
     "is only 524288 B"),
    ("cap_zero", "tflm", table(STD) + staging(cap=0), 1, "not a capacity"),
    ("slot_no_payload", "tflm", table([HDR]) + staging(), 1, "no payload"),
    ("control_null", "null", table(STD), 0, "NOT APPLICABLE"),
    ("null_but_buffer", "null", table(STD) + staging(), 1,
     "the build and the image disagree"),
    ("tflm_but_no_buffer", "tflm", table(STD), 1,
     "0 symbols named nn_tflm_stage_cap"),
    ("null_table_missing", "null", table(STD, name="other_table"), 1,
     "0 symbols named blob_map_slots;"),
    # issue #194: the header unit is the board's argument, and it is USED -- a
    # slot one unit over the buffer passes only when the unit is two.
    ("header_arg_used", "tflm", table([MB + 2 * HDR]) + staging(), 0,
     "0 B to spare", ["--header-bytes", str(2 * HDR),
                      "--staged-backend", "tflm"]),
    # issue #194 D4: a backend the board says loads elsewhere is not applicable,
    # checked against the image like null.
    ("reloc_not_applicable", "stedgeai_reloc", table(STD), 0,
     "NOT APPLICABLE -- backend stedgeai_reloc", RELOC_ARGS),
    ("reloc_but_buffer", "stedgeai_reloc", table(STD) + staging(), 1,
     "the build and the image disagree", RELOC_ARGS),
    ("reloc_table_missing", "stedgeai_reloc",
     table(STD, name="other_table"), 1, "0 symbols named blob_map_slots;",
     RELOC_ARGS),
    ("reloc_unlisted", "stedgeai_reloc", table(STD), 2, "unknown backend"),
    ("header_zero", "tflm", table(STD) + staging(), 2, "is not a size",
     ["--header-bytes", "0", "--staged-backend", "tflm"]),
    ("header_absent", "tflm", table(STD) + staging(), 2, "bad arguments",
     ["--staged-backend", "tflm"]),
    ("no_staged_backend", "null", table(STD), 2, "names no staged backend",
     ["--header-bytes", str(HDR), "--unstaged-backend", "null"]),
    ("backend_both_ways", "tflm", table(STD) + staging(), 2,
     "both staged and unstaged",
     STD_ARGS + ["--unstaged-backend", "tflm"]),
]


def image_cases(cc, work):
    for case in CASES:
        name, backend, src, want, needle = case[:5]
        args = case[5] if len(case) > 5 else STD_ARGS
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
        r = subprocess.run([sys.executable, GATE, elf, "--backend", backend]
                           + args, capture_output=True, text=True)
        out = r.stdout + r.stderr
        judge(name, r.returncode == want and needle in out,
              "rc %d (want %d): %s" % (r.returncode, want, out.strip()))

    # A duplicate: the same name in two translation units.
    a = os.path.join(work, "dup_a.c")
    b = os.path.join(work, "dup_b.c")
    elf = os.path.join(work, "dup.elf")
    with open(a, "w") as fh:
        fh.write(table(STD).replace("const struct", "static const struct")
                 + staging() + "\nint start(void) { return 0; }\n"
                 "const void *keep_a(void) { return blob_map_slots; }\n")
    with open(b, "w") as fh:
        fh.write("struct blob_slot { unsigned base, size; };\n"
                 "static const struct blob_slot blob_map_slots[] = "
                 "{ { 1u, 2u } };\n"
                 "const void *keep_b(void) { return blob_map_slots; }\n")
    r = subprocess.run([cc, "-mcpu=cortex-m7", "-mthumb", "-nostdlib",
                        "-nostartfiles", "-Wl,-e,start",
                        "-Wl,--no-gc-sections", "-Wl,-Ttext=0x08000000",
                        "-Wl,-Tdata=0x20010000", a, b, "-o", elf],
                       capture_output=True, text=True)
    if r.returncode != 0:
        judge("table_twice", False, "did not link: " + r.stderr[:200])
    else:
        r = subprocess.run([sys.executable, GATE, elf, "--backend", "tflm"]
                           + STD_ARGS, capture_output=True, text=True)
        judge("table_twice", r.returncode == 1 and
              "2 symbols named blob_map_slots;" in r.stderr, r.stderr)

    # Not an ELF, and an unknown backend: could not check (2), never a pass.
    junk = os.path.join(work, "junk.elf")
    with open(junk, "wb") as fh:
        fh.write(b"not an elf")
    r = subprocess.run([sys.executable, GATE, junk, "--backend", "tflm"]
                       + STD_ARGS, capture_output=True, text=True)
    judge("not_an_elf", r.returncode == 2 and "CANNOT CHECK" in r.stderr,
          r.stderr)
    r = subprocess.run([sys.executable, GATE,
                        os.path.join(work, "control_tflm.elf"),
                        "--backend", "stedgeai"] + STD_ARGS,
                       capture_output=True, text=True)
    judge("unknown_backend", r.returncode == 2 and "CANNOT CHECK" in r.stderr,
          r.stderr)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--board-dir", required=True,
                    help="boards/<board>: its src/blob.h and board.cmake")
    ap.add_argument("--include", action="append", default=[],
                    help="another include directory blob.h needs, relative to "
                         "the board (port/nor, port/qspi)")
    ap.add_argument("--host-cc", default="gcc")
    ap.add_argument("--cc", help="arm-none-eabi-gcc, for the image cases")
    args = ap.parse_args()
    board = os.path.abspath(args.board_dir)
    print("run_blob_staging_tests (%s):" % os.path.basename(board))
    with tempfile.TemporaryDirectory() as work:
        const_cases(work, args.host_cc, board, args.include)
        if args.cc:
            image_cases(args.cc, work)
        else:
            print("  SKIPPED the image cases: no --cc (test/host_tests.sh "
                  "runs them with the pinned toolchain)", file=sys.stderr)
    if bad:
        print("run_blob_staging_tests: %d case(s) FAILED" % bad,
              file=sys.stderr)
        return 1
    print("run_blob_staging_tests: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
