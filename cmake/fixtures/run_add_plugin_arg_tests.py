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
project(NONE) that includes the real helper -- no toolchain, no board, a
fraction of a second per case.

[!] THE CONTROL CASE IS A PASS.  Every refusal below differs from it by one
argument, so a refusal proves that argument was the reason -- rather than the
harness being unable to configure anything at all, which would make every case
"refuse" and every case look fine.
"""

import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))

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
    "ENTRIES": "pl_entry=64",
}

# (name, argument to drop or None, override dict, expected substring or None)
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
]


def configure(work, args):
    src = os.path.join(work, "src")
    os.makedirs(src)
    with open(os.path.join(src, "mem.ld"), "w") as fh:
        fh.write("MEMORY { PLUGIN (rwx) : ORIGIN = 0x24048000, "
                 "LENGTH = 32K }\n")
    call = "\n".join("    %s %s" % (k, v) for k, v in args.items())
    with open(os.path.join(src, "CMakeLists.txt"), "w") as fh:
        fh.write("cmake_minimum_required(VERSION 3.20)\n"
                 "project(add_plugin_args NONE)\n"
                 "set(Python3_EXECUTABLE python3)\n"
                 'include("%s")\n'
                 "add_plugin(cifar10\n%s)\n"
                 % (os.path.join(REPO, "cmake", "add_plugin.cmake"), call))
    r = subprocess.run(["cmake", "-S", src, "-B", os.path.join(work, "b")],
                       capture_output=True, text=True)
    return r.returncode, r.stdout + r.stderr


def main():
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
        if expect is None:
            ok = rc == 0
            why = "configures (every other case differs from this by one "
            why += "argument)"
        else:
            ok = rc != 0 and expect in out
            why = "refused: " + expect
        if ok:
            print("  ok   %-17s %s" % (name, why))
        else:
            bad += 1
            print("  FAIL %-17s rc=%d, expected %s\n%s"
                  % (name, rc, expect or "success", out[-800:]))
    if bad:
        print("run_add_plugin_arg_tests: FAILED", file=sys.stderr)
        return 1
    print("run_add_plugin_arg_tests: all cases behave as specified")
    return 0


if __name__ == "__main__":
    sys.exit(main())
