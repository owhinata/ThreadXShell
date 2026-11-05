#!/usr/bin/env python3
"""Tests for cmake/check_blob_staging.py (issue #193).

Two halves:

  1. THE TWO NUMBERS THE GATE RESTATES ARE THE FIRMWARE'S.  The gate subtracts
     HEADER_BYTES from a slot's size and reads the table in ENTRY_BYTES strides.
     Both are facts of the C side (src/blob.h's BLOB_ERASE_UNIT and
     sizeof(struct blob_slot), base first), so the host compiler is asked for
     them through the real headers and each is compared with the gate's.  Change one
     side alone and this fails -- which is what keeps a smaller header from
     quietly loosening the gate.  Needs only the host gcc, so it always runs.

  2. SYNTHETIC IMAGES, one rule broken in each (needs --cc, the pinned
     arm-none-eabi-gcc).  The control PASSES; every refusal is asserted on its
     own diagnostic, so a case that starts failing for another reason is a test
     failure rather than a pass.  The boundary is walked on both sides: a slot
     whose payload is exactly the staging capacity passes, one unit more fails.
"""

import argparse
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
CMAKE_DIR = os.path.dirname(HERE)
BOARD = os.path.dirname(CMAKE_DIR)
REPO = os.path.dirname(os.path.dirname(BOARD))
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


def const_cases(work, host_cc):
    c = os.path.join(work, "consts.c")
    exe = os.path.join(work, "consts")
    with open(c, "w") as fh:
        fh.write(CONSTS_SRC)
    r = subprocess.run([host_cc, "-std=c11",
                        "-I", os.path.join(BOARD, "src"),
                        "-I", os.path.join(BOARD, "port", "nor"),
                        "-I", os.path.join(REPO, "svc"),
                        "-I", os.path.join(REPO, "svc", "blob"),
                        c, "-o", exe], capture_output=True, text=True)
    if r.returncode != 0:
        judge("consts_compile", False, r.stderr[:300])
        return
    unit, entry, off_base, off_size = (int(x) for x in subprocess.run(
        [exe], capture_output=True, text=True, check=True).stdout.split())
    judge("header_is_erase_unit", unit == check_blob_staging.HEADER_BYTES,
          "src/blob.h BLOB_ERASE_UNIT is %d, the gate's HEADER_BYTES %d"
          % (unit, check_blob_staging.HEADER_BYTES))
    judge("entry_is_struct_size", entry == check_blob_staging.ENTRY_BYTES,
          "sizeof(struct blob_slot) is %d, the gate's ENTRY_BYTES %d"
          % (entry, check_blob_staging.ENTRY_BYTES))
    # The gate unpacks each entry as <base, size>; a swapped struct would be
    # the same size and read every base as a size.
    judge("entry_is_base_then_size", (off_base, off_size) == (0, 4),
          "struct blob_slot has base at %d and size at %d, the gate reads "
          "them at 0 and 4" % (off_base, off_size))


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

# (name, backend, source, want rc, substring the output must carry)
CASES = [
    ("control_tflm", "tflm", table(STD) + staging(), 0, "OK -- 4 slots"),
    ("edge_exact_fit", "tflm", table([MB + HDR]) + staging(), 0,
     "0 B to spare"),
    ("slot_too_big", "tflm", table([MB + 2 * HDR]) + staging(), 1,
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
]


def image_cases(cc, work):
    for name, backend, src, want, needle in CASES:
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
        r = subprocess.run([sys.executable, GATE, elf, "--backend", backend],
                           capture_output=True, text=True)
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
        r = subprocess.run([sys.executable, GATE, elf, "--backend", "tflm"],
                           capture_output=True, text=True)
        judge("table_twice", r.returncode == 1 and
              "2 symbols named blob_map_slots;" in r.stderr, r.stderr)

    # Not an ELF, and an unknown backend: could not check (2), never a pass.
    junk = os.path.join(work, "junk.elf")
    with open(junk, "wb") as fh:
        fh.write(b"not an elf")
    r = subprocess.run([sys.executable, GATE, junk, "--backend", "tflm"],
                       capture_output=True, text=True)
    judge("not_an_elf", r.returncode == 2 and "CANNOT CHECK" in r.stderr,
          r.stderr)
    r = subprocess.run([sys.executable, GATE,
                        os.path.join(work, "control_tflm.elf"),
                        "--backend", "stedgeai"], capture_output=True,
                       text=True)
    judge("unknown_backend", r.returncode == 2 and "CANNOT CHECK" in r.stderr,
          r.stderr)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host-cc", default="gcc")
    ap.add_argument("--cc", help="arm-none-eabi-gcc, for the image cases")
    args = ap.parse_args()
    print("run_blob_staging_tests:")
    with tempfile.TemporaryDirectory() as work:
        const_cases(work, args.host_cc)
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
