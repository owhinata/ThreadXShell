#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 ThreadX Shell Project
"""Negative tests for the post-build gates that assert a LINKER property.

Covers cmake/check_timer_seam.py (issue #30) and the loadable-SRAM floor in
cmake/check_placement_budget.py (issue #29).

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

  P1  .lcd_fb made LOADABLE             -> the loader-window floor fires
  P0  the unmodified firmware           -> PASS

F3 and P0 are not decoration: without them, the fixtures above could all be
failing for some unrelated reason that also happens to break the pristine link,
and the suite would still look green.

P1 exists because the LINKER cannot enforce this rule.  The region split
expresses it, but ld places a section where it is told and has no notion of
"this region takes NOBITS only", and the ASSERTs there can only speak about the
one section the script happens to name.  With .lcd_fb loadable the link
succeeds and both ASSERTs hold -- only a gate reading the section flags back
off the ELF objects.

[!] Un-NOLOADing .lcd_fb also trips the older NOBITS-residency check, because
.lcd_fb is on that check's fixed list.  That is why the assertion below is on
the floor DIAGNOSTIC and not on a non-zero exit: otherwise deleting the floor
check would leave this fixture passing on the other check's back.  The case the
floor check alone catches is a NEW loadable section nobody has listed --
which is exactly the case that has no test until someone writes one, and the
reason the check reads flags rather than names.

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


def run_placement_gate(board_dir, build_dir, elf, nm, objdump):
    """check_placement_budget.py on one ELF.

    That gate numbers its checks in comments rather than in its output, so the
    assertion below matches a distinctive phrase instead of an [ID].  The intent
    is the same as everywhere else here: a fixture that starts failing for a
    different reason than it was built for must not read as a pass.
    """
    r = subprocess.run(
        [sys.executable,
         os.path.join(board_dir, "cmake", "check_placement_budget.py"),
         "--nm", nm, "--objdump", objdump, os.path.join(build_dir, elf)],
        capture_output=True, text=True)
    text = r.stdout + r.stderr
    ids = {"FLOOR"} if "loadable-SRAM floor" in text else set()
    return r.returncode, ids, text


def link_only(cmd):
    """Just the compiler invocation, without the POST_BUILD chain.

    Ninja reports the firmware link with the image generation and all four gates
    chained on with &&.  A fixture wants the link and nothing else -- running the
    real image generator on a deliberately broken ELF would be slow and would
    fail for its own reasons.  (The command also STARTS with `: &&`, which is
    why this picks the segment out rather than truncating at the first &&.)
    """
    for part in cmd.split(" && "):
        if "arm-none-eabi-g++" in part and "-o shell.elf" in part:
            return part.strip()
    raise SystemExit("run_fixture_tests: no compiler segment in the shell link")


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
    #
    # [!] Since issue #35 the firmware itself CALLS the archives, so removing
    # them is no longer a link that merely lacks vendor timer references -- it is
    # one that does not link at all.  That is a stronger guarantee than the gate
    # gives (you cannot build this firmware without the archives, and the
    # compiler says so), but it would leave S4 with no negative test, so the
    # fixture drops port/camera/ and the camera command along with the archives
    # and reaches the vacuous link the check is about.  main.c's single call to
    # camera_create_objects() is the only reference left after that, and a
    # --defsym is cheaper than carving up an object file: nothing in a fixture
    # ELF is ever executed.  It aliases another void(void) rather than an
    # address, because a bare --defsym value carries no Thumb bit and the call
    # site's relocation needs one.
    f2 = re.sub(r"\S*lib(?:sensordp|extdevice)\.a\s+", "", base)
    f2 = re.sub(r"-Wl,-u,(?:sensordplib|hx_drv_cis)\S*\s+", "", f2)
    # Every camera CONSUMER has to go with the archives, not just the camera
    # port itself: cmd_nn.c captures a frame to feed the NPU (issue #44), so
    # leaving it in turns this fixture into an undefined-reference failure
    # instead of the vacuous link it is supposed to produce.
    f2_objs = re.sub(r"\S*shell_objs\.dir/boards/\S*/(?:port/camera/\S+|"
                     r"cmds/cmd_camera\.c|cmds/cmd_nn\.c)\.obj\s+", "", f2)
    if f2_objs == f2:
        raise SystemExit("run_fixture_tests: the probe link names no "
                         "port/camera objects; the fixture cannot remove them")
    # Prepended to the COMPILER invocation, not appended to the command: the
    # line Ninja reports ends with the POST_BUILD gate run chained on with &&,
    # so anything added at the end becomes an argument to that instead.
    f2 = f2_objs.replace(
        "arm-none-eabi-g++ ",
        "arm-none-eabi-g++ "
        "-Wl,--defsym=camera_create_objects=lcd_create_objects ", 1)
    if f2 == base:
        raise SystemExit("run_fixture_tests: the probe link names no camera "
                         "archives; the fixture cannot remove them")
    f2 = retarget(f2, "seam_probe.elf", "seam-fixtures/f2_no_archives.elf")
    link(build, f2, "F2")
    rc, ids, text = run_gate(board, build, "seam-fixtures/f2_no_archives.elf",
                             nm, objdump)
    ok &= expect("F2 vacuous link is caught", rc, ids, 1, {"S3", "S4"}, text)

    # --- check_placement_budget.py: the loadable-SRAM floor (issue #29) -------
    print("run_fixture_tests (check_placement_budget.py, SRAM floor):")

    shell_link = link_only(ninja_link_command(build, "shell.elf"))

    rc, ids, text = run_placement_gate(board, build, "shell.elf", nm, objdump)
    ok &= expect("P0 pristine firmware passes", rc, ids, 0, set(), text)

    # P1: .lcd_fb loses its NOLOAD.  It then has CONTENTS at 0x3401F000, i.e.
    # the loader would write it over the code it is executing.  The link still
    # succeeds and every other gate still passes -- this one must not.
    ld_src = os.path.join(board, "ldscript", "HX6538_CM55M_S.ld")
    ld_dst = os.path.join(outdir, "p1_loadable_low.ld")
    with open(ld_src) as f:
        ld = f.read()
    ld_broken = ld.replace(".lcd_fb (NOLOAD) : ALIGN(32)", ".lcd_fb : ALIGN(32)")
    if ld_broken == ld:
        raise SystemExit("run_fixture_tests: .lcd_fb is not NOLOAD in the "
                         "linker script; the fixture cannot break it")
    with open(ld_dst, "w") as f:
        f.write(ld_broken)

    p1 = re.sub(r"-T\S*HX6538_CM55M_S\.ld", f"-T{ld_dst}", shell_link)
    if p1 == shell_link:
        raise SystemExit("run_fixture_tests: the firmware link names no linker "
                         "script; the fixture cannot repoint it")
    p1 = retarget(p1, "shell.elf", "seam-fixtures/p1_loadable_low.elf")
    link(build, p1, "P1")
    rc, ids, text = run_placement_gate(board, build,
                                       "seam-fixtures/p1_loadable_low.elf",
                                       nm, objdump)
    ok &= expect("P1 loadable section in the loader window is caught",
                 rc, ids, 1, {"FLOOR"}, text)

    if not ok:
        print("run_fixture_tests: FAIL", file=sys.stderr)
        return 1
    print("run_fixture_tests: OK (5 fixtures)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
