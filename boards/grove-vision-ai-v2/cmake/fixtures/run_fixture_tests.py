#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 ThreadX Shell Project
"""Negative tests for cmake/check_timer_seam.py (issue #30).

WHY REAL LINKS AND NOT UNIT TESTS
---------------------------------
The seam's whole claim is about what a LINKER does: that -Wl,--wrap severs every
reference from the prebuilt camera archives to the vendor timer API, so nothing
named hx_drv_timer_* except the permitted hx_drv_timer_init reaches the image and
the placement gate's blanket ban keeps passing untouched.  A unit test on the
checker's parsing would not test that claim at all -- it would test the parser.

So each fixture is a REAL link of the REAL objects and the REAL archives, with
exactly one thing broken:

  F1  one --wrap flag removed          -> S1 (a vendor symbol survives)
                                       -> S5 (and a branch reaches it)
  F2  the camera archives left out     -> S4 (the link is vacuous)
                                       -> S3 (no wrapper is referenced)
  F3  the unmodified probe             -> PASS

F3 is not decoration: without it, F1 and F2 could both be failing for some
unrelated reason that also happens to break the pristine link, and the suite
would still look green.

Assertions are on the EXIT CODE and the DIAGNOSTIC IDs, never on "non-zero".  A
fixture that starts failing for a different reason than it was built for is a
test failure, not a pass.

The link command is read out of the real build (Ninja) rather than reconstructed
here: a hand-written copy would drift, and then these fixtures would be testing
a link the project does not perform.  Nothing is written outside the build
directory; everything lands in <build>/seam-fixtures/.

Usage:
    python3 run_fixture_tests.py --build-dir build/grove-vision-ai-v2 \\
                                 --board-dir boards/grove-vision-ai-v2
"""

import argparse
import os
import re
import subprocess
import sys


def ninja_link_command(build_dir, target):
    """The g++ link line Ninja uses for `target`, verbatim."""
    out = subprocess.run(["ninja", "-C", build_dir, "-t", "commands", target],
                         check=True, capture_output=True, text=True).stdout
    for line in reversed(out.splitlines()):
        if "arm-none-eabi-g++" in line and f"-o {target}" in line:
            return line.strip()
    raise SystemExit(f"run_fixture_tests: no link command for {target} in "
                     f"{build_dir} -- build it first")


def retarget(cmd, old, new):
    """Point a link command at a different output (ELF and map)."""
    stem_old = os.path.splitext(old)[0]
    stem_new = os.path.splitext(new)[0]
    return cmd.replace(f"-o {old}", f"-o {new}") \
              .replace(f"-Wl,-Map={stem_old}.map", f"-Wl,-Map={stem_new}.map")


def link(build_dir, cmd, what):
    r = subprocess.run(cmd, shell=True, cwd=build_dir,
                       capture_output=True, text=True)
    if r.returncode != 0:
        print(f"run_fixture_tests: fixture {what} failed to LINK -- the fixture "
              f"is broken, not the gate:\n{r.stderr[-2000:]}", file=sys.stderr)
        raise SystemExit(1)


def run_gate(board_dir, build_dir, elf, nm, objdump):
    r = subprocess.run(
        [sys.executable, os.path.join(board_dir, "cmake", "check_timer_seam.py"),
         "--nm", nm, "--objdump", objdump, "--require-archives",
         os.path.join(build_dir, elf)],
        capture_output=True, text=True)
    ids = set(re.findall(r"\[([A-Z]\d)\]", r.stdout + r.stderr))
    return r.returncode, ids, (r.stdout + r.stderr)


def expect(name, got_rc, got_ids, want_rc, want_ids, text):
    ok = (got_rc == want_rc) and (got_ids == want_ids)
    print(f"  {'ok  ' if ok else 'FAIL'} {name}: rc={got_rc} ids={sorted(got_ids) or '-'}")
    if not ok:
        print(f"       wanted rc={want_rc} ids={sorted(want_ids) or '-'}",
              file=sys.stderr)
        print("       gate said:\n" + "\n".join("         " + l
                                                for l in text.splitlines()),
              file=sys.stderr)
    return ok


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--build-dir", required=True)
    ap.add_argument("--board-dir", required=True)
    ap.add_argument("--nm", default=None)
    ap.add_argument("--objdump", default=None)
    args = ap.parse_args()

    build = os.path.abspath(args.build_dir)
    board = os.path.abspath(args.board_dir)
    repo = os.path.abspath(os.path.join(board, "..", ".."))
    tc = os.path.join(repo, "tools",
                      "arm-gnu-toolchain-15.2.rel1-x86_64-arm-none-eabi", "bin")
    nm = args.nm or os.path.join(tc, "arm-none-eabi-nm")
    objdump = args.objdump or os.path.join(tc, "arm-none-eabi-objdump")

    base = ninja_link_command(build, "seam_probe.elf")
    outdir = os.path.join(build, "seam-fixtures")
    os.makedirs(outdir, exist_ok=True)

    print("run_fixture_tests (check_timer_seam.py):")
    ok = True

    # F3 first: the pristine probe must PASS, or the two fixtures below prove
    # nothing (they would be failing for a reason unrelated to what they break).
    rc, ids, text = run_gate(board, build, "seam_probe.elf", nm, objdump)
    ok &= expect("F3 pristine probe passes", rc, ids, 0, set(), text)

    # F1: one --wrap flag removed.  The vendor hw_stop is pulled straight out of
    # libdriver.a again, so both the symbol check and the branch scan must fire.
    f1 = base.replace("-Wl,--wrap=hx_drv_timer_hw_stop ", "")
    if f1 == base:
        raise SystemExit("run_fixture_tests: --wrap=hx_drv_timer_hw_stop is not "
                         "in the probe link; the fixture cannot break it")
    f1 = retarget(f1, "seam_probe.elf", "seam-fixtures/f1_unwrapped.elf")
    link(build, f1, "F1")
    rc, ids, text = run_gate(board, build, "seam-fixtures/f1_unwrapped.elf",
                             nm, objdump)
    ok &= expect("F1 missing --wrap is caught", rc, ids, 1, {"S1", "S5"}, text)

    # F2: the camera archives left out (and with them the forced references that
    # would otherwise be unresolvable).  Every other check passes vacuously on
    # such a link, which is exactly why S4 exists.
    f2 = re.sub(r"\S*lib(?:sensordp|extdevice)\.a\s+", "", base)
    f2 = re.sub(r"-Wl,-u,(?:sensordplib|hx_drv_cis)\S*\s+", "", f2)
    if f2 == base:
        raise SystemExit("run_fixture_tests: the probe link names no camera "
                         "archives; the fixture cannot remove them")
    f2 = retarget(f2, "seam_probe.elf", "seam-fixtures/f2_no_archives.elf")
    link(build, f2, "F2")
    rc, ids, text = run_gate(board, build, "seam-fixtures/f2_no_archives.elf",
                             nm, objdump)
    ok &= expect("F2 vacuous link is caught", rc, ids, 1, {"S3", "S4"}, text)

    if not ok:
        print("run_fixture_tests: FAIL", file=sys.stderr)
        return 1
    print("run_fixture_tests: OK (3 fixtures)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
