#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 ThreadX Shell Project
"""Build-level tests for cmake/veneer_cost_gate.cmake (issue #112).

check_veneer_base_cost.py derives the stack the firmware spends below each
plugin veneer; cmake/fixtures/run_veneer_cost_tests.py tests THAT.  What is
tested here is the other half, the part that makes the check a gate: the
wiring.  A check that is correct but that a build can go around is a report,
and every way around it is a CMake / ninja fact, so it is tested by building --
a small real Cortex-M7 project, configured by real cmake and built by real
ninja with the repository's pinned cross compiler, against COPIES of the helper
and both scripts (so a script can be edited here, and a mutated helper passed
with --cmake-dir, without touching the tree).

[!] THE DELIVERIES REFUSE ON THEIR OWN.  The fake `flash` and the fake assets
do not trust the dependency graph under test: each one runs deliver.py, which
fails unless the stamp exists AND is newer than the image.  So "flash ran
before the check", "an asset was packed with no check", and "a stamp from an
earlier image let it through" are each a failure of the delivery itself, not a
property inferred from ninja's plan.

The project mirrors how the boards feed their image, one of each:
  - root.c         in the executable itself (the six veneer roots),
  - objsrc.c       an OBJECT library taken by $<TARGET_OBJECTS:> (Grove's
                   shell_objs),
  - objlink.c      an OBJECT library in target_link_libraries (coremark_obj),
  - lib.c          a STATIC library reached through an INTERFACE library
                   (wio's tflm, Grove's bsp_iface),
  - other.c        an unrelated executable (wio's boot reference build) that
                   must NOT be instrumented.
Each root calls into one of the two object libraries, so a body with no record
below a veneer -- a compile the helper did not find -- fails the check.  The
static library is reached from main() only: its record is asserted to EXIST
(the helper found the compile), but the check cannot use it, because it cannot
yet name the record of an in-tree archive member and refuses one below a veneer
(fail-closed; the C4 hand-over of #112, and wio's libtflm.a).

Cases:
  firmware_alone    `ninja shell` builds the image and leaves no stamp: the
                    check is its own edge, not a side effect of linking.
  flash_waits       then `ninja flash` runs the check before the flash command.
  noop              `ninja` again reruns nothing.
  witnesses         every TU of the image has its record; other.c has none.
  script_change     editing either script (the veneer set lives in
                    check_plugin_image.py) reruns the check without a relink.
  image_change      a source edit that relinks the image reruns the check.
  relink_retracts   building the IMAGE alone relinks without checking, and
                    that link removes the stamp: a pass cannot outlive the
                    image it was about.
  failure           DECLARED one below the derived value: the build fails
                    naming the chain, no stamp survives, and neither flash nor
                    either asset proceeds.
  recovery          the derived value again: the stamp comes back and flash
                    proceeds.
  asset_alone       from a fresh tree, `ninja asset-bare` (an asset with no
                    dependency of its own) runs the check first.
  asset_packed      the same for an asset whose pack rule names the gate, as
                    the boards' do.
  ltrans_drop       an LTO link that writes fewer partitions than the last
                    (max -> one) still passes: the old records are removed
                    before the link, not left for the check to refuse.
  cost_wiring       (issue #111) DECLARED reaches BOTH places the loader's c
                    is read from, and follows a change of it: the compile of a
                    TU in the $<TARGET_OBJECTS:> library (where Grove's policy
                    lives) as PLUGIN_VENEER_BASE_COST, and an asset rule's
                    command line through veneer_cost_gate_declared() (where the
                    host container verifier gets it).  A loader test cannot see
                    this: it is handed a policy, never the build's.
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
COPIED = ("veneer_cost_gate.cmake", "check_veneer_base_cost.py",
          "check_plugin_image.py", "check_policy_probe.py")

LINKER_SCRIPT = """
MEMORY { FLASH (rx) : ORIGIN = 0x08000000, LENGTH = 1M
         RAM (rwx) : ORIGIN = 0x20000000, LENGTH = 64K }
SECTIONS {
  .text : { *(.text*) } > FLASH
  .rodata : { *(.rodata*) } > FLASH
  .data : { *(.data*) } > RAM
  .bss : { *(.bss*) *(COMMON) } > RAM
}
"""

# A frame in every body (the volatile array), and no inlining, so that each
# chain goes through the TU it names -- LTO included.
LEAF = """
__attribute__((noinline, used)) int %s(int x)
{
    volatile char b[%d];
    b[0] = (char)x;
    return b[0] + 1;
}
"""

SOURCES = {
    "root.c": """
extern int obj_src_leaf(int);
extern int obj_link_leaf(int);
extern int lib_leaf(int);
#define ROOT(name, callee, n)                                              \\
    __attribute__((noinline, used)) int name(int x)                        \\
    {                                                                      \\
        volatile char b[n];                                                \\
        b[0] = (char)x;                                                    \\
        return callee(b[0]) + 1;                                           \\
    }
ROOT(nn_plugin_log, obj_src_leaf, 16)
ROOT(nn_active_to_frame, obj_link_leaf, 24)
ROOT(paint_rect, obj_src_leaf, 32)
ROOT(paint_fill_rect, obj_link_leaf, 8)
ROOT(paint_blit, obj_src_leaf, 8)
ROOT(nn_report_write, obj_link_leaf, 8)
int main(void)
{
    return nn_plugin_log(1) + nn_active_to_frame(2) + paint_rect(3)
           + paint_fill_rect(4) + paint_blit(5) + nn_report_write(6)
           + lib_leaf(7);
}
""",
    "objsrc.c": LEAF % ("obj_src_leaf", 40),
    # The board's plugin policy stands here: it reads the c the helper
    # defines, and its SIZE carries the value into the object, where nm reads it.
    "policy.c": """
#include "plugin_load.h"
#ifndef PLUGIN_VENEER_BASE_COST
#error "PLUGIN_VENEER_BASE_COST did not reach this compile"
#endif
__attribute__((used)) char policy_cost_probe[PLUGIN_VENEER_BASE_COST];
/* A board's policy and its probe, as nn_svc_grove.c / nn_svc_wio.c write them:
 * the stamp waits for check_policy_probe.py to read this back from shell.elf. */
static const struct plugin_policy fix_policy = {
    .stack_limit      = { 1, 2, 3, 4, 5, 6, 7 },
    .veneer_cost      = PLUGIN_VENEER_BASE_COST,
    .stack_accounting = PLUGIN_STACK_ACCOUNTING,
};
PLUGIN_POLICY_PROBE(fix_policy);
""",
    "record.py": """import sys
open(sys.argv[2], "w").write(sys.argv[1] + "\\n")
""",
    "objlink.c": LEAF % ("obj_link_leaf", 48),
    "lib.c": LEAF % ("lib_leaf", 56),
    "other.c": "int main(void) { volatile char b[8]; b[0] = 1; "
               "return b[0]; }\n",
    # The delivery's own refusal (see the module docstring).
    "deliver.py": """import os, sys
stamp, image, out = sys.argv[1:4]
if not os.path.exists(stamp):
    sys.exit("deliver: no stamp -- delivered without a passed check")
if os.path.getmtime(stamp) < os.path.getmtime(image):
    sys.exit("deliver: the stamp is older than the image")
open(out, "w").write("delivered\\n")
""",
}

CMAKELISTS = r"""
cmake_minimum_required(VERSION 3.20)
project(veneer_gate_build C)
set(Python3_EXECUTABLE "@PYTHON@")
set(DECLARED 4096 CACHE STRING "")
set(FIX_LTO OFF CACHE STRING "")

add_library(fw_objs OBJECT objsrc.c policy.c)
target_include_directories(fw_objs PRIVATE "@SVC@")
add_library(fw_objlink OBJECT objlink.c)
add_library(fw_lib STATIC lib.c)
add_library(fw_iface INTERFACE)
target_link_libraries(fw_iface INTERFACE fw_lib)

add_executable(shell root.c $<TARGET_OBJECTS:fw_objs>)
set_target_properties(shell PROPERTIES SUFFIX .elf)
target_link_libraries(shell PRIVATE fw_iface fw_objlink)
target_link_options(shell PRIVATE
    -T "${CMAKE_SOURCE_DIR}/fix.ld" "-Wl,-Map=${CMAKE_BINARY_DIR}/shell.map"
    -Wl,-e,main)
if(NOT FIX_LTO STREQUAL "OFF")
    # The static library stays a plain archive (no gcc-ar needed).
    foreach(t shell fw_objs fw_objlink)
        target_compile_options(${t} PRIVATE -flto)
    endforeach()
    target_link_options(shell PRIVATE -flto -flto-partition=${FIX_LTO})
endif()

add_executable(other other.c)
set_target_properties(other PROPERTIES SUFFIX .elf)
target_link_options(other PRIVATE -T "${CMAKE_SOURCE_DIR}/fix.ld" -Wl,-e,main)

set(STAMP "${CMAKE_BINARY_DIR}/veneer_cost/shell.checked")
set(DELIVER "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/deliver.py"
    "${STAMP}" "$<TARGET_FILE:shell>")
add_custom_target(flash
    COMMAND ${DELIVER} "${CMAKE_BINARY_DIR}/flashed"
    DEPENDS shell VERBATIM)

include("${CMAKE_SOURCE_DIR}/cmake/veneer_cost_gate.cmake")
veneer_cost_gate(
    FIRMWARE shell
    MAP      "${CMAKE_BINARY_DIR}/shell.map"
    DECLARED ${DECLARED}
    ROOTS    pl_base_log=nn_plugin_log
             pl_base_to_frame=nn_active_to_frame
             pl_paint_rect=paint_rect
             pl_paint_fill_rect=paint_fill_rect
             pl_paint_blit=paint_blit
             pl_print_write=nn_report_write
    PREBUILT_ROOTS "@PREBUILT@"
    DELIVERY flash)

# An asset with no dependency of its own: only the helper's name match stands
# between it and an unchecked firmware.
add_custom_target(asset-bare
    COMMAND ${DELIVER} "${CMAKE_BINARY_DIR}/bare.nnc" VERBATIM)
# An asset whose pack rule names the gate, as grove/wio_add_asset() do.
get_property(_gate GLOBAL PROPERTY VENEER_GATE_TARGET)
add_custom_command(OUTPUT "${CMAKE_BINARY_DIR}/packed.nnc"
    COMMAND ${DELIVER} "${CMAKE_BINARY_DIR}/packed.nnc"
    DEPENDS ${_gate} VERBATIM)
add_custom_target(asset-packed DEPENDS "${CMAKE_BINARY_DIR}/packed.nnc")
# The c an asset hands the host container verifier, as grove/wio_add_asset()
# take it (issue #111).
veneer_cost_gate_declared(_cost)
add_custom_target(asset-cost
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/record.py"
            "${_cost}" "${CMAKE_BINARY_DIR}/asset_cost.txt"
    VERBATIM)
"""


class Fail(Exception):
    pass


class Project:
    def __init__(self, work, cmake_dir, cc):
        self.src = os.path.join(work, "src")
        self.bld = os.path.join(work, "b")
        os.makedirs(os.path.join(self.src, "cmake"))
        for f in COPIED:
            shutil.copy(os.path.join(cmake_dir, f),
                        os.path.join(self.src, "cmake", f))
        for name, text in SOURCES.items():
            with open(os.path.join(self.src, name), "w") as fh:
                fh.write(text)
        with open(os.path.join(self.src, "fix.ld"), "w") as fh:
            fh.write(LINKER_SCRIPT)
        prebuilt = os.path.dirname(os.path.dirname(cc))
        with open(os.path.join(self.src, "CMakeLists.txt"), "w") as fh:
            fh.write(CMAKELISTS.replace("@PYTHON@", sys.executable)
                     .replace("@PREBUILT@", prebuilt)
                     .replace("@SVC@", os.path.join(REPO, "svc")))
        with open(os.path.join(work, "toolchain.cmake"), "w") as fh:
            fh.write(
                "set(CMAKE_SYSTEM_NAME Generic)\n"
                "set(CMAKE_SYSTEM_PROCESSOR arm)\n"
                'set(CMAKE_C_COMPILER "%s")\n'
                "set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)\n"
                'set(CMAKE_C_FLAGS_INIT "-mcpu=cortex-m7 -mthumb '
                "-mfpu=fpv5-d16 -mfloat-abi=hard -Os -ffreestanding "
                "-fno-builtin -ffunction-sections -fno-unwind-tables "
                '-fno-asynchronous-unwind-tables")\n'
                'set(CMAKE_EXE_LINKER_FLAGS_INIT "-nostdlib -nostartfiles '
                '-Wl,--no-warn-rwx-segments")\n' % cc)
        self.toolchain = os.path.join(work, "toolchain.cmake")
        self.c_init = ("-mcpu=cortex-m7 -mthumb -mfpu=fpv5-d16 "
                       "-mfloat-abi=hard -Os -ffreestanding -fno-builtin "
                       "-ffunction-sections -fno-unwind-tables "
                       "-fno-asynchronous-unwind-tables")
        self.cc = cc
        self.stamp = os.path.join(self.bld, "veneer_cost", "shell.checked")
        self.elf = os.path.join(self.bld, "shell.elf")

    def configure(self, **defs):
        cmd = ["cmake", "-G", "Ninja", "-S", self.src, "-B", self.bld,
               "-DCMAKE_TOOLCHAIN_FILE=" + self.toolchain]
        cmd += ["-D%s=%s" % kv for kv in defs.items()]
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode != 0:
            raise Fail("configure failed:\n" + r.stdout[-1500:]
                       + r.stderr[-1500:])

    def ninja(self, *targets):
        r = subprocess.run(["ninja", "-C", self.bld] + list(targets),
                           capture_output=True, text=True)
        return r.returncode, r.stdout + r.stderr

    def path(self, name):
        return os.path.join(self.bld, name)

    def rm(self, name):
        p = self.path(name)
        if os.path.exists(p):
            os.remove(p)

    def records(self):
        found = []
        for root, _, files in os.walk(self.bld):
            found += [f for f in files if f.endswith(".su")]
        return found


def expect(cond, why, out=""):
    if not cond:
        raise Fail(why + ("\n" + out[-2000:] if out else ""))


def ran_check(out):
    return "check_veneer_base_cost: " in out


def derived_of(out):
    m = re.search(r"VENEER_BASE_COST (\d+) B >= (\d+) B derived", out)
    if not m:
        raise Fail("no OK line to read the derived value from:\n" + out[-1500:])
    return int(m.group(2))


def main_tree(p, results):
    """The cases that run in sequence on one tree."""
    p.configure(DECLARED="4096")

    rc, out = p.ninja("shell")
    expect(rc == 0, "firmware_alone: `ninja shell` failed", out)
    expect(os.path.exists(p.elf) and not os.path.exists(p.stamp),
           "firmware_alone: the image should exist and the stamp should not",
           out)
    results.append(("firmware_alone", "the image links; no stamp yet"))

    rc, out = p.ninja("flash")
    expect(rc == 0, "flash_waits: `ninja flash` failed", out)
    expect(ran_check(out) and os.path.exists(p.stamp)
           and os.path.exists(p.path("flashed")),
           "flash_waits: flash should have run the check, then delivered",
           out)
    results.append(("flash_waits", "the check ran before the flash command"))

    rc, out = p.ninja()
    expect(rc == 0, "noop: `ninja` failed", out)
    elf_mtime = os.path.getmtime(p.elf)
    rc, out = p.ninja()
    expect(rc == 0 and not ran_check(out) and "no work to do" in out,
           "noop: a second `ninja` should do nothing", out)
    results.append(("noop", "a second `ninja` reruns nothing"))

    recs = p.records()
    for tu in ("root.c.su", "objsrc.c.su", "objlink.c.su", "lib.c.su"):
        expect(any(r.endswith(tu) for r in recs),
               "witnesses: no record for %s (records: %s)" % (tu, recs))
    expect(not any(r.endswith("other.c.su") for r in recs),
           "witnesses: other.c was instrumented (records: %s)" % recs)
    results.append(("witnesses", "all four TUs of the image have a record, "
                                 "other.c none"))

    for script in ("check_veneer_base_cost.py", "check_plugin_image.py"):
        with open(os.path.join(p.src, "cmake", script), "a") as fh:
            fh.write("\n# touched by the build test\n")
        rc, out = p.ninja()
        expect(rc == 0 and ran_check(out),
               "script_change: editing %s did not rerun the check" % script,
               out)
        expect(os.path.getmtime(p.elf) == elf_mtime,
               "script_change: editing %s relinked the image" % script, out)
    results.append(("script_change", "either script reruns the check, "
                                     "no relink"))

    with open(os.path.join(p.src, "root.c"), "a") as fh:
        fh.write("\nint image_change_marker(void) { return 3; }\n")
    rc, out = p.ninja()
    expect(rc == 0 and ran_check(out)
           and os.path.getmtime(p.elf) != elf_mtime,
           "image_change: a relinked image did not rerun the check", out)
    results.append(("image_change", "a relinked image reruns the check"))

    # The derived value, from a run of the check itself.
    os.remove(p.stamp)
    rc, out = p.ninja()
    expect(rc == 0 and ran_check(out), "failure: could not rerun the check",
           out)
    derived = derived_of(out)

    p.configure(DECLARED=str(derived - 1))
    for f in ("flashed", "bare.nnc", "packed.nnc"):
        p.rm(f)
    rc, out = p.ninja()
    flat = " ".join(out.split())
    expect(rc != 0 and "[budget]" in flat
           and ("needs %d B below it (" % derived) in flat
           and ("declares %d B" % (derived - 1)) in flat,
           "failure: the build should fail in [budget] naming the chain", out)
    expect(not os.path.exists(p.stamp),
           "failure: a stamp survived the failed check", out)
    for target, made in (("flash", "flashed"), ("asset-bare", "bare.nnc"),
                         ("asset-packed", "packed.nnc")):
        rc, out = p.ninja(target)
        expect(rc != 0 and not os.path.exists(p.path(made)),
               "failure: `ninja %s` proceeded past the failed check"
               % target, out)
    expect(not os.path.exists(p.stamp),
           "failure: a stamp appeared after the failed check")
    results.append(("failure", "DECLARED %d < %d: build fails naming the "
                               "chain, no stamp, flash and both assets "
                               "stopped" % (derived - 1, derived)))

    p.configure(DECLARED=str(derived))
    rc, out = p.ninja()
    expect(rc == 0 and ran_check(out) and os.path.exists(p.stamp),
           "recovery: the derived value should pass again", out)
    rc, out = p.ninja("flash")
    expect(rc == 0 and os.path.exists(p.path("flashed")),
           "recovery: flash should proceed", out)
    results.append(("recovery", "DECLARED %d: stamp back, flash proceeds"
                                % derived))

    # [!] A STAMP MUST NOT OUTLIVE THE IMAGE IT DESCRIBES.  Building the image
    # alone -- `ninja shell.img` on a board, anything that needs the ELF but
    # not the check -- relinks without checking, and before this the previous
    # run's passing stamp stayed behind, describing an image that no longer
    # existed.  Delivery still waited for the check, so nothing unchecked
    # shipped; what was wrong is that the stamp went on asserting a pass.
    with open(os.path.join(p.src, "root.c"), "a") as fh:
        fh.write("\nint relink_marker(void) { return 4; }\n")
    before = os.path.getmtime(p.elf)
    rc, out = p.ninja("shell")
    expect(rc == 0 and os.path.getmtime(p.elf) != before,
           "relink_retracts_stamp: the image should have been relinked", out)
    expect(not ran_check(out),
           "relink_retracts_stamp: `ninja shell` should not run the check "
           "(if it does, this case no longer tests anything)", out)
    expect(not os.path.exists(p.stamp),
           "relink_retracts_stamp: a passing stamp survived a relink that "
           "did not re-run the check", out)
    rc, out = p.ninja("flash")
    expect(rc == 0 and ran_check(out) and os.path.exists(p.stamp),
           "relink_retracts_stamp: the next delivery should check and stamp "
           "again", out)
    results.append(("relink_retracts_stamp",
                    "`ninja shell` alone relinks and leaves no stamp"))


def asset_tree(p, target, made, results, name):
    p.configure(DECLARED="4096")
    rc, out = p.ninja(target)
    expect(rc == 0 and ran_check(out) and os.path.exists(p.stamp)
           and os.path.exists(p.path(made)),
           "%s: `ninja %s` from a fresh tree should run the check first"
           % (name, target), out)
    results.append((name, "`ninja %s` from nothing ran the check first"
                          % target))


def lto_tree(p, results):
    p.configure(DECLARED="4096", FIX_LTO="max")
    rc, out = p.ninja()
    expect(rc == 0 and ran_check(out), "ltrans_drop: the max link failed",
           out)
    many = sorted(r for r in p.records() if ".ltrans" in r)
    expect(len(many) >= 2, "ltrans_drop: -flto-partition=max wrote %d "
                           "partition records" % len(many), out)
    p.configure(DECLARED="4096", FIX_LTO="one")
    rc, out = p.ninja()
    expect(rc == 0 and ran_check(out) and os.path.exists(p.stamp),
           "ltrans_drop: max -> one should still pass", out)
    one = sorted(r for r in p.records() if ".ltrans" in r)
    expect(len(one) == 1, "ltrans_drop: %d partition records after the "
                          "one-partition link: %s" % (len(one), one), out)
    results.append(("ltrans_drop", "%d partition records -> 1, and the check "
                                   "passes" % len(many)))


def cost_tree(p, results):
    """DECLARED -> the policy TU's compile and the asset's command line."""
    nm = p.cc[:-len("gcc")] + "nm"
    seen = []
    for declared in (4096, 5000):
        p.configure(DECLARED=str(declared))
        rc, out = p.ninja("shell", "asset-cost")
        expect(rc == 0, "cost_wiring: build at DECLARED %d failed" % declared,
               out)
        objs = [os.path.join(r, f) for r, _, fs in os.walk(p.bld)
                for f in fs if f.startswith("policy.c.") and
                f.endswith((".o", ".obj"))]
        expect(len(objs) == 1, "cost_wiring: policy.c object not found: %r"
               % objs)
        r = subprocess.run([nm, "-S", objs[0]], capture_output=True,
                           text=True)
        m = re.search(r"^[0-9a-f]+ ([0-9a-f]+) \S+ policy_cost_probe$",
                      r.stdout, re.M)
        expect(m is not None, "cost_wiring: no probe in policy.c.o", r.stdout)
        fw = int(m.group(1), 16)
        with open(p.path("asset_cost.txt")) as fh:
            asset = int(fh.read().strip())
        expect(fw == declared and asset == declared,
               "cost_wiring: DECLARED %d, but the policy TU was compiled with "
               "%d and the asset was handed %d" % (declared, fw, asset))
        seen.append(declared)
    results.append(("cost_wiring", "DECLARED %s reached the policy TU and "
                                   "the asset, and followed the change"
                                   % " -> ".join(map(str, seen))))

    # [!] AN OVERRIDE THAT COMPILES.  A -D later on the command line than the
    # helper's wins with only a warning, so the image carries c = 1 while the
    # build checked 5000.  Nothing may be delivered from it: the check reads the
    # policy back from shell.elf and refuses, and neither flash nor an asset
    # proceeds.
    for f in ("flashed", "packed.nnc"):
        p.rm(f)
    p.configure(DECLARED="5000",
                CMAKE_C_FLAGS=p.c_init + " -DPLUGIN_VENEER_BASE_COST=1u")
    rc, out = p.ninja("shell")
    expect(rc == 0, "cost_override: the overridden image should still link",
           out)
    for target, made in (("flash", "flashed"), ("asset-packed", "packed.nnc")):
        rc, out = p.ninja(target)
        flat = " ".join(out.split())
        expect(rc != 0 and not os.path.exists(p.path(made))
               and "check_policy_probe: FAIL" in flat
               and "its veneer_cost is 1 B" in flat
               and "DECLARED 5000 B" in flat,
               "cost_override: `ninja %s` should fail in check_policy_probe"
               % target, out)
    expect(not os.path.exists(p.stamp),
           "cost_override: a stamp survived the refused probe")
    results.append(("cost_override", "CMAKE_C_FLAGS -DPLUGIN_VENEER_BASE_COST="
                                     "1u links, and the probe stops flash and "
                                     "the asset: " + next(
                                         ln.strip() for ln in out.splitlines()
                                         if "check_policy_probe: FAIL" in ln)
                                     [:110] + "..."))

    # And back: the same tree without the override passes again.
    p.configure(DECLARED="5000", CMAKE_C_FLAGS=p.c_init)
    rc, out = p.ninja("flash")
    expect(rc == 0 and "check_policy_probe: OK" in out,
           "cost_override: removing the override should pass again", out)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--cc", required=True,
                    help="the pinned arm-none-eabi-gcc")
    ap.add_argument("--cmake-dir", default=os.path.join(REPO, "cmake"),
                    help="where the helper and scripts under test are "
                         "(a mutated copy, to test these tests)")
    args = ap.parse_args()
    args.cc = os.path.abspath(args.cc)
    if not shutil.which("ninja"):
        print("run_veneer_gate_build_tests: FAILED -- ninja not found",
              file=sys.stderr)
        return 1

    print("run_veneer_gate_build_tests:")
    results, bad = [], 0
    runs = [
        ("main", lambda p: main_tree(p, results)),
        ("asset_alone", lambda p: asset_tree(p, "asset-bare", "bare.nnc",
                                             results, "asset_alone")),
        ("asset_packed", lambda p: asset_tree(p, "asset-packed",
                                              "packed.nnc", results,
                                              "asset_packed")),
        ("ltrans_drop", lambda p: lto_tree(p, results)),
        ("cost_wiring", lambda p: cost_tree(p, results)),
    ]
    for name, fn in runs:
        with tempfile.TemporaryDirectory() as work:
            try:
                fn(Project(work, args.cmake_dir, args.cc))
            except Fail as e:
                bad += 1
                print("  FAIL %-15s %s" % (name, e))
    for name, why in results:
        print("  ok   %-15s %s" % (name, why))
    if bad:
        print("run_veneer_gate_build_tests: FAILED", file=sys.stderr)
        return 1
    print("run_veneer_gate_build_tests: the gate cannot be built around")
    return 0


if __name__ == "__main__":
    sys.exit(main())
