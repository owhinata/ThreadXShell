#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 ThreadX Shell Project
"""Host tests for port/npu/nn_plugin_stack.h (issue #119).

WHAT WENT WRONG, AND WHAT THIS HAS TO BE ABLE TO SEE

Until issue #119 this board declared the stack allowance of entry, shapes_ok and
decode against the camera producer's figure -- 4,096 B -- while all three are
also called on a shell thread whose whole stack is 4,096 B.  A plugin declaring
4,096 would have been admitted and overflowed.  Nothing could notice, for two
separate reasons, and this file answers each:

  1. No compile-time check tied an allowance to the threads its slot runs on.
     The header now asserts each slot's allowance below every stack that slot
     runs on, one assert per pair.  An assert nobody has watched fail is not a
     check (issues #42/#66), so every ceiling is driven here at, below and above
     the allowance, WITH THE OTHER CEILINGS OUT OF THE WAY, and the refusal has
     to be EXACTLY the set of asserts this file's own table says run on that
     thread -- one missing is a row deleted from the header, one extra is a
     slot wired to the wrong allowance.  The asserts are the header's: this
     compiles `#include "nn_plugin_stack.h"` and nothing else.  A test that
     restated the comparisons would go on passing with the header's deleted.

  2. Every allowance was 1,024 B, so a slot wired to the wrong one was invisible.
     The runtime half (test_plugin_stack.c) prints the table the firmware builds
     its policy from, compiled with the two allowances set to DIFFERENT sentinel
     values, and it is checked here against this file's own statement of which
     slot runs where.  It also runs the device's validator (svc/plugin_load.c)
     against the REAL allowances: one byte past each limit must be refused.

The real values are read from where the firmware gets them -- board.cmake for
the allowances and the shell stacks, camera.h / cam_lcd_sink.h for the camera
threads -- and each must be found exactly once.  A value this file could not
find is a failure, not a default.

[!] WHAT THIS DOES NOT SEE.  board.cmake states the same slot -> allowance
mapping four more times (the host policy, both plugins' ENTRIES and the compile
definitions); this file checks the firmware's table only.  Nor does it see
nn_svc_grove.c stop using GROVE_PLUGIN_STACK_LIMITS: it tests the header.
"""

import os
import re
import shlex
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
BOARD = os.path.dirname(HERE)
REPO = os.path.dirname(os.path.dirname(BOARD))

INCLUDES = ["-I", os.path.join(BOARD, "port", "npu"),
            "-I", os.path.join(REPO, "shell", "include"),
            "-I", os.path.join(REPO, "svc")]
CFLAGS = shlex.split(os.environ.get("HOST_TEST_CFLAGS", "-std=c11 -Wall -Wextra"))
# -Werror: an "ok" has to mean the header compiled clean, not that a warning
# about one of its comparisons scrolled past.
CFLAGS += ["-Werror"]

# Which threads each slot runs on, and which allowance it is declared against
# -- the table in the header, stated again here ON PURPOSE: this is what the
# header is checked against.
SLOTS = ["entry", "shapes_ok", "decode", "draw", "report", "param_set",
         "param_get"]
THREADS = {"entry": ["console", "bg"], "shapes_ok": ["console", "bg"],
           "decode": ["producer", "console", "bg"], "draw": ["panel"],
           "report": ["console", "bg"], "param_set": ["console", "bg"],
           "param_get": ["console", "bg"]}
EXPECT = {"entry": "SHELL", "shapes_ok": "SHELL", "decode": "SHELL",
          "draw": "PANEL", "report": "SHELL", "param_set": "SHELL",
          "param_get": "SHELL"}

# ceiling -> (the macro that carries it, the allowance its slots take)
CEILINGS = {
    "console":  ("CLI_INSTANCE_STACK_SIZE", "SHELL"),
    "bg":       ("CLI_BG_JOB_STACK_SIZE", "SHELL"),
    "producer": ("CAM_PRODUCER_STACK_BYTES", "SHELL"),
    "panel":    ("CAM_PANEL_STACK_BYTES", "PANEL"),
}


def on(thread):
    """The asserts that must fire when `thread`'s stack is too small."""
    return frozenset("%s < %s" % (s, thread) for s in SLOTS
                     if thread in THREADS[s])


def nonzero(allow):
    """The asserts that must fire when allowance `allow` is 0."""
    return frozenset("%s > 0" % s for s in SLOTS if EXPECT[s] == allow)


def fired(err):
    """Which of the header's asserts refused, by the name each one carries."""
    return frozenset(m.group(1) for ln in err.splitlines()
                     if "static assertion failed" in ln
                     for m in [re.search(r"plugin stack \[([^\]]+)\]", ln)]
                     if m)


def find_one(path, pattern, what):
    """The single value `pattern` captures in `path`, or a failure."""
    with open(path) as fh:
        hits = re.findall(pattern, fh.read(), re.MULTILINE)
    if len(hits) != 1:
        sys.exit("test_plugin_stack: FAILED -- expected exactly one %s in %s, "
                 "found %d (pattern %r)" % (what, os.path.relpath(path, REPO),
                                            len(hits), pattern))
    return int(hits[0])


def real_values():
    cmake = os.path.join(BOARD, "board.cmake")
    cam = os.path.join(BOARD, "port", "camera")
    return {
        "GROVE_PLUGIN_STACK_SHELL": find_one(
            cmake, r"^set\(GROVE_PLUGIN_STACK_SHELL\s+(\d+)\)\s*$",
            "shell allowance"),
        "GROVE_PLUGIN_STACK_PANEL": find_one(
            cmake, r"^set\(GROVE_PLUGIN_STACK_PANEL\s+(\d+)\)\s*$",
            "panel allowance"),
        "CLI_INSTANCE_STACK_SIZE": find_one(
            cmake, r"^\s*CLI_INSTANCE_STACK_SIZE=(\d+)\b", "console stack"),
        "CLI_BG_JOB_STACK_SIZE": find_one(
            cmake, r"^\s*CLI_BG_JOB_STACK_SIZE=(\d+)\b", "job stack"),
        "CAM_PRODUCER_STACK_BYTES": find_one(
            os.path.join(cam, "camera.h"),
            r"^#define\s+CAM_PRODUCER_STACK_BYTES\s+(\d+)u?\s*$",
            "producer stack"),
        "CAM_PANEL_STACK_BYTES": find_one(
            os.path.join(cam, "cam_lcd_sink.h"),
            r"^#define\s+CAM_PANEL_STACK_BYTES\s+(\d+)u?\s*$",
            "panel stack"),
    }


def defines(values):
    return ["-D%s=%su" % (k, v) for k, v in sorted(values.items())
            if v is not None]


def compile_header(work, values, extra=()):
    tu = os.path.join(work, "tu.c")
    with open(tu, "w") as fh:
        fh.write('#include "nn_plugin_stack.h"\n')
    r = subprocess.run(["gcc"] + CFLAGS + ["-fsyntax-only"] + INCLUDES
                       + defines(values) + list(extra) + [tu],
                       capture_output=True, text=True)
    return r.returncode, r.stderr


# --- the compile-time half: each ceiling on its own ------------------------
#
# Two DIFFERENT allowances, so an assert that compared the wrong one is caught
# too: the panel's "equal" case below leaves the shell allowance comfortably
# under it, and the producer's "below" case would trip an assert that compared
# draw's allowance there instead of decode's.
ALLOW = {"SHELL": 1000, "PANEL": 1016}
WIDE = 1 << 16          # a ceiling that is out of the way


def base():
    v = {"GROVE_PLUGIN_STACK_SHELL": ALLOW["SHELL"],
         "GROVE_PLUGIN_STACK_PANEL": ALLOW["PANEL"]}
    for macro, _ in CEILINGS.values():
        v[macro] = WIDE
    return v


def ct_cases(real):
    none = frozenset()
    cases = [("real values", real, [], none, None),
             ("every ceiling wide", base(), [], none, None)]
    for name, (macro, allow) in CEILINGS.items():
        a = ALLOW[allow]
        for label, ceiling, want in (("below", a + 8, none),
                                     ("equal", a, on(name)),
                                     ("above", a - 8, on(name))):
            v = base()
            v[macro] = ceiling
            cases.append(("%s %s (%s %d, allowance %d)"
                          % (name, label, macro, ceiling, a), v, [], want,
                          None))
    for allow in ("SHELL", "PANEL"):
        v = base()
        v["GROVE_PLUGIN_STACK_" + allow] = 0
        cases.append(("%s allowance 0" % allow.lower(), v, [],
                      nonzero(allow), None))
    v = base()
    cases.append(("the retired producer allowance", v,
                  ["-DGROVE_PLUGIN_STACK_PRODUCER=4096u"], None,
                  "was retired by issue #119"))
    v = base()
    v["CAM_PRODUCER_STACK_BYTES"] = None
    cases.append(("a ceiling the includer did not bring", v, [], None,
                  "include camera.h and cam_lcd_sink.h"))
    return cases


def run_ct(real):
    bad = 0
    for what, values, extra, want, must_say in ct_cases(real):
        with tempfile.TemporaryDirectory() as work:
            rc, err = compile_header(work, values, extra)
        names = fired(err)
        # [!] A REFUSAL COUNTS ONLY FOR ITS OWN REASONS.  Any compile error
        # would otherwise "refuse" -- a broken include would pass every
        # negative case while testing nothing.  So a static-assert case must
        # fail with exactly the named asserts and no other error line, and an
        # #error case must say what it was asked to.
        other = [ln for ln in err.splitlines() if "error:" in ln
                 and "static assertion failed" not in ln]
        if must_say is not None:
            ok = rc != 0 and "#error" in err and must_say in err
            said = "refused (#error)" if ok else "not the #error expected"
        elif not want:
            ok = rc == 0
            said = "compiles"
        else:
            ok = rc != 0 and names == want and not other
            said = "refused by exactly %d assert(s)" % len(want)
        if not ok:
            print("  FAIL %-58s" % what)
            if must_say is None:
                print("        wanted %s" % (sorted(want) or "a clean compile"))
                print("        fired  %s" % (sorted(names) or "nothing"))
            for ln in (other or err.strip().splitlines())[:6]:
                print("        " + ln)
            bad += 1
        else:
            print("  ok   %-58s %s" % (what, said))
    return bad


# --- the runtime half: the table, and the validator against it -------------

def run_probe(work, values):
    exe = os.path.join(work, "probe")
    r = subprocess.run(["gcc"] + CFLAGS + INCLUDES + defines(values)
                       + [os.path.join(HERE, "test_plugin_stack.c"),
                          os.path.join(REPO, "svc", "plugin_load.c"),
                          os.path.join(REPO, "svc", "crc32.c"),
                          "-o", exe], capture_output=True, text=True)
    if r.returncode != 0:
        return None, "did not build:\n" + r.stderr
    r = subprocess.run([exe], capture_output=True, text=True)
    table = {}
    for ln in r.stdout.splitlines():
        m = re.match(r"limit (\d+) (\d+)$", ln)
        if m:
            table[SLOTS[int(m.group(1))]] = int(m.group(2))
    return (table if r.returncode == 0 else None), r.stdout


def run_rt(real):
    bad = 0
    # Two sentinel pairs, swapped in order, so a slot that happened to be
    # hard-wired to one of the numbers cannot match both times.
    for label, shell, panel in (("real values", None, None),
                                ("sentinels 1001 / 1013", 1001, 1013),
                                ("sentinels 1019 / 997", 1019, 997)):
        v = dict(real)
        if shell is not None:
            v["GROVE_PLUGIN_STACK_SHELL"] = shell
            v["GROVE_PLUGIN_STACK_PANEL"] = panel
        with tempfile.TemporaryDirectory() as work:
            table, out = run_probe(work, v)
        if table is None:
            print("  FAIL %-58s the validator run failed" % label)
            lines = out.strip().splitlines()
            for ln in [ln for ln in lines if "FAIL" in ln] or lines[:10]:
                print("        " + ln)
            bad += 1
            continue
        refused = sum(1 for ln in out.splitlines() if "PLUGIN_ERR_STACK" in ln
                      or "stack request refused" in ln)
        print("  ok   %-58s one byte past every limit refused (%d refusals)"
              % (label, refused))
        if shell is None:
            continue
        # The mapping, slot by slot, against this file's own table.
        want = {s: v["GROVE_PLUGIN_STACK_" + EXPECT[s]] for s in SLOTS}
        wrong = ["%s=%d (want %s=%d)" % (s, table.get(s, -1), EXPECT[s],
                                         want[s])
                 for s in SLOTS if table.get(s) != want[s]]
        if wrong:
            print("  FAIL %-58s %s" % (label + ": slot -> allowance",
                                       ", ".join(wrong)))
            bad += 1
        else:
            print("  ok   %-58s every slot got the allowance of its threads"
                  % (label + ": slot -> allowance"))
    return bad


def main():
    real = real_values()
    print("test_plugin_stack: allowances shell %d / panel %d; stacks console "
          "%d, job %d, producer %d, panel %d" % (
              real["GROVE_PLUGIN_STACK_SHELL"],
              real["GROVE_PLUGIN_STACK_PANEL"],
              real["CLI_INSTANCE_STACK_SIZE"], real["CLI_BG_JOB_STACK_SIZE"],
              real["CAM_PRODUCER_STACK_BYTES"],
              real["CAM_PANEL_STACK_BYTES"]))
    print("the header's asserts, one ceiling at a time:")
    bad = run_ct(real)
    print("the table the firmware builds its policy from:")
    bad += run_rt(real)
    if bad:
        print("test_plugin_stack: FAILED (%d)" % bad, file=sys.stderr)
        return 1
    print("test_plugin_stack: all cases behave as specified")
    return 0


if __name__ == "__main__":
    sys.exit(main())
