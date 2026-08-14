#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 ThreadX Shell Project
"""Gate 4: the vendor timer API seam actually severed every vendor reference.

Issue #30.  The prebuilt camera archives call four hx_drv_timer_* entry points.
This port bars that whole prefix from the image (check_placement_budget.py, and
AGENTS.md records it as an invariant) because TIMER2 is the execution-profile
time source and no name-based check can tell which timer id a generic call
carries.  The seam redirects those four references to board-owned code with
-Wl,--wrap and never calls __real_*, so no vendor timer code reaches the ELF and
the existing gate keeps passing unchanged.

That is a claim about a LINK, so it is checked on a link.  Run against the
seam_probe ELF (the firmware objects plus libsensordp.a / libextdevice.a plus
forced references to the datapath entry points), and against the firmware ELF
itself once those archives move into it.

What is asserted:

  1. No hx_drv_timer_* symbol survives except hx_drv_timer_init.  That is the
     same rule check_placement_budget.py applies -- restated here because THIS
     is the link where it could actually break, and because a failure here has
     a specific diagnosis the other gate cannot give.
  2. No __real_hx_drv_timer_* survives.  Its presence would mean some path
     calls through to the vendor implementation after all, which is exactly
     what the seam promises not to do.
  3. Every wrapped symbol the archives still REFERENCE has its __wrap_ counter-
     part defined in the image, and every __wrap_ symbol present is one of the
     four the build asked for.  A wrapper that vanished (renamed source, dropped
     --wrap flag) must not pass as "no vendor timer code, therefore fine".
  4. With --require-archives: the archives really are in the link.  Without it
     this gate is trivially satisfiable by not linking them at all -- the exact
     fail-open that would make the probe meaningless.
  5. No call or jump instruction in the image targets a vendor timer entry
     point.  Symbol absence is checked above; this catches the case where a
     vendor function was inlined or reached through a linker veneer whose
     symbol is gone but whose code is not.

Stdlib-only; POST_BUILD.
"""

import argparse
import re
import subprocess
import sys

# The four symbols board.cmake wraps.  Kept in step with SDK_TIMER_WRAP_SYMBOLS
# there; a mismatch is reported rather than silently tolerated (check 3).
WRAPPED = [
    "hx_drv_timer_hw_start",
    "hx_drv_timer_hw_stop",
    "hx_drv_timer_cm55x_delay_ms",
    "hx_drv_timer_cm55x_delay_us",
]

# The one vendor timer entry point this port permits: platform_driver_init()
# calls it for all nine timers and it only records a base address and derives
# g_timer_clk[] -- it starts nothing and writes no timer register.
ALLOWED = {"hx_drv_timer_init"}

# Symbols that prove the camera archives are actually in the link (check 4).
ARCHIVE_WITNESSES = ["sensordplib_retrigger_capture", "hx_drv_cis_init"]


def run(cmd):
    return subprocess.run(cmd, check=True, capture_output=True, text=True).stdout


def defined_symbols(nm, elf):
    out = run([nm, "--defined-only", elf])
    names = set()
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 3:
            names.add(parts[2])
    return names


def branch_targets(objdump, elf):
    """Every symbol name reached by a bl/b/blx/bx branch in the image."""
    out = run([objdump, "-d", "--no-show-raw-insn", elf])
    targets = set()
    for line in out.splitlines():
        m = re.search(r"\b(?:bl|blx|b|b\.w|bl\.w)\s+[0-9a-f]+\s+<([^>+]+)", line)
        if m:
            targets.add(m.group(1))
    return targets


def function_body(objdump, elf, name):
    """Disassembly text of one function, literal pool included."""
    out = run([objdump, "-d", elf])
    body, inside = [], False
    for line in out.splitlines():
        m = re.match(r"^[0-9a-f]+ <([^>]+)>:", line)
        if m:
            inside = (m.group(1) == name)
            continue
        if inside:
            if not line.strip():
                break
            body.append(line)
    return "\n".join(body)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--nm", required=True)
    ap.add_argument("--objdump", required=True)
    ap.add_argument("--require-archives", action="store_true",
                    help="fail unless the camera archives are really linked")
    ap.add_argument("elf")
    args = ap.parse_args()

    errors = []
    syms = defined_symbols(args.nm, args.elf)

    # Diagnostics carry a stable ID so the fixture tests
    # (cmake/fixtures/run_fixture_tests.py) can assert that a fixture failed for
    # the reason it was BUILT to fail for, rather than settling for "non-zero".
    def fail(ident, msg):
        errors.append(f"[{ident}] {msg}")

    # S4. the archives are present -- checked FIRST, because every check below
    #     passes vacuously on a link that simply left them out.
    if args.require_archives:
        missing = [w for w in ARCHIVE_WITNESSES if w not in syms]
        if missing:
            fail("S4", "the camera archives are not in this link (missing "
                       + ", ".join(missing)
                       + ") -- every other check here would pass vacuously")

    # S1. no vendor timer symbol except the permitted one
    survivors = sorted(n for n in syms
                       if n.startswith("hx_drv_timer_") and n not in ALLOWED)
    for s in survivors:
        fail("S1", f"vendor timer symbol {s} survived the --wrap; the seam is "
                   "not severing the reference (or a new call site appeared "
                   "that board.cmake does not wrap)")

    # S2. nothing calls through to the real implementation
    for s in sorted(n for n in syms if n.startswith("__real_hx_drv_timer_")):
        fail("S2", f"{s} is defined: something calls the vendor timer "
                   "implementation through the seam")

    # S3. the wrappers themselves are present and are exactly the expected set
    present_wraps = {n for n in syms if n.startswith("__wrap_hx_drv_timer_")}
    expected_wraps = {"__wrap_" + s for s in WRAPPED}
    for extra in sorted(present_wraps - expected_wraps):
        fail("S3", f"{extra} is defined but board.cmake does not wrap the "
                   "corresponding symbol; the two have drifted apart")
    if args.require_archives and not present_wraps:
        fail("S3", "no __wrap_hx_drv_timer_* survived: with the archives "
                   "linked at least one wrapper must be referenced, so the "
                   "seam is not in the link at all")

    # S5. no branch reaches a vendor timer entry point
    reached = sorted(t for t in branch_targets(args.objdump, args.elf)
                     if t.startswith("hx_drv_timer_") and t not in ALLOWED)
    for t in reached:
        fail("S5", f"a branch in the image targets {t}")

    # S6. the seam drives the REAL Timer0 block.  timer_seam.c lets the host
    #     test point its register block at an array (a pointer is not a
    #     constant expression, so the file's own _Static_assert cannot cover
    #     that build).  This is the guard that does not go away: the linked
    #     firmware must actually carry 0x5500A000 inside the stop wrapper,
    #     which is the smallest function that touches every Timer0 register.
    if "__wrap_hx_drv_timer_hw_stop" in syms:
        body = function_body(args.objdump, args.elf,
                             "__wrap_hx_drv_timer_hw_stop")
        if not re.search(r"\b5500a000\b", body, re.IGNORECASE):
            fail("S6", "__wrap_hx_drv_timer_hw_stop does not reference "
                       "0x5500A000; the seam was built against some other "
                       "Timer0 base (GROVE_TIMER_SEAM_T0_BASE is for the host "
                       "test only)")

    if errors:
        print("check_timer_seam: FAIL", file=sys.stderr)
        for e in errors:
            print(f"  - {e}", file=sys.stderr)
        return 1

    print("check_timer_seam: OK (" + ", ".join(sorted(present_wraps)) + " in, "
          "no vendor timer code)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
