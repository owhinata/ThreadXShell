#!/usr/bin/env python3
"""The length checks of scripts/verify_tflite.cc (issue #194; wio-lite-ai's
cmake/fixtures/run_verify_tflite_size_tests.py, with this board's history).

verify_tflite judges a file's length against the payload of the largest slot of
the SHIPPED blob table (compiled in from src/blob_slots.c) and against the tflm
staging buffer, before it looks at a single byte of the model.  Until #194 the
only limit was the literal 1 MB of the staging buffer, so a file of 1,044,481 ..
1,048,576 B passed here and could not be stored in any slot -- so the cases
below sit on both sides of the slot limit and at the old one.

[!] THE FILES ARE NOT MODELS, ON PURPOSE.  Only the length is under test, and
the length is judged first: a file that passes it goes on to the identifier and
the flatbuffer verifier and is refused THERE, which is the proof that the
length did not stop it.  Each case asserts which line it ends on.  The limits
are written out here, not taken from the tool or the table, so a tool that
drifted would disagree with them.

Run with the configured build tree that has the `verify_tflite` rule; the tool
is brought up to date first, so this never tests yesterday's binary.
"""

import argparse
import os
import subprocess
import sys
import tempfile

SLOT_MAX = 1044480      # a 1 MB slot less its 4 KB header (src/blob_slots.c)
MID_SLOT = 520192       # a 512 KB slot's payload
SMALL_SLOT = 258048     # a 256 KB slot's payload
OLD_LIMIT = 1048576     # the staging buffer, the only limit before #194

REFUSE_SLOT = "exceeds the largest blob slot's payload (%d B)" % SLOT_MAX
LATER = "the flatbuffer is malformed"

# (name, length, the line the run must print, a line it must not print)
CASES = [
    ("fits_1mb_slot", SLOT_MAX, LATER, "exceeds the largest"),
    ("names_1mb_class", MID_SLOT + 1,
     "fits a 1024 KB slot (%d B of payload)" % SLOT_MAX, None),
    ("names_256kb_class", 4096,
     "fits a 256 KB slot (%d B of payload)" % SMALL_SLOT, None),
    ("names_512kb_class", SMALL_SLOT + 1,
     "fits a 512 KB slot (%d B of payload)" % MID_SLOT, None),
    ("one_above_slot_limit", SLOT_MAX + 1, REFUSE_SLOT, LATER),
    # [!] What passed before #194 and no slot could hold.
    ("at_the_old_limit", OLD_LIMIT, REFUSE_SLOT, LATER),
]

bad = 0


def judge(name, ok, detail=""):
    global bad
    print("  %s %-22s %s" % ("ok  " if ok else "FAIL", name, "" if ok else detail))
    if not ok:
        bad += 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--build-dir", required=True)
    args = ap.parse_args()
    print("run_verify_tflite_size_tests:")
    up = subprocess.run(["ninja", "-C", args.build_dir, "verify_tflite"],
                        capture_output=True, text=True)
    if up.returncode != 0:
        sys.stderr.write(up.stdout[-2000:] + up.stderr[-2000:])
        print("run_verify_tflite_size_tests: cannot build verify_tflite",
              file=sys.stderr)
        return 1
    tool = os.path.join(args.build_dir, "verify_tflite")
    with tempfile.TemporaryDirectory() as work:
        for name, n, want, never in CASES:
            path = os.path.join(work, name + ".bin")
            data = bytearray(n)
            data[4:8] = b"TFL3"   # past the identifier check, to the verifier
            with open(path, "wb") as fh:
                fh.write(data)
            r = subprocess.run([tool, path], capture_output=True, text=True)
            out = r.stdout + r.stderr
            ok = r.returncode == 1 and want in out and \
                (never is None or never not in out)
            judge(name, ok, "rc %d: %s" % (r.returncode, out.strip()[-300:]))
    if bad:
        print("run_verify_tflite_size_tests: %d case(s) FAILED" % bad,
              file=sys.stderr)
        return 1
    print("run_verify_tflite_size_tests: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
