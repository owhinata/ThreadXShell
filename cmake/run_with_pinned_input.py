#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 ThreadX Shell Project
"""Check a pinned input's SHA256, then run the command that consumes it (#132).

    run_with_pinned_input.py --sha256 <hex> --file <path> -- <command ...>

cmake/fetch_model.cmake verifies a pinned model when it publishes it.  After
that, ninja reruns only the CONSUMER if the file changes, so a model edited
after its fetch would flow through unverified.  The first consumer therefore
checks the pin again.  Where that consumer is our packer it hashes the bytes it
packs itself (asset/tools/pack_container.py --model-sha256); where it is a tool
that should not have to learn hashing (Grove's tflite_strip_boundary, vela),
this wrapper stands in front of it.

[!] THE CHECK AND THE CONSUMER'S READ ARE ONE BUILD COMMAND, so no ninja edge
lies between them: nothing the build graph can rerun separately sits in the gap.
What remains is a hand edit during the few milliseconds between this hash and
the tool's own open -- not the scenario of issue #132, and not one a hash
outside the consumer could close anyway.

[!] AN EMPTY --sha256 IS A REFUSAL, NOT "NO CHECK".  The override path does not
use this wrapper at all; a pin that reaches it without a hash is a bug.  On a
mismatch nothing is run, so the consumer's output is neither written nor
touched.  Otherwise the command's exit code is this script's.
"""
import hashlib
import os
import sys


def die(msg):
    sys.stderr.write("run_with_pinned_input: %s\n" % msg)
    sys.exit(1)


def sha256_error(path, want):
    """None when the file at `path` hashes to `want`, else the message."""
    if not want:
        return ("pinned input %s: --sha256 is empty (a pin without a hash; the "
                "override path does not use this wrapper)" % path)
    if len(want) != 64 or any(c not in "0123456789abcdef" for c in want):
        return ("pinned input %s: --sha256 must be 64 lowercase hex digits, "
                "got %r" % (path, want))
    try:
        with open(path, "rb") as fh:
            got = hashlib.sha256(fh.read()).hexdigest()
    except OSError as e:
        return "pinned input %s: cannot read it (%s)" % (path, e.strerror)
    if got != want:
        return ("pinned input %s: sha256 mismatch (expected %s, got %s)"
                % (path, want, got))
    return None


def parse(argv):
    """(sha256, file, command) from argv, or die.  Hand-rolled because
    argparse would try to read the consumer's own options."""
    if "--" not in argv:
        die("usage: run_with_pinned_input.py --sha256 <hex> --file <path> "
            "-- <command ...>")
    k = argv.index("--")
    head, cmd = argv[:k], argv[k + 1:]
    opts = {}
    i = 0
    while i < len(head):
        if head[i] not in ("--sha256", "--file") or i + 1 >= len(head):
            die("unexpected argument %r before --" % head[i])
        if head[i] in opts:
            die("%s given twice" % head[i])
        opts[head[i]] = head[i + 1]
        i += 2
    for o in ("--sha256", "--file"):
        if o not in opts:
            die("%s is required" % o)
    if not cmd:
        die("no command after --")
    return opts["--sha256"], opts["--file"], cmd


def main(argv):
    want, path, cmd = parse(argv)
    err = sha256_error(path, want)
    if err:
        die(err + "; the command was not run")
    sys.stdout.flush()
    sys.stderr.flush()
    try:
        os.execvp(cmd[0], cmd)
    except OSError as e:
        die("cannot run %s: %s" % (cmd[0], e.strerror))


if __name__ == "__main__":
    main(sys.argv[1:])
