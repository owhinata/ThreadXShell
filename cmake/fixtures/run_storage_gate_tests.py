#!/usr/bin/env python3
"""Negative tests for cmake/check_no_mutable_storage.py (issue #97).

A gate nobody has watched fail is not a gate.  This compiles a handful of small
fixtures and asserts what the checker says about each, so that the rule is
exercised rather than assumed.

Three of them are shapes an adversarial review defeated the first version with,
and they are here so the fix stays fixed: thread-local storage and anonymous
inline-asm bytes have no STT_OBJECT symbol, explicit COMMON has no section at
all, and storage behind `__INCLUDE_LEVEL__` exists only when the file is compiled
directly -- which is why the audit compiles the real source rather than a wrapper
that includes it.

The fixtures are held here as source strings rather than as files because they
exist only to be rejected: as files they would be four more things that have to
stay in step with a checker they are not otherwise near.

[!] THE FOURTH ONE IS THE POINT.  `target_only` puts a static behind
`#if defined(__arm__)`.  Run with the host compiler it PASSES -- which is not a
bug in the fixture, it is the demonstration that a host-side storage check is
fail-open for anything a board compiles differently.  Run with a cross compiler
the same source FAILS.  That asymmetry is why check_no_mutable_storage.py is
wired per board and not once in the host test suite.
"""
import argparse
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
CHECKER = os.path.join(HERE, os.pardir, "check_no_mutable_storage.py")

CLEAN = """
int f(int x) { return x + 1; }
static const int table[4] = { 1, 2, 3, 4 };
int g(int i) { return table[i & 3]; }
"""

FIXTURES = [
    ("clean", CLEAN, "pass",
     "code and read-only data only"),
    ("plain_static", CLEAN + "\nstatic int counter;\nint bump(void) { return ++counter; }\n",
     "fail",
     "an ordinary mutable static -- the shape this gate exists for"),
    ("thread_local", CLEAN + "\n_Thread_local int tls;\nint bump(void) { return ++tls; }\n",
     "fail",
     "thread-local storage, which is NOT an STT_OBJECT symbol"),
    ("anon_asm", CLEAN +
     '\n__asm__(".section .bss.anon,\\"aw\\",%nobits\\n.space 64\\n.previous");\n',
     "fail",
     "anonymous writable bytes with no symbol at all"),
    ("explicit_common", CLEAN +
     "\nint shared __attribute__((common));\nint *use(void) { return &shared; }\n",
     "fail",
     "explicit COMMON, which -fno-common does not stop and which has NO section"),
    ("include_level", CLEAN +
     "\n#if __INCLUDE_LEVEL__ == 0\nstatic volatile unsigned hidden;\n"
     "unsigned bump(void) { return ++hidden; }\n#endif\n",
     "fail",
     "storage that exists only when the file is compiled DIRECTLY -- an audit "
     "that #included it would miss this"),
    ("target_only", CLEAN +
     "\n#if defined(__arm__)\nstatic int only_on_target;\n"
     "int bump(void) { return ++only_on_target; }\n#endif\n",
     "arm-only-fail",
     "storage that exists only under __arm__"),
]


def is_cross(cc):
    """Does this compiler define __arm__?"""
    try:
        out = subprocess.run([cc, "-dM", "-E", "-"], input="", check=True,
                             capture_output=True, text=True).stdout
    except (OSError, subprocess.CalledProcessError):
        return False
    return "__arm__" in out


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--cc", required=True)
    ap.add_argument("--objdump", required=True)
    ap.add_argument("--nm")
    ap.add_argument("--cflags", default="",
                    help="architecture flags the compiler needs (e.g. -mcpu=)")
    args = ap.parse_args()

    cross = is_cross(args.cc)
    print("run_storage_gate_tests: %s (%s)" %
          (os.path.basename(args.cc), "cross/arm" if cross else "host"))

    failures = 0
    with tempfile.TemporaryDirectory() as tmp:
        for name, source, want, why in FIXTURES:
            src = os.path.join(tmp, name + ".c")
            obj = os.path.join(tmp, name + ".o")
            with open(src, "w") as fh:
                fh.write(source)

            # -fno-common deliberately NOT passed: the explicit_common fixture
            # exists to show that suppressing tentative definitions is not the
            # same as refusing COMMON, and the gate refuses it either way.
            cmd = [args.cc, "-std=c11", "-O1", "-fno-lto"]
            cmd += args.cflags.split()
            cmd += ["-c", src, "-o", obj]
            try:
                subprocess.run(cmd, check=True, capture_output=True, text=True)
            except subprocess.CalledProcessError as exc:
                print("  FAIL %-14s did not compile: %s" %
                      (name, exc.stderr.strip().splitlines()[:1]))
                failures += 1
                continue

            check = [sys.executable, CHECKER, "--objdump", args.objdump,
                     "--label", name]
            if args.nm:
                check += ["--nm", args.nm]
            check.append(obj)
            done = subprocess.run(check, capture_output=True, text=True)
            rc = done.returncode
            if rc not in (0, 1):
                print("  FAIL %-14s checker itself errored (%d): %s" %
                      (name, rc, done.stderr.strip().splitlines()[:1]))
                failures += 1
                continue

            if want == "arm-only-fail":
                expect_fail = cross
                note = ("rejected on target" if cross else
                        "PASSES on the host -- the blind spot this proves")
            else:
                expect_fail = want == "fail"
                note = "rejected" if expect_fail else "accepted"

            got_fail = rc != 0
            if got_fail == expect_fail:
                print("  ok   %-14s %s (%s)" % (name, note, why))
            else:
                print("  FAIL %-14s expected %s, checker returned %d (%s)" %
                      (name, "rejection" if expect_fail else "acceptance",
                       rc, why))
                failures += 1

    # [!] AND THE CHECKER MUST REFUSE TO RUN HALF OF ITSELF.  Its COMMON pass
    # needs nm, and an earlier version turned an nm that would not execute into
    # "no COMMON symbols found" -- a clean OK about something never looked at,
    # which is the failure mode this whole gate exists to prevent, reproduced
    # inside the gate.  Making the argument required did not make its execution
    # required.
    with tempfile.TemporaryDirectory() as tmp:
        src = os.path.join(tmp, "probe.c")
        obj = os.path.join(tmp, "probe.o")
        with open(src, "w") as fh:
            fh.write(CLEAN)
        cmd = [args.cc, "-std=c11", "-O1", "-fno-lto"] + args.cflags.split()
        cmd += ["-c", src, "-o", obj]
        subprocess.run(cmd, check=True, capture_output=True, text=True)
        rc = subprocess.run(
            [sys.executable, CHECKER, "--objdump", args.objdump,
             "--nm", "/bin/false", "--label", "nm_broken", obj],
            capture_output=True, text=True).returncode
        if rc == 2:
            print("  ok   %-14s reported CANNOT_CHECK, not a pass (a checker that "
                  "cannot run its own COMMON pass must not say OK)" % "nm_broken")
        else:
            print("  FAIL %-14s returned %d; an nm that will not execute must give "
                  "2, never 0" % ("nm_broken", rc))
            failures += 1

    if failures:
        print("run_storage_gate_tests: %d failure(s)" % failures)
        return 1
    print("run_storage_gate_tests: all fixtures behave as specified")
    return 0


if __name__ == "__main__":
    sys.exit(main())
