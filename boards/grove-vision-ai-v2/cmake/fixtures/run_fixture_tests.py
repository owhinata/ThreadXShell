#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 ThreadX Shell Project
"""Negative tests for the post-build gates that assert a LINKER property.

Covers cmake/check_timer_seam.py (issue #30), the loadable-SRAM floor and the
required-symbol rule in cmake/check_placement_budget.py (issues #29, #42), and
cmake/check_nor_seam.py (issue #88).

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
  P2  the FP precondition call deleted  -> REQUIRED (--gc-sections drops it)
  P3  the staging buffer without its
      section attribute                 -> RESIDENCY (it lands in DTCM)
  P4  the staging buffer in a section
      with no flags at all              -> ALLOC (it claims no memory)
  P0  the unmodified firmware           -> PASS

  Q0  the unmodified probe              -> PASS
  Q1  another TU calls the inner name   -> N5
  Q2  another TU references __real_     -> N6
  Q3  another TU revives the vendor's
      outer forwarder                   -> N5, N11 and N16
  Q4  a linked input left out of the
      manifest                          -> N2
  Q5  an authorised caller takes the
      ADDRESS instead of calling        -> N9
  Q6  chip erase made reachable         -> N8
  Q7  the probe without its forced
      wrapper references                -> N14 (the vacuous link)
  Q8  a map from a different link       -> N1
  Q9  a --wrap flag dropped             -> the LINK fails
  Q10 an input built with LTO           -> N2 and N4

[!] THE Q CASES RUN ON THE PROBE, NOT THE FIRMWARE.  Until issue #88 Part C
lands there is no caller for the NOR write path in `shell`, so the whole seam is
collected out of it and the rules would be true of nothing.  Q7 is the fixture
for exactly that shape of emptiness.

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
import shlex
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


def ninja_compile_command(build_dir, obj_suffix):
    """The compile line Ninja uses for the object ending in `obj_suffix`."""
    out = subprocess.run(["ninja", "-C", build_dir, "-t", "commands",
                          "shell_objs"],
                         check=True, capture_output=True, text=True).stdout
    for line in out.splitlines():
        if obj_suffix in line and " -c " in line:
            return line.strip()
    raise SystemExit(f"run_fixture_tests: no compile command for {obj_suffix}")


def compile_one(build_dir, cmd, what):
    r = subprocess.run(cmd, shell=True, cwd=build_dir,
                       capture_output=True, text=True)
    if r.returncode != 0:
        print(f"run_fixture_tests: fixture {what} failed to COMPILE -- the "
              f"fixture is broken, not the gate:\n{r.stderr[-2000:]}",
              file=sys.stderr)
        raise SystemExit(1)


def run_gate(board_dir, build_dir, elf, nm, objdump):
    r = subprocess.run(
        [sys.executable, os.path.join(board_dir, "cmake", "check_timer_seam.py"),
         "--nm", nm, "--objdump", objdump, "--require-archives",
         os.path.join(build_dir, elf)],
        capture_output=True, text=True)
    ids = set(re.findall(r"\[([A-Z]\d)\]", r.stdout + r.stderr))
    return r.returncode, ids, (r.stdout + r.stderr)


def nor_gate_command(build_dir, target):
    """The check_nor_seam.py invocation Ninja runs for `target`, verbatim.

    Read out of the real build for the same reason the link command is: the
    gate takes the writable interval and the erase unit as arguments, and a
    copy of those numbers here would be a second declaration of the layout --
    free to drift, and then these fixtures would be testing a rule the project
    does not enforce.
    """
    out = subprocess.run(["ninja", "-C", build_dir, "-t", "commands", target],
                         check=True, capture_output=True, text=True).stdout
    for line in reversed(out.splitlines()):
        for part in line.split(" && "):
            if "check_nor_seam.py" in part:
                return shlex.split(part)
    raise SystemExit("run_fixture_tests: no check_nor_seam.py command for "
                     f"{target} in {build_dir} -- build it first")


def nor_gate_variant(base, elf=None, mapfile=None, manifest=None,
                     callers=(), require_live=None):
    """`base` with only what a fixture needs changed."""
    cmd = list(base)

    # [!] AFTER THE SCRIPT PATH.  cmd[0] is the interpreter and cmd[1] the
    # script; inserting at 1 puts the option where python reads a filename, and
    # the gate exits 2 having parsed nothing -- which reads as "no diagnostics",
    # i.e. a fixture that failed silently rather than one that caught anything.
    def set_opt(name, value):
        if name in cmd:
            cmd[cmd.index(name) + 1] = value
        else:
            cmd.insert(2, value)
            cmd.insert(2, name)

    if mapfile is not None:
        set_opt("--map", mapfile)
    if manifest is not None:
        set_opt("--manifest", manifest)
    if require_live is False and "--require-live-wrappers" in cmd:
        cmd.remove("--require-live-wrappers")
    for c in callers:
        cmd.insert(2, c)
        cmd.insert(2, "--allow-caller")
    if elf is not None:
        cmd[-1] = elf
    return cmd


def run_nor_gate(cmd, build_dir):
    r = subprocess.run(cmd, cwd=build_dir, capture_output=True, text=True)
    ids = set(re.findall(r"\[(N\d+)\]", r.stdout + r.stderr))
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
    ids = set()
    if "loadable-SRAM floor" in text:
        ids.add("FLOOR")
    if "required symbol fp_enforce_judge" in text:
        ids.add("REQUIRED")
    if "pinned buffer blob_stage_buf" in text:
        ids.add("RESIDENCY")
    if "claims no memory" in text:
        ids.add("ALLOC")
    return r.returncode, ids, text


def link_only(cmd, output="shell.elf"):
    """Just the compiler invocation, without the PRE_LINK and POST_BUILD chain.

    Ninja reports a link with everything CMake chained onto it: the map deletion
    that runs before it, the image generation, and the gates.  A fixture wants
    the link and nothing else -- running the real image generator on a
    deliberately broken ELF would be slow and fail for its own reasons.

    [!] AND SINCE ISSUE #88 IT IS NOT OPTIONAL FOR THE PROBE EITHER.  The probe
    link now carries `rm -f seam_probe.map` in front of it and two gates behind
    it, and retarget() renames only the compiler's own -o and -Wl,-Map.  Running
    the whole reported command would delete the REAL map and then run the real
    gates against the real ELF with the map gone -- a fixture destroying the
    thing the next fixture reads.  (The command also STARTS with `: &&`, which
    is why this picks the segment out rather than truncating at the first &&.)
    """
    for part in cmd.split(" && "):
        if "arm-none-eabi-g++" in part and f"-o {output}" in part:
            return part.strip()
    raise SystemExit(f"run_fixture_tests: no compiler segment for {output}")


# Counted rather than written down: the total below was 17 while this file ran
# 17 fixtures, and stayed 17 when the eighteenth was added.  A number that has
# to be maintained by hand reads as coverage whether or not it is there.
RAN = 0


def expect(name, got_rc, got_ids, want_rc, want_ids, text):
    global RAN

    RAN += 1
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
    global RAN

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

    base = link_only(ninja_link_command(build, "seam_probe.elf"),
                     "seam_probe.elf")
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
    # [!] EVERY create_objects() the removed sources define, not just the
    # camera's.  Issue #57 gave the panel sink a thread of its own and main.c a
    # second call, and this fixture went on naming only the first -- so it
    # stopped LINKING at 3bb16e4 and stayed that way, unnoticed, because the
    # suite is run by hand.  A fixture that cannot link is a negative test that
    # is not running.
    f2 = f2_objs.replace(
        "arm-none-eabi-g++ ",
        "arm-none-eabi-g++ "
        "-Wl,--defsym=camera_create_objects=lcd_create_objects "
        "-Wl,--defsym=cam_lcd_sink_create_objects=lcd_create_objects ", 1)
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

    # --- check_placement_budget.py: the required symbol (issue #42) ----------
    #
    # P2: the call to the FP precondition is deleted from tx_glue.c.  That check
    # replaced the MVE predication scan, and it works ONLY because
    # --gc-sections drops an uncalled function -- so this fixture is what says
    # the requirement has teeth rather than being a name that happens to be
    # there.  Without it the replacement would be exactly the kind of gate the
    # scan turned out to be (issue #66): green, and unable to fail.
    #
    # The source is COPIED and edited, the way P1 copies the linker script, so
    # nothing in the firmware carries a hook that exists for a test.
    print("run_fixture_tests (check_placement_budget.py, required symbol):")

    glue_src = os.path.join(board, "port", "threadx", "tx_glue.c")
    glue_dst = os.path.join(outdir, "p2_no_fp_enforce.c")
    with open(glue_src) as f:
        glue = f.read()
    glue_broken = glue.replace("    fp_enforce_preconditions();\n", "")
    if glue_broken == glue:
        raise SystemExit("run_fixture_tests: tx_glue.c does not call "
                         "fp_enforce_preconditions(); the fixture cannot "
                         "remove it")
    with open(glue_dst, "w") as f:
        f.write(glue_broken)

    cc = ninja_compile_command(build, "port/threadx/tx_glue.c.obj")
    obj_old = re.search(r"-o (\S*tx_glue\.c\.obj)", cc).group(1)
    obj_new = "seam-fixtures/p2_tx_glue.obj"
    p2_cc = cc.replace(f"-o {obj_old}", f"-o {obj_new}")
    p2_cc = re.sub(r"-c \S*tx_glue\.c",
                   f"-I {os.path.dirname(glue_src)} -c {glue_dst}", p2_cc)
    # The dependency file would be written next to the original object.
    p2_cc = re.sub(r"-MD -MT \S+ -MF \S+", "", p2_cc)
    compile_one(build, p2_cc, "P2")

    p2 = shell_link.replace(obj_old, obj_new)
    if p2 == shell_link:
        raise SystemExit("run_fixture_tests: the firmware link does not name "
                         f"{obj_old}; the fixture cannot substitute it")
    p2 = retarget(p2, "shell.elf", "seam-fixtures/p2_no_fp_enforce.elf")
    link(build, p2, "P2")
    rc, ids, text = run_placement_gate(board, build,
                                       "seam-fixtures/p2_no_fp_enforce.elf",
                                       nm, objdump)
    ok &= expect("P2 a deleted FP precondition call is caught",
                 rc, ids, 1, {"REQUIRED"}, text)

    # --- check_placement_budget.py: buffer residency (issues #25, #92) -------
    #
    # P3: the blob staging buffer loses its section attribute, so it lands in
    # .bss -- which on this board is DTCM.
    #
    # [!] THIS IS THE FIRST NEGATIVE TEST THE RESIDENCY RULE HAS EVER HAD.  It
    # has pinned the benchmark buffers since #25, the framebuffer since #30 and
    # the camera and NN buffers since #35 and #44, and nothing had ever watched
    # it fail -- the shape of gate this repository has been bitten by twice
    # (issues #66, #42).  The staging buffer is the case where it matters most
    # and shows least: unlike the framebuffer and the DMA landing buffers,
    # NOTHING would break.  No DMA reads it, so in DTCM it would work perfectly
    # while spending 64 KB of the heap-to-stack gap that this same gate guards
    # from the other side.
    print("run_fixture_tests (check_placement_budget.py, buffer residency):")

    stage_src = os.path.join(board, "src", "blob_stage.c")
    stage_dst = os.path.join(outdir, "p3_unpinned_stage.c")
    with open(stage_src) as f:
        stage = f.read()
    stage_broken = stage.replace('section(".blob_stage"), ', "")
    if stage_broken == stage:
        raise SystemExit("run_fixture_tests: blob_stage.c does not place the "
                         "buffer with a section attribute; the fixture cannot "
                         "remove it")
    with open(stage_dst, "w") as f:
        f.write(stage_broken)

    cc = ninja_compile_command(build, "src/blob_stage.c.obj")
    obj_old = re.search(r"-o (\S*blob_stage\.c\.obj)", cc).group(1)
    obj_new = "seam-fixtures/p3_blob_stage.obj"
    p3_cc = cc.replace(f"-o {obj_old}", f"-o {obj_new}")
    p3_cc = re.sub(r"-c \S*blob_stage\.c",
                   f"-I {os.path.dirname(stage_src)} -c {stage_dst}", p3_cc)
    p3_cc = re.sub(r"-MD -MT \S+ -MF \S+", "", p3_cc)
    compile_one(build, p3_cc, "P3")

    p3 = shell_link.replace(obj_old, obj_new)
    if p3 == shell_link:
        raise SystemExit("run_fixture_tests: the firmware link does not name "
                         f"{obj_old}; the fixture cannot substitute it")
    p3 = retarget(p3, "shell.elf", "seam-fixtures/p3_unpinned_stage.elf")
    link(build, p3, "P3")
    rc, ids, text = run_placement_gate(board, build,
                                       "seam-fixtures/p3_unpinned_stage.elf",
                                       nm, objdump)
    ok &= expect("P3 a staging buffer that drifted out of SRAM is caught",
                 rc, ids, 1, {"RESIDENCY"}, text)

    # P4: the same buffer in a section that claims no memory -- no ALLOC and
    # no CONTENTS -- at the address the reservation was given.  The gate's
    # overlap check only looks at allocated sections, so such a section keeps
    # its address and its symbol while dropping out of it, and the next section
    # could be handed the same range with nothing to say so.
    #
    # [!] THIS IS THE ONLY FIXTURE HERE THAT NO OTHER RULE CATCHES.  It came
    # from review of 1c3730d, and it took two rounds: four attempts to build it
    # (output section (INFO) and (COPY), objcopy --set-section-flags, and the
    # asm section below on its own) all came back with CONTENTS, which the
    # NOLOAD rule refuses -- so the finding looked unreachable.  Sending that
    # evidence back produced the combination that works: a non-ALLOC %nobits
    # INPUT section and an output section whose type is overridden to
    # SHT_NOBITS.  Against the gate as it shipped in 1c3730d this ELF passes.
    #
    # It needs both halves, which is why it edits a source AND the linker
    # script.  The size comes out of the real header so it cannot drift from
    # the reservation it stands in for, and `.size` is what keeps nm reporting
    # the symbol -- without it residency answers first and the fixture would be
    # passing on that rule's back.
    stage_h = os.path.join(board, "src", "blob_stage.h")
    with open(stage_h) as f:
        m = re.search(r"BLOB_STAGE_BYTES\s+\((\d+)u \* (\d+)u\)", f.read())
    if not m:
        raise SystemExit("run_fixture_tests: cannot read BLOB_STAGE_BYTES out "
                         "of blob_stage.h; the fixture cannot size itself")
    stage_bytes = int(m.group(1)) * int(m.group(2))

    asm_dst = os.path.join(outdir, "p4_unallocated_stage.c")
    with open(asm_dst, "w") as f:
        f.write('__asm__(".section .blob_stage,\\"\\",%nobits\\n"\n'
                '        ".global blob_stage_buf\\n"\n'
                '        ".balign 32\\n"\n'
                f'        "blob_stage_buf: .space {stage_bytes}\\n"\n'
                f'        ".size blob_stage_buf, {stage_bytes}\\n"\n'
                '        ".type blob_stage_buf, %object\\n"\n'
                '        ".previous\\n");\n')

    p4_cc = cc.replace(f"-o {obj_old}", "-o seam-fixtures/p4_blob_stage.obj")
    p4_cc = re.sub(r"-c \S*blob_stage\.c", f"-c {asm_dst}", p4_cc)
    p4_cc = re.sub(r"-MD -MT \S+ -MF \S+", "", p4_cc)
    compile_one(build, p4_cc, "P4")

    ld_nobits = os.path.join(outdir, "p4_nobits_type.ld")
    ld_typed = ld.replace(".blob_stage (NOLOAD) : ALIGN(32)",
                          ".blob_stage (TYPE = SHT_NOBITS) : ALIGN(32)")
    if ld_typed == ld:
        raise SystemExit("run_fixture_tests: .blob_stage is not NOLOAD in the "
                         "linker script; the fixture cannot retype it")
    with open(ld_nobits, "w") as f:
        f.write(ld_typed)

    p4 = shell_link.replace(obj_old, "seam-fixtures/p4_blob_stage.obj")
    p4 = re.sub(r"-T\S*HX6538_CM55M_S\.ld", f"-T{ld_nobits}", p4)
    p4 = retarget(p4, "shell.elf", "seam-fixtures/p4_unallocated_stage.elf")
    link(build, p4, "P4")
    rc, ids, text = run_placement_gate(board, build,
                                       "seam-fixtures/p4_unallocated_stage.elf",
                                       nm, objdump)
    # Exactly one id.  Residency finds the symbol, at the right address, the
    # right size and inside its own section; the NOLOAD rule finds no CONTENTS.
    # Everything the gate checked before 1c3730d is satisfied.
    ok &= expect("P4 a reservation that claims no memory is caught",
                 rc, ids, 1, {"ALLOC"}, text)

    # --- check_nor_seam.py: who may reach the NOR write path (issue #88) -----
    #
    # The gate's claim is that ONE translation unit reaches the vendor's erase
    # and program entry points.  Every case below is a different way another one
    # could, linked for real into the probe -- and each asserts the DIAGNOSTIC,
    # because several of these trip more than one rule and a fixture that
    # settles for "refused" stays green when the rule it was written for dies.
    #
    # [!] THE PROBE, NOT THE FIRMWARE.  Until issue #88 Part C lands the
    # firmware has no caller for the write path at all, so `shell` collects the
    # whole seam away and there is nothing for these to be true or false about.
    # That is exactly why board.cmake forces the wrapper references in the probe.
    print("run_fixture_tests (check_nor_seam.py):")

    nor_base = nor_gate_command(build, "seam_probe.elf")
    manifest = nor_base[nor_base.index("--manifest") + 1]
    with open(manifest) as f:
        manifest_lines = [l.strip() for l in f if l.strip()]

    # Q0 first, for the reason F3 and P0 come first: without it the cases below
    # could all be failing for some unrelated reason.
    rc, ids, text = run_nor_gate(nor_base, build)
    ok &= expect("Q0 pristine probe passes", rc, ids, 0, set(), text)

    fixture_src = os.path.join(board, "cmake", "fixtures", "nor_seam_caller.c")
    seam_cc = ninja_compile_command(build, "port/sdk_seam/nor_seam.c.obj")
    seam_obj = re.search(r"-o (\S*nor_seam\.c\.obj)", seam_cc).group(1)

    def build_case(case, extra_cflags=""):
        """Compile one FX_ case with the FIRMWARE's own compile line, and link
        it into the probe with the wrapper references still forced."""
        obj = f"seam-fixtures/q_{case.lower()}.obj"
        cc = seam_cc.replace(f"-o {seam_obj}", f"-o {obj}")
        cc = re.sub(r"-c \S*nor_seam\.c",
                    f"{extra_cflags} -D{case} -c {fixture_src}", cc)
        cc = re.sub(r"-MD -MT \S+ -MF \S+", "", cc)
        compile_one(build, cc, case)
        elf = f"seam-fixtures/q_{case.lower()}.elf"
        mapfile = f"seam-fixtures/q_{case.lower()}.map"
        link_cmd = base.replace(
            "arm-none-eabi-g++ ",
            f"arm-none-eabi-g++ {obj} -Wl,-u,nor_fixture ", 1)
        link_cmd = retarget(link_cmd, "seam_probe.elf", elf)
        link(build, link_cmd, case)
        # The manifest is what tells the gate which inputs it may audit, so a
        # new translation unit belongs in it -- board.cmake would have put it
        # there.  Q4 is the case that leaves it out on purpose.
        man = os.path.join(build, "seam-fixtures", f"q_{case.lower()}.txt")
        with open(man, "w") as f:
            f.write("\n".join(manifest_lines
                               + [os.path.join(build, obj)]) + "\n")
        return elf, mapfile, man, obj

    # Q1: another translation unit calls the wrapped entry point.  --wrap still
    # sends it to the seam, so it is still bounded -- but by a caller nobody
    # enumerated, and the enumeration is the whole claim.
    elf, mp, man, _obj = build_case("FX_INNER_CALL")
    rc, ids, text = run_nor_gate(
        nor_gate_variant(nor_base, elf=elf, mapfile=mp, manifest=man), build)
    ok &= expect("Q1 an unlisted caller of the inner name is caught",
                 rc, ids, 1, {"N5"}, text)

    # Q2: straight past the seam -- __real_ IS the vendor implementation.
    elf, mp, man, _obj = build_case("FX_REAL_CALL")
    rc, ids, text = run_nor_gate(
        nor_gate_variant(nor_base, elf=elf, mapfile=mp, manifest=man), build)
    ok &= expect("Q2 a __real_ reference outside the seam is caught",
                 rc, ids, 1, {"N6"}, text)

    # Q3: the case that makes this gate work on input sections rather than on
    # objects.  spi_eeprom_comm.o is already in the link; this file only keeps
    # its outer forwarder alive, and reaches the inner entry point through it.
    #
    # [!] ALL THREE IDS ARE REQUIRED.  Reviving the forwarder trips the absence
    # rule (N11) on its own, so asserting only "refused" would leave the
    # caller-edge rule (N16) free to be deleted with this fixture still green.
    # N5 is the third: the revived forwarder is itself a live section calling
    # the inner name, from an object that is not an authorised caller -- which
    # is the same rule Q1 tests, arriving here through vendor code.  That it
    # fires is what makes "classify by input section" more than a phrase: an
    # object-level rule would have had to permit spi_eeprom_comm.o outright.
    elf, mp, man, _obj = build_case("FX_OUTER_CALL")
    rc, ids, text = run_nor_gate(
        nor_gate_variant(nor_base, elf=elf, mapfile=mp, manifest=man), build)
    ok &= expect("Q3 reaching the inner name through the vendor's forwarder "
                 "is caught, by all three rules",
                 rc, ids, 1, {"N5", "N11", "N16"}, text)

    # Q4: the same file as Q1, left OUT of the manifest.  An input the gate
    # never opens is an input whose calls it never sees, so the manifest check
    # is what stops the audit being narrowed by adding a file.
    elf, mp, _man, _obj = build_case("FX_INNER_CALL")
    rc, ids, text = run_nor_gate(
        nor_gate_variant(nor_base, elf=elf, mapfile=mp), build)
    ok &= expect("Q4 an input missing from the manifest is caught",
                 rc, ids, 1, {"N2"}, text)

    # Q5: an AUTHORISED caller that takes the address instead of calling.  Run
    # with the fixture allowed, so the address rule is the only one left to
    # fire -- an edge audit that accepted this would be following a call graph
    # the program does not have.
    elf, mp, man, obj = build_case("FX_ADDRESS")
    rc, ids, text = run_nor_gate(
        nor_gate_variant(nor_base, elf=elf, mapfile=mp, manifest=man,
                         callers=[os.path.basename(obj)]), build)
    ok &= expect("Q5 taking the address instead of calling is caught",
                 rc, ids, 1, {"N9"}, text)

    # Q6: chip erase reached from an authorised caller.  It is refused because
    # it has no address to be bounded against, not because of who called it --
    # so the authorised caller is what isolates that rule.
    elf, mp, man, obj = build_case("FX_CHIP_ERASE")
    rc, ids, text = run_nor_gate(
        nor_gate_variant(nor_base, elf=elf, mapfile=mp, manifest=man,
                         callers=[os.path.basename(obj)]), build)
    ok &= expect("Q6 a live reference to chip erase is caught",
                 rc, ids, 1, {"N8"}, text)

    # Q7: the probe linked WITHOUT the forced wrapper references.  Every rule
    # above then passes over an empty set, which is the shape of gate this
    # repository has been bitten by twice (issues #66, #42).
    q7 = base
    for _sym in ("erase_sector", "write", "erase_all", "word_write"):
        q7 = q7.replace(f"-Wl,-u,__wrap_hx_lib_qspi_eeprom_{_sym} ", "")
    if q7 == base:
        raise SystemExit("run_fixture_tests: the probe link forces no NOR "
                         "wrapper references; the fixture cannot remove them")
    q7 = retarget(q7, "seam_probe.elf", "seam-fixtures/q7_collected.elf")
    link(build, q7, "Q7")
    rc, ids, text = run_nor_gate(
        nor_gate_variant(nor_base, elf="seam-fixtures/q7_collected.elf",
                         mapfile="seam-fixtures/q7_collected.map"), build)
    ok &= expect("Q7 a link with the whole seam collected away is caught",
                 rc, ids, 1, {"N14"}, text)

    # Q8: a map from a DIFFERENT link.  Everything this gate decides about
    # live-vs-discarded comes out of the map, so a stale one answers about
    # another link -- board.cmake deletes the map PRE_LINK, and this is what
    # says the cross-check behind that belt is fastened too.
    rc, ids, text = run_nor_gate(
        nor_gate_variant(nor_base, mapfile="shell.map"), build)
    ok &= expect("Q8 a map from another link is caught",
                 rc, ids, 1, {"N1"}, text)

    # Q9: a --wrap flag dropped.  There is no gate diagnostic for this and there
    # does not need to be: with the reference forced, __real_ resolves to
    # nothing and the LINK fails.  Asserted as a link failure naming the symbol,
    # because "the link broke" on its own could be any mistake in the fixture.
    q9 = base.replace("-Wl,--wrap=hx_lib_qspi_eeprom_write ", "")
    if q9 == base:
        raise SystemExit("run_fixture_tests: --wrap=hx_lib_qspi_eeprom_write is "
                         "not in the probe link; the fixture cannot break it")
    q9 = retarget(q9, "seam_probe.elf", "seam-fixtures/q9_unwrapped.elf")
    r = subprocess.run(q9, shell=True, cwd=build, capture_output=True, text=True)
    q9_ok = (r.returncode != 0 and
             "__real_hx_lib_qspi_eeprom_write" in (r.stdout + r.stderr))
    RAN += 1   # asserted by hand rather than through expect(); still a fixture
    print(f"  {'ok  ' if q9_ok else 'FAIL'} Q9 a dropped --wrap is a link error")
    if not q9_ok:
        print("       wanted a failed link naming "
              "__real_hx_lib_qspi_eeprom_write; got rc="
              f"{r.returncode}\n{r.stderr[-1500:]}", file=sys.stderr)
    ok &= q9_ok

    # Q10: the same call, in a translation unit built with LTO.  Two things go
    # wrong at once and both diagnostics are wanted:
    #
    #   N4  there are no relocations against the vendor names in an LTO object.
    #       The calls are still in the IR and do not exist until the plugin
    #       recompiles at link time, so every rule above would pass over a file
    #       doing exactly what Q1 does.
    #   N2  and the linker's real inputs stop being the ones CMake declared --
    #       the plugin hands ld /tmp/cc*.ltrans0.ltrans.o, which no manifest
    #       could ever name.
    #
    # Refusing to audit what it cannot read is the only honest answer, and it is
    # why this board bans LTO rather than working around it.
    elf, mp, man, _obj = build_case("FX_INNER_CALL", extra_cflags="-flto")
    rc, ids, text = run_nor_gate(
        nor_gate_variant(nor_base, elf=elf, mapfile=mp, manifest=man), build)
    ok &= expect("Q10 an LTO input is refused rather than audited",
                 rc, ids, 1, {"N2", "N4"}, text)

    if not ok:
        print("run_fixture_tests: FAIL", file=sys.stderr)
        return 1
    print(f"run_fixture_tests: OK ({RAN} fixtures)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
