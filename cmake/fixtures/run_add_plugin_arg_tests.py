#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 ThreadX Shell Project
"""Negative tests for add_plugin()'s board arguments (issue #108).

Since #108 the plugin image gate is shared, and the three facts that differ per
board -- the reservation the gate checks against, the forbidden table and the
stack the BASE spends behind a veneer -- reach it only as add_plugin()
arguments, as does the target word it checks the image against.  The value of that shape is entirely in the refusal: a second board
that leaves one out must fail at configure, not inherit the first board's
number.  So the refusal is what is tested, through a real `cmake` configure of a
small project that includes the real helpers -- no cross toolchain, no board, a
fraction of a second per case.

Since issue #112 add_plugin() also requires the FIRMWARE side of the veneer
cost to be registered (cmake/veneer_cost_gate.cmake): a plugin's charge that
nothing checks against the shipped firmware is refused at configure.  So every
case registers a real gate -- against a host-compiled dummy executable, which is
why the project enables C -- and the cases below also cover that registration's
own refusals, and the check that the value it verifies is the value every
add_plugin() was charged with.

[!] THE CONTROL CASE IS A PASS.  Every refusal below differs from it by one
argument, so a refusal proves that argument was the reason -- rather than the
harness being unable to configure anything at all, which would make every case
"refuse" and every case look fine.
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
# Where the helpers under test are; --cmake-dir points it at a mutated copy.
CMAKE_DIR = os.path.join(REPO, "cmake")

# A complete call.  The plugin name has to be a real one: add_plugin() resolves
# the plugin's sources from where they live, and refuses a path that does not.
FULL = {
    "CFLAGS": "-Os",
    "ARCH_FLAGS": "-mthumb",
    "MEMORY_LD": '"${CMAKE_CURRENT_LIST_DIR}/mem.ld"',
    "IMAGE_BASE": "0x24048000",
    "IMAGE_END": "0x24050000",
    "FORBIDDEN": "HAL_FLASH_Program",
    "VENEER_BASE_COST": "256",
    "TARGET_ID": "0x1201",
    "OUT_DIR": '"${CMAKE_BINARY_DIR}/plugin"',
    "OUT_VAR": "ELFS",
    "ENTRIES": "pl_entry=64 pl_sbuf_write=64",
}

# A complete gate registration, as a board makes it.  Its DECLARED matches
# FULL's VENEER_BASE_COST: the two are one fact.
GATE = {
    "FIRMWARE": "fw",
    "MAP": '"${CMAKE_BINARY_DIR}/fw.map"',
    "DECLARED": "256",
    "ROOTS": ("pl_base_log=f pl_base_to_frame=f pl_paint_rect=f "
              "pl_paint_fill_rect=f pl_paint_blit=f pl_print_write=f"),
    "PREBUILT_ROOTS": '"${CMAKE_BINARY_DIR}/prebuilt"',
    "DELIVERY": "fake_flash",
}

# (name, argument to drop or None, override dict, expected substring or None)
# -- add_plugin()'s own arguments, with the gate registered as a board would.
CASES = [
    ("control", None, {}, None),
    ("no_veneer_cost", "VENEER_BASE_COST", {}, "VENEER_BASE_COST is required"),
    ("no_base", "IMAGE_BASE", {}, "IMAGE_BASE is required"),
    ("no_end", "IMAGE_END", {}, "IMAGE_END is required"),
    ("no_forbidden", "FORBIDDEN", {}, "FORBIDDEN is required"),
    ("no_target_id", "TARGET_ID", {}, "TARGET_ID is required"),
    # Present but not a cost: the analysis would assume the base is free.
    ("zero_veneer_cost", None, {"VENEER_BASE_COST": "0"},
     "VENEER_BASE_COST must be a positive byte count"),
    # A variable that was never set expands to nothing and trips the presence
    # check; one that holds a NAME rather than a number must trip this.
    ("symbolic_base", None, {"IMAGE_BASE": "PLUGIN_BASE"},
     "IMAGE_BASE must be a hex number"),
    # issue #112: the plugin's own printer bound is part of what the veneer
    # cost must cover; a plugin that states none cannot be checked.
    ("no_printer_limit", None, {"ENTRIES": "pl_entry=64"},
     "ENTRIES has no pl_sbuf_write"),
]

# -- the firmware-side registration (issue #112).
# (name, gate override dict or None to register no gate, add_plugin override,
#  register twice?, expected substring or None)
GATE_CASES = [
    ("gate_control", {}, {}, False, None),
    # [!] THE REFUSAL #112 EXISTS FOR: a plugin charged a cost nothing checks.
    ("no_gate", None, {}, False,
     "has not registered the firmware side of the veneer cost"),
    # [!] AND THE ONE DECLARATION: the checked value is the charged value.
    ("cost_mismatch", {"DECLARED": "300"}, {}, False,
     "is charged VENEER_BASE_COST 256, but the firmware is checked against "
     "300"),
    ("gate_no_roots", {"ROOTS": None}, {}, False, "ROOTS is required"),
    ("gate_no_delivery", {"DELIVERY": None}, {}, False,
     "DELIVERY is required"),
    ("gate_no_prebuilt", {"PREBUILT_ROOTS": None}, {}, False,
     "PREBUILT_ROOTS is required"),
    ("gate_zero_declared", {"DECLARED": "0"}, {"VENEER_BASE_COST": "0"}, False,
     "DECLARED must be a positive byte count"),
    ("gate_bad_root", {"ROOTS": "pl_base_log"}, {}, False,
     "ROOTS takes VENEER=FUNCTION"),
    ("gate_no_firmware", {"FIRMWARE": "no_such_fw"}, {}, False,
     "FIRMWARE 'no_such_fw' is not a target"),
    ("gate_firmware_not_exe", {"FIRMWARE": "fake_flash"}, {}, False,
     "is a UTILITY, not the executable that ships"),
    ("gate_delivery_missing", {"DELIVERY": "no_such_flash"}, {}, False,
     "DELIVERY 'no_such_flash' is not a target"),
    ("gate_twice", {}, {}, True, "already registered"),
]


def configure(work, args, gate=GATE, plugin=True, twice=False):
    src = os.path.join(work, "src")
    os.makedirs(src)
    with open(os.path.join(src, "mem.ld"), "w") as fh:
        fh.write("MEMORY { PLUGIN (rwx) : ORIGIN = 0x24048000, "
                 "LENGTH = 32K }\n")
    with open(os.path.join(src, "fw.c"), "w") as fh:
        fh.write("int main(void) { return 0; }\n")
    call = "\n".join("    %s %s" % (k, v) for k, v in args.items())
    reg = ""
    if gate is not None:
        gcall = "\n".join("    %s %s" % (k, v) for k, v in gate.items()
                          if v is not None)
        reg = ('include("%s")\n' % os.path.join(CMAKE_DIR,
                                                "veneer_cost_gate.cmake")
               + "veneer_cost_gate(\n%s)\n" % gcall) * (2 if twice else 1)
    with open(os.path.join(src, "CMakeLists.txt"), "w") as fh:
        fh.write("cmake_minimum_required(VERSION 3.20)\n"
                 "project(add_plugin_args C)\n"
                 "set(Python3_EXECUTABLE python3)\n"
                 "add_executable(fw fw.c)\n"
                 "add_custom_target(fake_flash)\n"
                 + reg
                 + 'include("%s")\n' % os.path.join(CMAKE_DIR,
                                                    "add_plugin.cmake")
                 + ("add_plugin(cifar10\n%s)\n" % call if plugin else ""))
    r = subprocess.run(["cmake", "-S", src, "-B", os.path.join(work, "b")],
                       capture_output=True, text=True)
    return r.returncode, r.stdout + r.stderr


def wired(work):
    """[!] WHAT add_plugin() RECORDED MUST REACH THE CHECK'S COMMAND LINE.  The
    refusals above are configure-time; the printer bound and the declaration
    are consumed only when the check runs, so a helper that recorded them and
    never passed them on would configure every case correctly.  The generated
    build files carry the command: look for both there."""
    text = ""
    for root, _, files in os.walk(os.path.join(work, "b")):
        for f in files:
            try:
                with open(os.path.join(root, f), errors="replace") as fh:
                    text += fh.read()
            except OSError:
                pass
    missing = [w for w in (r"--declared\s+256\b",
                           r"--printer-limit\s+cifar10=64\b")
               if not re.search(w, text)]
    return missing


def header_rebuild(cmake_dir):
    """[!] A SHARED HEADER CHANGES, EVERY PLUGIN OBJECT THAT READ IT REBUILDS.

    Found on the hardware in issue #111: the compile rule depended on its .c
    alone, so bumping PLUGIN_ABI_VERSION in svc/plugin_abi.h recompiled only the
    plugin TUs whose .c had also changed.  plugin_main.o kept comparing the base
    against ABI 1, the packer (which reads the header) stamped ABI 2 into the
    manifest, the device's loader accepted it -- and the plugin refused its own
    entry point.  No gate saw it: every one of them reads the linked image, and
    the image was self-consistent.

    So this BUILDS, on the host compiler: a copy of cmake/, asset/ and svc/ (so
    the header can be touched without touching the tree), the compile edges of
    one plugin, then the header is made newer and the same edges are built
    again.  Every object whose source includes plugin_abi.h must be rewritten.
    plugin_libc.c includes nothing of the ABI and is not required to be.
    Returns an error string, or None."""
    if shutil.which("ninja") is None:
        return "ninja not found"
    with tempfile.TemporaryDirectory() as work:
        repo = os.path.join(work, "repo")
        shutil.copytree(cmake_dir, os.path.join(repo, "cmake"),
                        ignore=shutil.ignore_patterns("__pycache__"))
        shutil.copytree(os.path.join(REPO, "asset"), os.path.join(repo, "asset"),
                        ignore=shutil.ignore_patterns("*.tflite"))
        shutil.copytree(os.path.join(REPO, "svc"), os.path.join(repo, "svc"))
        src = os.path.join(work, "src")
        os.makedirs(src)
        with open(os.path.join(src, "mem.ld"), "w") as fh:
            fh.write("MEMORY { PLUGIN (rwx) : ORIGIN = 0x24048000, "
                     "LENGTH = 32K }\n")
        with open(os.path.join(src, "fw.c"), "w") as fh:
            fh.write("int main(void) { return 0; }\n")
        args = dict(FULL)
        # -fstack-usage as the boards pass it: without it the .su byproducts
        # never appear and ninja reruns every edge, so every case would pass.
        args["CFLAGS"] = '-O1 -ffreestanding -fstack-usage -I "%s" -I "%s"' % (
            os.path.join(repo, "svc"), os.path.join(repo, "asset", "common"))
        call = "\n".join("    %s %s" % (k, v) for k, v in args.items())
        gcall = "\n".join("    %s %s" % (k, v) for k, v in GATE.items())
        with open(os.path.join(src, "CMakeLists.txt"), "w") as fh:
            fh.write("cmake_minimum_required(VERSION 3.20)\n"
                     "project(add_plugin_deps C)\n"
                     "set(Python3_EXECUTABLE python3)\n"
                     "add_executable(fw fw.c)\n"
                     "add_custom_target(fake_flash)\n"
                     'include("%s/cmake/veneer_cost_gate.cmake")\n' % repo
                     + "veneer_cost_gate(\n%s)\n" % gcall
                     + 'include("%s/cmake/add_plugin.cmake")\n' % repo
                     + "add_plugin(cifar10\n%s)\n" % call
                     # As the boards do; without a consumer the rules are
                     # not generated at all.
                     + "add_custom_target(plugin DEPENDS ${ELFS})\n")
        bld = os.path.join(work, "b")
        r = subprocess.run(["cmake", "-G", "Ninja", "-S", src, "-B", bld],
                           capture_output=True, text=True)
        if r.returncode != 0:
            return "configure failed:\n" + (r.stdout + r.stderr)[-800:]
        out = os.path.join(bld, "plugin", "cifar10")
        stems = ("plugin_main", "plugin_base", "plugin_fmt", "plugin_text",
                 "plugin_libc")
        stamps = [os.path.join(out, x + ".audited") for x in stems]

        def build():
            r = subprocess.run(["ninja", "-C", bld] + stamps,
                               capture_output=True, text=True)
            return r.returncode, r.stdout + r.stderr
        rc, log = build()
        if rc != 0:
            return "first build of the compile edges failed:\n" + log[-800:]
        # The control: with nothing changed, nothing is rebuilt -- or the
        # rebuild below would prove nothing about the header.
        rc, log = build()
        if rc != 0 or "no work to do" not in log:
            return "a second build with nothing changed did work:\n" + log[-800:]
        before = {x: os.stat(os.path.join(out, x + ".o")).st_mtime_ns
                  for x in stems}
        hdr = os.path.join(repo, "svc", "plugin_abi.h")
        future = max(before.values()) + 10 ** 9
        os.utime(hdr, ns=(future, future))
        rc, log = build()
        if rc != 0:
            return "rebuild failed:\n" + log[-800:]
        stale = [x for x in stems[:4]
                 if os.stat(os.path.join(out, x + ".o")).st_mtime_ns
                 == before[x]]
        if stale:
            return ("svc/plugin_abi.h is newer, but %s.o %s not recompiled "
                    "-- the compile rule does not know the headers it reads"
                    % (".o, ".join(stale), "was" if len(stale) == 1
                       else "were"))
    return None


def judge(name, rc, out, expect, why_ok):
    if expect is None:
        ok = rc == 0
        why = why_ok
    else:
        # cmake wraps a long message across lines: match it as words.
        ok = rc != 0 and " ".join(expect.split()) in " ".join(out.split())
        why = "refused: " + expect
    if ok:
        print("  ok   %-22s %s" % (name, why))
        return 0
    print("  FAIL %-22s rc=%d, expected %s\n%s"
          % (name, rc, expect or "success", out[-800:]))
    return 1


def main():
    global CMAKE_DIR
    ap = argparse.ArgumentParser()
    ap.add_argument("--cmake-dir", default=CMAKE_DIR,
                    help="where add_plugin.cmake and veneer_cost_gate.cmake "
                         "are (a mutated copy, to test these tests)")
    CMAKE_DIR = os.path.abspath(ap.parse_args().cmake_dir)
    if shutil.which("cmake") is None:
        print("run_add_plugin_arg_tests: SKIPPED -- no cmake on PATH",
              file=sys.stderr)
        return 0
    bad = 0
    print("run_add_plugin_arg_tests:")
    for name, drop, override, expect in CASES:
        args = dict(FULL)
        if drop:
            del args[drop]
        args.update(override)
        with tempfile.TemporaryDirectory() as work:
            rc, out = configure(work, args)
        bad += judge(name, rc, out, expect,
                     "configures (every other case differs from this by one "
                     "argument)")
    for name, goverride, poverride, twice, expect in GATE_CASES:
        gate = None
        if goverride is not None:
            gate = dict(GATE)
            gate.update(goverride)
        args = dict(FULL)
        args.update(poverride)
        with tempfile.TemporaryDirectory() as work:
            rc, out = configure(work, args, gate=gate, twice=twice)
            missing = wired(work) if expect is None and rc == 0 else []
        if missing:
            print("  FAIL %-22s configures, but the check's command line "
                  "lacks %s" % (name, missing))
            bad += 1
            continue
        bad += judge(name, rc, out, expect,
                     "configures with the gate registered, and the check "
                     "gets --declared 256 --printer-limit cifar10=64")
    err = header_rebuild(CMAKE_DIR)
    if err:
        print("  FAIL %-22s %s" % ("header_rebuild", err))
        bad += 1
    else:
        print("  ok   %-22s touching svc/plugin_abi.h recompiles plugin_main, "
              "plugin_base, plugin_fmt and plugin_text" % "header_rebuild")
    if bad:
        print("run_add_plugin_arg_tests: FAILED", file=sys.stderr)
        return 1
    print("run_add_plugin_arg_tests: all cases behave as specified")
    return 0


if __name__ == "__main__":
    sys.exit(main())
