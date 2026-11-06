#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 ThreadX Shell Project
"""Tests for the shared slot-table tool, asset/tools/slot_table.c (issue #195).

The tool is compiled the way each board's build rule compiles it -- the
board's table, svc/blob/blob_map.c, and the header the firmware's store reads
its interval and erase unit from, forced in with -include -- and run:

  - a synthetic table inside Grove's interval comes out as EXACTLY the JSON
    written below (the numbers by hand: payload_max is the slot less its 4 KB
    header unit);
  - each board's SHIPPED table passes and comes out as JSON in the schema
    cmake/build_asset.py reads, without pinning its contents (it grows by
    appending): one row per entry of the table's source, index 0.., each base
    the source's and inside the interval on a unit boundary, payload_max the
    source's size less one unit and short of the next slot's base, and no
    temporary file left beside it;
  - a table with a slot outside the interval, two slots that overlap, or a slot
    that is not whole erase units is REFUSED in a sentence naming the store's
    verdict: no JSON, no temporary file, and a JSON an earlier run left at that
    path is removed (a stale table would be read as current);
  - the interval is the BOARD'S: a table inside wio-lite-ai's / f746g-disco's
    interval but below Grove's is refused when built as Grove and passes as
    wio, and one inside Grove's but above wio's the other way round.

Grove's interval is written out as -D, as in boards/grove-vision-ai-v2/test/
host_tests.sh (board.cmake derives it from cmake/flash_geometry.cmake); the
build itself compiles the tool from board.cmake's variables.  Host cc only.

[!] EVERY REFUSAL IS ASSERTED ON ITS OWN SENTENCE, and each control passes, so
a case that fails for another reason (a compile error, say) is a failure.
"""
import json
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
TOOL = os.path.join(REPO, "asset", "tools", "slot_table.c")
SVC = os.path.join(REPO, "svc")
BOARDS = os.path.join(REPO, "boards")

# What each board's rule passes (board.cmake), apart from its table.
WIO = ["-I", os.path.join(BOARDS, "wio-lite-ai", "src"),
       "-I", os.path.join(BOARDS, "wio-lite-ai", "port", "nor"),
       "-include", "blob.h", "-DSLOT_TABLE_LO=BLOB_REGION_BASE",
       "-DSLOT_TABLE_HI=BLOB_REGION_END", "-DSLOT_TABLE_UNIT=BLOB_ERASE_UNIT"]
F746 = ["-I", os.path.join(BOARDS, "f746g-disco", "src"),
        "-I", os.path.join(BOARDS, "f746g-disco", "port", "qspi"),
        "-include", "blob.h", "-DSLOT_TABLE_LO=BLOB_REGION_BASE",
        "-DSLOT_TABLE_HI=BLOB_REGION_END", "-DSLOT_TABLE_UNIT=BLOB_ERASE_UNIT"]
GROVE = ["-DNOR_PART_FW_END=0x200000", "-DNOR_PART_BLOB_END=0xffe000",
         "-DNOR_ERASE_GRAN=0x1000",
         "-I", os.path.join(BOARDS, "grove-vision-ai-v2", "src"),
         "-I", os.path.join(BOARDS, "grove-vision-ai-v2", "port", "sdk_seam"),
         "-I", os.path.join(BOARDS, "grove-vision-ai-v2", "port", "nor"),
         "-include", "nor_seam.h", "-DSLOT_TABLE_LO=NOR_WRITABLE_LO",
         "-DSLOT_TABLE_HI=NOR_WRITABLE_HI", "-DSLOT_TABLE_UNIT=NOR_ERASE_UNIT"]
BOARD_CFG = {"wio-lite-ai": WIO, "f746g-disco": F746,
             "grove-vision-ai-v2": GROVE}

TABLE_C = """#include "blob_map.h"
const struct blob_slot blob_map_slots[] = { %s };
const unsigned blob_map_slots_count =
    (unsigned)(sizeof blob_map_slots / sizeof blob_map_slots[0]);
const struct blob_slot *blob_map_table(void) { return blob_map_slots; }
unsigned blob_map_count(void) { return blob_map_slots_count; }
"""

# Prints the table straight from the array src/blob_slots.c defines -- not
# through blob_map.c or the tool -- so the JSON is compared with the source.
LISTER_C = """#include <stdio.h>
#include "blob_map.h"
int main(void)
{
    unsigned i;
    printf("%u %u %u\\n", blob_map_slots_count, (unsigned)(SLOT_TABLE_LO),
           (unsigned)(SLOT_TABLE_HI));
    for (i = 0u; i < blob_map_slots_count; i++)
        printf("%u %u\\n", (unsigned)blob_map_slots[i].base,
               (unsigned)blob_map_slots[i].size);
    return 0;
}
"""

# A table that passes, and the JSON it must produce, written out by hand.
EXACT_ROWS = ("{0x00200000u, 0x00100000u}, {0x00300000u, 0x00040000u}, "
              "{0x00400000u, 0x00002000u}")
EXACT_JSON = """{
  "unit": 4096,
  "slots": [
    {"index": 0, "base": 2097152, "payload_max": 1044480},
    {"index": 1, "base": 3145728, "payload_max": 258048},
    {"index": 2, "base": 4194304, "payload_max": 4096}
  ]
}
"""

bad = 0


def judge(name, ok, detail=""):
    global bad
    print("  %s %-34s %s" % ("ok  " if ok else "FAIL", name,
                             detail if not ok else ""))
    if not ok:
        bad += 1


def build(work, name, cfg, table_c):
    exe = os.path.join(work, name)
    cmd = (["cc", "-std=c11", "-O1", "-Wall", "-Wextra", "-Werror",
            "-I", SVC, "-I", os.path.join(SVC, "blob")] + cfg +
           [os.path.join(SVC, "blob", "blob_map.c"), table_c, TOOL,
            "-o", exe])
    r = subprocess.run(cmd, capture_output=True, text=True)
    return exe if r.returncode == 0 else None, r.stderr


def run(exe, out):
    r = subprocess.run([exe, out], capture_output=True, text=True)
    return r.returncode, r.stdout + r.stderr


def source_table(work, name, cfg, table_c):
    """(count, lo, hi, [(base, size)]) read from the table's own source."""
    src = os.path.join(work, name + "_lister.c")
    with open(src, "w") as fh:
        fh.write(LISTER_C)
    exe = os.path.join(work, name + "_lister")
    r = subprocess.run(["cc", "-std=c11", "-I", SVC, "-I",
                        os.path.join(SVC, "blob")] + cfg +
                       [table_c, src, "-o", exe], capture_output=True,
                       text=True)
    if r.returncode != 0:
        return None
    lines = subprocess.run([exe], capture_output=True,
                           text=True).stdout.split("\n")
    count, lo, hi = (int(x) for x in lines[0].split())
    rows = [tuple(int(x) for x in ln.split()) for ln in lines[1:] if ln]
    return count, lo, hi, rows


def shipped(work):
    for board, cfg in sorted(BOARD_CFG.items()):
        name = "shipped_" + board.replace("-", "_")
        table_c = os.path.join(BOARDS, board, "src", "blob_slots.c")
        exe, err = build(work, name, cfg, table_c)
        src = source_table(work, name, cfg, table_c)
        if exe is None or src is None:
            judge(name, False, "did not compile: " + err[:300])
            continue
        count, lo, hi, rows = src
        out = os.path.join(work, name + ".json")
        rc, msg = run(exe, out)
        ok, why = rc == 0 and os.path.exists(out), msg
        if ok:
            t = json.load(open(out))
            slots, unit = t["slots"], t["unit"]
            nxt = [s["base"] for s in slots[1:]] + [hi]
            ok = (unit == 4096 and len(slots) == count == len(rows) and
                  [s["index"] for s in slots] == list(range(count)) and
                  all(s["base"] == r[0] and lo <= s["base"] < hi and
                      s["base"] % unit == 0 and
                      s["payload_max"] == r[1] - unit and
                      0 < s["payload_max"] < n - s["base"]
                      for s, r, n in zip(slots, rows, nxt)) and
                  not os.path.exists(out + ".tmp"))
            why = "source %r\njson %s" % (src, json.dumps(t)[:400])
        judge(name, ok, why)


def exact(work):
    name = "exact_json"
    src = os.path.join(work, name + "_table.c")
    with open(src, "w") as fh:
        fh.write(TABLE_C % EXACT_ROWS)
    exe, err = build(work, name, GROVE, src)
    if exe is None:
        judge(name, False, "did not compile: " + err[:300])
        return
    out = os.path.join(work, name + ".json")
    rc, msg = run(exe, out)
    got = open(out).read() if os.path.exists(out) else None
    judge(name, rc == 0 and got == EXACT_JSON,
          "rc=%d\ngot:\n%s\nwant:\n%s%s" % (rc, got, EXACT_JSON, msg))


def refused(work, name, cfg, rows, want):
    src = os.path.join(work, name + "_table.c")
    with open(src, "w") as fh:
        fh.write(TABLE_C % rows)
    exe, err = build(work, name, cfg, src)
    if exe is None:
        judge(name, False, "did not compile: " + err[:300])
        return
    out = os.path.join(work, name + ".json")
    with open(out, "w") as fh:              # a stale table from an earlier run
        fh.write('{"unit": 4096, "slots": []}\n')
    rc, msg = run(exe, out)
    judge(name, rc != 0 and want in msg and "no JSON written" in msg and
          not os.path.exists(out) and not os.path.exists(out + ".tmp"),
          "rc=%d json=%s\n%s" % (rc, os.path.exists(out), msg))


def passes(work, name, cfg, rows):
    src = os.path.join(work, name + "_table.c")
    with open(src, "w") as fh:
        fh.write(TABLE_C % rows)
    exe, err = build(work, name, cfg, src)
    if exe is None:
        judge(name, False, "did not compile: " + err[:300])
        return
    out = os.path.join(work, name + ".json")
    rc, msg = run(exe, out)
    judge(name, rc == 0 and os.path.exists(out) and
          not os.path.exists(out + ".tmp"), "rc=%d\n%s" % (rc, msg))


def main():
    print("run_slot_table_tests:")
    with tempfile.TemporaryDirectory() as work:
        shipped(work)
        exact(work)
        # Shapes the store refuses, against Grove's interval [0x200000,
        # 0xffe000) in 4 KB units.
        refused(work, "grove_outside", GROVE,
                "{0x00F00000u, 0x00100000u}",
                "slot is outside the writable interval (slot 0)")
        refused(work, "grove_overlap", GROVE,
                "{0x00200000u, 0x00200000u}, {0x00300000u, 0x00100000u}",
                "slots overlap")
        refused(work, "grove_unaligned", GROVE,
                "{0x00200800u, 0x00100000u}",
                "slot is not whole erase units (slot 0)")
        refused(work, "wio_outside", WIO,
                "{0x00700000u, 0x00200000u}",
                "slot is outside the writable interval (slot 0)")
        # The interval is the board's: the same table, two answers.
        low = "{0x00100000u, 0x00100000u}"     # wio/f746 yes, Grove no
        high = "{0x00900000u, 0x00100000u}"    # Grove yes, wio/f746 no
        passes(work, "low_table_as_wio", WIO, low)
        refused(work, "low_table_as_grove", GROVE, low,
                "slot is outside the writable interval (slot 0)")
        passes(work, "high_table_as_grove", GROVE, high)
        refused(work, "high_table_as_f746", F746, high,
                "slot is outside the writable interval (slot 0)")
    if bad:
        print("run_slot_table_tests: %d case(s) FAILED" % bad, file=sys.stderr)
        return 1
    print("run_slot_table_tests: all cases behave as specified")
    return 0


if __name__ == "__main__":
    sys.exit(main())
