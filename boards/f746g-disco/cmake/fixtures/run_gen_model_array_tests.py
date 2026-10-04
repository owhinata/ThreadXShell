#!/usr/bin/env python3
"""Tests for cmake/gen_model_array.py's re-verification of the pin (issue #130).

The fetch verifies the pinned model when it publishes it; the generator checks
the bytes it is about to emit, so a file changed after the fetch cannot reach
the image.  Each refusal is asserted on its own diagnostic AND on leaving no
output behind -- a stale array from an earlier run must not survive a refusal
either, so the refusal cases start with outputs already present.
"""

import hashlib
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
GEN = os.path.join(os.path.dirname(HERE), "gen_model_array.py")


def main():
    failures = []
    with tempfile.TemporaryDirectory() as tmp:
        model = os.path.join(tmp, "m.tflite")
        blob = bytes(range(256)) * 3 + b"\x01"
        with open(model, "wb") as f:
            f.write(blob)
        good = hashlib.sha256(blob).hexdigest()
        off = hashlib.sha256(blob[:-1] + b"\x02").hexdigest()
        cc, h = os.path.join(tmp, "a.cc"), os.path.join(tmp, "a.h")

        def case(name, extra, want_rc, needle=None):
            for p in (cc, h):              # a previous run's outputs
                with open(p, "w") as f:
                    f.write("stale\n")
            r = subprocess.run([sys.executable, GEN, model, cc, h, "g_sym", *extra],
                               capture_output=True, text=True)
            ok = r.returncode == want_rc
            if want_rc == 0:
                text = open(cc).read() if os.path.exists(cc) else ""
                ok = ok and "g_sym_size = %du;" % len(blob) in text \
                    and "0x00,0x01,0x02" in text and os.path.exists(h)
            else:
                ok = ok and needle in r.stderr \
                    and not os.path.exists(cc) and not os.path.exists(h)
            print(f"  {'ok  ' if ok else 'FAIL'} {name}: rc {r.returncode}"
                  + ("" if ok else f"\n{r.stderr}"))
            if not ok:
                failures.append(name)

        case("pin-matches", ["--sha256", good], 0)
        case("pin-one-byte-off", ["--sha256", off], 1, "is not the pinned content")
        case("pin-hash-empty", ["--sha256", ""], 1, "must be 64 lowercase hex")
        case("pin-hash-missing-value", ["--sha256"], 1, "--sha256 needs a value")
        case("pin-hash-uppercase", ["--sha256", good.upper()], 1, "must be 64 lowercase hex")
        case("pin-hash-short", ["--sha256", good[:63]], 1, "must be 64 lowercase hex")
        case("override-no-check", [], 0)

    if failures:
        print(f"run_gen_model_array_tests: {len(failures)} FAILED: {', '.join(failures)}",
              file=sys.stderr)
        return 1
    print("run_gen_model_array_tests: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
