#!/usr/bin/env python3
#
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 ThreadX Shell Project
#
# Every way into the plugin is walked by a host test, or is named here with the
# reason it needs no lease (issue #130).
#
# The entry checks in port/nn/nn_active.c and port/plugin/plugin_run.c are
# proved by test_nn_active.c and test_plugin_run_lease.c, which call each entry
# without the lease and require NN_ACTIVE_NOT_HELD / PLUGIN_RUN_NOT_HELD.  What
# those tests cannot see is an entry that is not in their table: a mutation that
# deletes a check turns its row red, but a new entry that was never given a check
# has no row to turn (grove-vision-ai-v2's #127 shipped its unload unchecked for
# exactly that reason).  So the list is DERIVED and held against the tests'
# tables in both directions:
#
#   - a function the TU defines must be walked, or named in EXEMPT with why;
#   - a name walked or exempted must still be defined (a stale row proves nothing).
#
# [!] "DEFINES" IS READ FROM THE OBJECT, NOT FROM THE SOURCE.  The first version
# matched definitions with a regular expression and missed a function whose
# return type sat on the line above, and one behind __attribute__ (the review of
# #130 step 4) -- both spellings this tree already uses.  host_tests.sh compiles
# each TU ON ITS OWN with -c, and the list is every external TEXT symbol (`T`)
# `nm --defined-only -g` reports for that object.  Never a linked test binary:
# its stubs would be counted as the board's entries.  Neither TU has an #if
# around a definition, so the host compile defines the same set the firmware's
# does; if one ever gains one, compile it here under the firmware's condition.
#
# Usage: check_lease_entries.py --obj <tu.o>... --test <test.c>...
#        (NM in the environment names nm; default "nm")
import os
import re
import subprocess
import sys

EXEMPT = {
    # reads the slot table to decide whether to take the lease at all; the
    # worker and the panel ask it before they hold anything (Grove's too)
    "nn_active_is_plugin": "the question asked before the lease",
    # the base vtable's transform: called BY the plugin, from inside a callback
    # whose caller already passed an entry check
    "nn_active_to_frame": "called from inside the plugin",
    # a const pointer to a vtable in .rodata; enters nothing
    "nn_active_base": "returns a constant",
    # records a stack depth for the loader's hook; enters nothing
    "plugin_run_note_entry": "records a depth",
    # read by nn_active_is_plugin() and the shared branch under the wrappers
    "plugin_run_active": "a slot-table read",
    "plugin_run_slot": "a slot-table read, under the wrappers' checks",
    # the fault reporter: may take no lock and enters nothing
    "plugin_run_attribute": "reachable from a fault handler",
    "plugin_run_why": "a string",
    "plugin_run_res_base": "a linker constant",
    "plugin_run_res_len": "a linker constant",
}

LIT = re.compile(r'"(\w+)"')


def defined(obj):
    out = subprocess.run([os.environ.get("NM", "nm"), "--defined-only", "-g",
                          obj], check=True, capture_output=True,
                         text=True).stdout
    names = set()
    for line in out.splitlines():
        f = line.split()
        if len(f) == 3 and f[1] == "T":
            names.add(f[2])
    if not names:
        sys.exit("check_lease_entries: FAIL -- no external text symbol in " + obj)
    return names


def walked(path):
    with open(path) as f:
        text = f.read()
    try:
        block = text[text.index("/* LEASE ENTRIES BEGIN */"):
                     text.index("/* LEASE ENTRIES END */")]
    except ValueError:
        sys.exit("check_lease_entries: FAIL -- no LEASE ENTRIES markers in " + path)
    return set(LIT.findall(block))


def main():
    objs, tests, cur = [], [], None
    for a in sys.argv[1:]:
        if a in ("--obj", "--test"):
            cur = objs if a == "--obj" else tests
        elif cur is None:
            sys.exit("usage: check_lease_entries.py --obj <o>... --test <c>...")
        else:
            cur.append(a)
    if not objs or not tests:
        sys.exit("usage: check_lease_entries.py --obj <o>... --test <c>...")
    defs = set()
    for o in objs:
        defs |= defined(o)
    walk = set()
    for t in tests:
        walk |= walked(t)
    bad = []
    for n in sorted(defs - walk - set(EXEMPT)):
        bad.append("%s is defined but neither walked without the lease nor "
                   "exempted" % n)
    for n in sorted((walk | set(EXEMPT)) - defs):
        bad.append("%s is walked or exempted but no longer defined" % n)
    for n in sorted(walk & set(EXEMPT)):
        bad.append("%s is both walked and exempted" % n)
    if not defs or not walk:
        bad.append("nothing found (%d defined, %d walked)" % (len(defs), len(walk)))
    if bad:
        for b in bad:
            print("check_lease_entries: FAIL -- " + b)
        sys.exit(1)
    print("check_lease_entries: OK -- %d entries walked without the lease, "
          "%d exempted, of %d defined" % (len(walk), len(EXEMPT), len(defs)))


if __name__ == "__main__":
    main()
