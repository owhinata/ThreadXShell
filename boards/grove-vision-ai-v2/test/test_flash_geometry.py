#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 ThreadX Shell Project
"""Host tests for cmake/flash_geometry.cmake (issue #85).

WHY THIS RUNS A REAL `cmake`

The four numbers in that file describe the part and the resident bootloader.
They were CACHE STRING entries when the firmware reservation first shrank to
2 MB, and that was a fail-open of a specific shape: the SAME values declare the
layout AND configure the check over it, so one -D moved the rule and its
verification together and the layout check still said OK.  Measured, before the
fix:

  -DGROVE_FW_SLOT_SIZE=0x200000  ->  firmware 0x0..0x400000, "max 1 artifact
                                     2097152 B", and a 1,704,672 B model is
                                     accepted as firmware -- which the
                                     bootloader refuses with ERR_IMAGE_SZ
  -DGROVE_FW_SLOTS=1             ->  blob starts at 0x100000, so blob owns the
                                     inactive firmware slot
  -DGROVE_ERASE_GRAN=0x1000      ->  slot-header shrinks to 0xfff000:0x1000 and
                                     blob-tail grows to 0xfff000, swallowing the
                                     BACKUP slot header at 0xFFE000
  -DGROVE_FLASH_SIZE=0x2000000   ->  slot-header moves to 0x1ff0000, off the end
                                     of the real part, so BOTH real headers at
                                     0xFFE000/0xFFF000 end up inside blob-tail

The fix is that a disagreeing cache entry is a hard error.  The refusal IS the
enforcement, so it is tested by driving the real file through a real configure
rather than by reading it -- a rule nobody has watched fail is worth as little
as no rule (issue #66).

The harness configures a `project(NONE)` that does nothing but include the file,
so there is no toolchain, no SDK and no compiler check: each case is a fraction
of a second.

[!] An entry that AGREES must still be accepted.  A build directory configured
before this file existed carries exactly these values in its cache, and making
those trees refuse to configure would buy nothing and cost every existing tree.
"""

import os
import re
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
GEOMETRY = os.path.normpath(os.path.join(HERE, "..", "cmake",
                                         "flash_geometry.cmake"))

# What the file must produce when nobody argues with it.
EXPECT = {
    "GROVE_FLASH_SIZE":        0x1000000,
    "GROVE_ERASE_GRAN":        0x10000,
    "GROVE_FW_SLOT_SIZE":      0x100000,
    "GROVE_FW_SLOTS":          2,
    "GROVE_FW_RESERVED":       0x200000,
    "GROVE_SLOT_HDR_ADDR":     0xFF0000,
    "GROVE_SLOT_HDR_RESERVED": 0x10000,
}

fails = 0


def configure(tmp, defines=()):
    """Configure a do-nothing project that only includes the file under test."""
    src = os.path.join(tmp, "src")
    bld = os.path.join(tmp, "build")
    os.makedirs(src, exist_ok=True)
    shutil.rmtree(bld, ignore_errors=True)
    report = os.path.join(tmp, "geometry.txt")
    if os.path.exists(report):
        os.remove(report)
    with open(os.path.join(src, "CMakeLists.txt"), "w") as f:
        f.write("cmake_minimum_required(VERSION 3.20)\n"
                "project(geometry_probe NONE)\n"
                f'include("{GEOMETRY}")\n')
    argv = ["cmake", "-S", src, "-B", bld,
            f"-DGROVE_FLASH_GEOMETRY_REPORT={report}"]
    argv += [f"-D{d}" for d in defines]
    r = subprocess.run(argv, capture_output=True, text=True)
    values = {}
    if os.path.exists(report):
        for line in open(report):
            k, _, v = line.strip().partition("=")
            if k:
                values[k] = v
    return r, values


def expect_ok(tmp, name, defines=()):
    global fails
    r, values = configure(tmp, defines)
    if r.returncode != 0:
        print(f"  FAIL {name}: configure refused\n{r.stdout}{r.stderr}")
        fails += 1
        return None
    print(f"  ok   {name}")
    return values


def expect_refused(tmp, name, defines, must_mention):
    global fails
    r, _ = configure(tmp, defines)
    # [!] Collapse whitespace before matching.  CMake reflows message() text, so
    # "... and 'lots' is\n  not even a number" is one phrase to a reader and two
    # lines to `in`.  Matching the raw output makes the test depend on the
    # formatter rather than on what the message says.
    out = re.sub(r"\s+", " ", r.stdout + r.stderr)
    if r.returncode == 0:
        print(f"  FAIL {name}: configure ACCEPTED -- still fail-open")
        fails += 1
        return
    if must_mention not in out:
        print(f"  FAIL {name}: refused, but without mentioning "
              f"{must_mention!r}\n{out}")
        fails += 1
        return
    print(f"  ok   {name}")


def main():
    global fails

    if shutil.which("cmake") is None:
        print("test_flash_geometry: no cmake on PATH -- SKIP")
        return 0

    with tempfile.TemporaryDirectory() as tmp:
        # --- the values the board actually ships -------------------------
        values = expect_ok(tmp, "a clean configure produces the shipped geometry")
        if values is not None:
            # [!] NOT a for/else.  `else` on a loop runs whenever the loop is
            # not broken out of, so with no `break` it reports "ok" even after
            # every value mismatched -- a fail-open in the test itself, which is
            # the exact shape of the finding this file exists to pin.
            mismatched = 0
            for key, want in EXPECT.items():
                got = values.get(key)
                if got is None:
                    print(f"  FAIL {key} is not reported at all")
                    mismatched += 1
                elif int(got, 0) != want:
                    print(f"  FAIL {key} is {got}, wanted 0x{want:X}")
                    mismatched += 1
            fails += mismatched
            if not mismatched:
                print(f"  ok   all {len(EXPECT)} geometry values are as shipped")

            # The derivations, restated here so a change to either the formula
            # or a constant has to be made in two places to go unnoticed.
            slot = int(values["GROVE_FW_SLOT_SIZE"], 0)
            slots = int(values["GROVE_FW_SLOTS"], 0)
            gran = int(values["GROVE_ERASE_GRAN"], 0)
            size = int(values["GROVE_FLASH_SIZE"], 0)
            if int(values["GROVE_FW_RESERVED"], 0) != slot * slots:
                print("  FAIL GROVE_FW_RESERVED is not slot size * slot count")
                fails += 1
            elif int(values["GROVE_SLOT_HDR_ADDR"], 0) != size - gran:
                print("  FAIL GROVE_SLOT_HDR_ADDR is not flash size - erase block")
                fails += 1
            else:
                print("  ok   the derived values follow from the measurements")

            # [!] The reservation must cover BOTH copies of the slot header:
            # flash_end - 0x1000 and flash_end - 0x2000.  This is the property
            # -DGROVE_ERASE_GRAN=0x1000 destroyed, and it is worth stating as a
            # property rather than trusting that 64 KB happens to be enough.
            hdr = int(values["GROVE_SLOT_HDR_ADDR"], 0)
            if hdr > size - 0x2000:
                print(f"  FAIL the slot-header reservation starts at 0x{hdr:X}, "
                      f"above the backup copy at 0x{size - 0x2000:X}")
                fails += 1
            else:
                print("  ok   the reservation covers both slot-header copies")

        # --- a cache entry that AGREES is accepted -----------------------
        # Every build directory configured before this file existed has these.
        expect_ok(tmp, "an inherited cache entry with the same value is accepted",
                  ["GROVE_ERASE_GRAN=0x10000", "GROVE_FW_SLOTS=2"])
        expect_ok(tmp, "the same value written differently is accepted",
                  ["GROVE_ERASE_GRAN=65536"])

        # --- a cache entry that DISAGREES is refused ---------------------
        # One case per exploit measured before the fix; see the module header.
        for name, define in (
            ("an oversized firmware slot is refused",
             "GROVE_FW_SLOT_SIZE=0x200000"),
            ("dropping to one firmware slot is refused",
             "GROVE_FW_SLOTS=1"),
            ("a smaller erase granularity is refused",
             "GROVE_ERASE_GRAN=0x1000"),
            ("a larger flash than the part is refused",
             "GROVE_FLASH_SIZE=0x2000000"),
        ):
            expect_refused(tmp, name, [define],
                           define.split("=")[0] + " is fixed board geometry")

        # Not a number at all: refused with the same reasoning, not a CMake
        # arithmetic error somebody has to decode.
        expect_refused(tmp, "a non-numeric override is refused",
                       ["GROVE_FW_SLOTS=lots"], "is not even a number")

    if fails:
        print(f"test_flash_geometry: {fails} failure(s)")
        return 1
    print("test_flash_geometry: all cases pass")
    return 0


if __name__ == "__main__":
    sys.exit(main())
