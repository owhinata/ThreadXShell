#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 ThreadX Shell Project
"""Negative tests for cmake/check_lease_entries.py on this board's nn_active.c.

[!] The gate is seen to fail, for the two spellings a source-text match missed
(the review of issue #130 step 4): a definition whose return type is on the
line above, and one behind __attribute__.  Each is appended to a copy of
port/nn/nn_active.c, compiled like the original, and must be refused BY NAME
-- a refusal for any other reason is a failure too.

Moved out of test/host_tests.sh in issue #205 so that the line saying it ran
comes from the code that made the decisions, not from an echo in the board
script.

Usage:
  test_lease_entries.py --check cmake/check_lease_entries.py \\
      --source port/nn/nn_active.c --out <dir> --cflags "<flags>" \\
      -- <the checker's other arguments: the rest of --obj, and --test ...>

The arguments after `--` follow the negative object on the checker's command
line (so they start with the remaining objects of `--obj`).
"""

import argparse
import os
import shlex
import shutil
import subprocess
import sys

# The appended definitions, by name.  Each must be named in the refusal.
CASES = (
    ("two_line",
     "int\nnn_active_neg_two_line(void)\n{\n\treturn 0;\n}\n"),
    ("attribute",
     "__attribute__((noinline)) int nn_active_neg_attribute(void)\n"
     "{\n\treturn 0;\n}\n"),
)

# issue #205 -- the last line every negative-test runner prints when it passes:
# this file's path from the repository root and how many cases this run decided.
# The count goes up where a verdict is reached, not from the length of a list.
decided = 0


def negative_tests_ok():
    me = os.path.realpath(__file__)
    root = me
    for _ in range(4):          # boards/<board>/test/<this file>
        root = os.path.dirname(root)
    print("negative-tests: %s OK (%d cases)"
          % (os.path.relpath(me, root), decided))


def main():
    global decided
    argv = sys.argv[1:]
    rest = []
    if "--" in argv:
        i = argv.index("--")
        argv, rest = argv[:i], argv[i + 1:]
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--check", required=True,
                    help="cmake/check_lease_entries.py")
    ap.add_argument("--source", required=True,
                    help="the translation unit the entries are appended to")
    ap.add_argument("--out", required=True,
                    help="where the copies, objects and outputs are written")
    ap.add_argument("--cc", default="gcc")
    ap.add_argument("--cflags", default="",
                    help="the compile flags, as the board script spells them")
    args = ap.parse_args(argv)

    for name, text in CASES:
        src = os.path.join(args.out, "lease_neg_%s.c" % name)
        obj = os.path.join(args.out, "lease_neg_%s.o" % name)
        log = os.path.join(args.out, "lease_neg_%s.txt" % name)
        shutil.copyfile(args.source, src)
        with open(src, "a") as fh:
            fh.write(text)
        subprocess.run([args.cc] + shlex.split(args.cflags)
                       + ["-c", src, "-o", obj], check=True)
        r = subprocess.run([sys.executable, args.check, "--obj", obj] + rest,
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                           text=True)
        with open(log, "w") as fh:
            fh.write(r.stdout)
        decided += 1
        if r.returncode == 0:
            print("check_lease_entries: FAIL -- the %s entry was not refused"
                  % name, file=sys.stderr)
            return 1
        if ("nn_active_neg_%s is defined but neither walked" % name
                not in r.stdout):
            print("check_lease_entries: FAIL -- the %s entry was refused for "
                  "some other reason:" % name, file=sys.stderr)
            sys.stderr.write(r.stdout)
            return 1
        print("check_lease_entries: refuses an unwalked entry (%s spelling)"
              % name)
    negative_tests_ok()
    return 0


if __name__ == "__main__":
    sys.exit(main())
