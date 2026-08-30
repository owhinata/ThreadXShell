#!/usr/bin/env python3
"""Negative tests for check_plugin_image.py (issue #101).

[!] A GATE NOBODY HAS WATCHED FAIL IS NOT A GATE.  This repo has been here
before: the MVE predication scan of issues #42/#66 passed all seven shapes it
was supposed to catch, because objdump never decoded the instructions it was
grepping for, and nothing ever checked that it could say no.  Every check in
the plugin gate is exercised here from a plugin that is otherwise clean, so a
refusal proves the check ran rather than that the fixture was broken in some
other way.

Three of these fixtures are shapes the gate was ONE TOKEN away from missing
while it was being written:

  veneer_bypass  the first draft matched `bx r[0-9]`, and objdump spells r12 as
                 `ip` -- the painter veneer's tail call was invisible.
  returns_ok     the draft after that flagged `bx lr`, which is a RETURN, and
                 reported forty findings on a clean image.  This fixture is a
                 PASS on purpose: it fails the day someone reintroduces that.
  unwind         -fno-unwind-tables is a compiler flag, so a plugin built
                 without it grows .ARM.exidx and needs an unwinder that does not
                 exist.  Nothing but this check would notice.
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))   # cmake/fixtures -> repo root
PLUGIN = os.path.join(REPO, "boards", "grove-vision-ai-v2", "plugin", "blazeface")
# The link script, the base veneers and the freestanding libc remnant are shared
# by every plugin (issue #103), so a fixture assembles the two directories the
# real build does.
COMMON = os.path.join(REPO, "boards", "grove-vision-ai-v2", "plugin", "common")
GATE = os.path.join(REPO, "boards", "grove-vision-ai-v2", "cmake",
                    "check_plugin_image.py")

BASE_CFLAGS = [
    "-mcpu=cortex-m55", "-mthumb", "-mfloat-abi=hard",
    "-Os", "-std=c11", "-ffreestanding", "-fno-builtin", "-fno-common",
    "-ffunction-sections", "-fdata-sections", "-fno-stack-protector",
    "-fstack-usage",
]
NO_UNWIND = ["-fno-unwind-tables", "-fno-asynchronous-unwind-tables"]


def build(cc, nm, objdump, work, mutate=None, cflags=None):
    """Build a plugin image, optionally mutated.  Returns (rc, output)."""
    src = os.path.join(work, "src")
    shutil.copytree(PLUGIN, src)
    shutil.copytree(COMMON, src, dirs_exist_ok=True)
    shutil.copy(os.path.join(REPO, "svc", "blazeface.c"), src)
    if mutate:
        mutate(src)

    cflags = (cflags if cflags is not None else BASE_CFLAGS + NO_UNWIND) + [
        "-I", os.path.join(REPO, "svc"), "-I", src]
    objs, sus = [], []
    for name in ("plugin_main", "plugin_base", "plugin_fmt", "plugin_libc",
                 "blazeface"):
        obj = os.path.join(work, name + ".o")
        r = subprocess.run([cc] + cflags + ["-c", os.path.join(src, name + ".c"),
                                            "-o", obj],
                           capture_output=True, text=True, cwd=work)
        if r.returncode != 0:
            return 99, "compile failed:\n" + r.stderr
        objs.append(obj)
        sus.append(os.path.join(work, name + ".su"))

    # A fixture may edit the MEASUREMENTS after the compile, which is the only
    # way to produce a .su that disagrees with the image without also changing
    # the image.  `.drop_su` removes a record; `.add_su` appends one.
    drop = os.path.join(src, ".drop_su")
    if os.path.exists(drop):
        with open(drop) as fh:
            names = {ln.strip() for ln in fh if ln.strip()}
        for su in sus:
            with open(su) as fh:
                keep = [ln for ln in fh
                        if ln.split("\t")[0].rsplit(":", 1)[-1] not in names]
            with open(su, "w") as fh:
                fh.writelines(keep)
    add = os.path.join(src, ".add_su")
    if os.path.exists(add):
        with open(add) as fh:
            extra = fh.read()
        with open(sus[0], "a") as fh:
            fh.write(extra)

    elf = os.path.join(work, "plugin.elf")
    r = subprocess.run([cc, "-nostdlib", "-nostartfiles",
                        "-T", os.path.join(src, "plugin.ld"),
                        "-Wl,--gc-sections", "-Wl,--no-warn-rwx-segments",
                        "-mcpu=cortex-m55", "-mthumb", "-mfloat-abi=hard"]
                       + objs + ["-o", elf],
                       capture_output=True, text=True, cwd=work)
    if r.returncode != 0:
        return 98, "link failed:\n" + r.stderr

    r = subprocess.run([sys.executable, GATE, elf, "--nm", nm,
                        "--objdump", objdump, "--su"] + sus
                       + ["--entry", "pl_draw=1024", "pl_decode=8192"],
                       capture_output=True, text=True)
    return r.returncode, r.stdout + r.stderr


def sub(path, old, new):
    with open(path) as fh:
        s = fh.read()
    assert old in s, f"fixture text not found in {path}: {old[:50]}"
    with open(path, "w") as fh:
        fh.write(s.replace(old, new, 1))


def m_veneer_bypass(src):
    sub(os.path.join(src, "plugin_main.c"),
        "pl_paint_rect(paint, &r, PL_RGB565, PL_STROKE);",
        "paint->rect(paint->ctx, &r, PL_RGB565, PL_STROKE);")


def m_forbidden(src):
    """[!] IT HAS TO BE REACHABLE.  An unreferenced function is removed by
    --gc-sections, so a fixture that merely DEFINES a caller produces an image
    byte-identical to the clean one and 'passes' while testing nothing.  The
    call goes inside pl_decode, which the slot table keeps alive."""
    sub(os.path.join(src, "plugin_main.c"),
        "\tpl_ndet = blazeface_decode(",
        "\tif (n == 0xDEADu) (void)hx_lib_qspi_eeprom_write();\n"
        "\tpl_ndet = blazeface_decode(")
    sub(os.path.join(src, "plugin_main.c"),
        "/* ---- state ---",
        "extern int hx_lib_qspi_eeprom_write(void);\n/* ---- state ---")


def m_undefined(src):
    """[!] NOT BY REMOVING memset.  At -Os -ffreestanding this plugin never
    emits a memset call at all -- the earlier measurement came from the
    FIRMWARE's -O3 object, and does not transfer.  Deleting the shim therefore
    produced an identical image.  An undefined symbol has to be introduced by
    calling something that does not exist."""
    sub(os.path.join(src, "plugin_main.c"),
        "\tpl_ndet = blazeface_decode(",
        "\tif (n == 0xDEADu) (void)a_symbol_that_does_not_exist();\n"
        "\tpl_ndet = blazeface_decode(")
    sub(os.path.join(src, "plugin_main.c"),
        "/* ---- state ---",
        "extern int a_symbol_that_does_not_exist(void);\n/* ---- state ---")


def m_no_frame(src):
    """[!] THE CHECK THE CLONE BUG WAS SITTING ON TOP OF (issue #103).

    GCC writes an interprocedural clone as `f.constprop` in the .su file and
    `f.constprop.0` in the ELF, so the analyser looked up a name it would never
    find and refused every image containing one.  Teaching it the second
    spelling is right, and it is exactly the kind of change that can turn a
    fail-closed check into a fail-open one.  This fixture keeps the underlying
    rule honest: a function that really has NO measurement -- here because its
    .su line is deleted after the compile -- must still be refused.
    """
    with open(os.path.join(src, ".drop_su"), "w") as fh:
        fh.write("pl_decode\n")


def m_dup_su(src):
    """[!] TWO RECORDS, ONE NAME, AND THE QUALIFIER IS THE EVIDENCE (issue #103).

    Teaching the gate that `f.constprop.0` and `f.constprop` are one body made
    duplicate normalised names possible, and the first merge kept whichever had
    the larger byte count.  A `dynamic` record -- alloca or a VLA, meaning there
    IS no static bound -- would lose to a bigger `static` one and the gate would
    state a bound for a body that has none.

    The .su file is the gate's INPUT, so the fixture writes one: a second record
    for pl_decode, dynamic and deliberately SMALL, so a size-only merge discards
    it.  The image is untouched.
    """
    with open(os.path.join(src, ".add_su"), "w") as fh:
        fh.write("plugin_main.c:0:0:pl_decode\t16\tdynamic\n")


def m_recursion(src):
    """[!] IT MUST BE RECURSION THE COMPILER CANNOT REMOVE.  A tail call was the
    obvious fixture and the wrong one: GCC inlined the helper and turned the
    self-call into a LOOP, so the image contained no cycle, the stack really was
    bounded, and the gate's silence was correct.  `1 +` on the result keeps the
    frame alive across the call, which is what a bound cannot be stated for."""
    sub(os.path.join(src, "plugin_main.c"),
        "\tpl_ndet = blazeface_decode(",
        "\tif (n == 0xDEADu) return pl_deep(outs, n);\n"
        "\tpl_ndet = blazeface_decode(")
    sub(os.path.join(src, "plugin_main.c"),
        "/* ---- state ---",
        "static volatile unsigned pl_sink;\n"
        "static int pl_deep(const struct tensor_desc *o, unsigned n)\n"
        "{ if (!n) return 0; pl_sink = n;\n"
        "  return (int)pl_sink + pl_deep(o, n - 1u); }\n/* ---- state ---")


# Expected outcome per fixture: "accept", "gate" (check_plugin_image refuses) or
# "link" (the linker refuses first, and the gate never gets a say).
#
# [!] THE "link" ONES ARE NOT WEAKER, BUT THEY ARE NOT THE GATE.  Under
# -nostdlib an unresolved symbol -- whether a missing memset or a vendor entry
# point that is nowhere in the image -- is a link error, so the gate's undefined
# and forbidden checks are unreachable in the normal build.  Recording that here
# keeps anyone from reading a passing gate as evidence those checks ran.
CASES = [
    ("clean", None, None, "accept",
     "accepted (the plugin as built)"),
    ("veneer_bypass", m_veneer_bypass, None, "gate",
     "gate: an indirect call outside a veneer -- the stack bound would stop "
     "being a bound"),
    ("forbidden", m_forbidden, None, "link",
     "linker: a vendor NOR entry point resolves to nothing in a -nostdlib link"),
    ("undefined", m_undefined, None, "link",
     "linker: an unresolved symbol never reaches the gate"),
    ("unwind", None, BASE_CFLAGS + ["-funwind-tables"], "link",
     "linker: unwind tables pull in a personality routine that does not exist"),
    ("recursion", m_recursion, None, "gate",
     "gate: a cycle the compiler could not flatten -- no bound exists"),
    ("dup_su", m_dup_su, None, "gate",
     "gate: a duplicate record with no static bound is not outvoted by a "
     "larger static one"),
    ("no_frame", m_no_frame, None, "gate",
     "gate: a function in the image with no measurement -- still fail-closed "
     "after the clone-name fix"),
]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cc", required=True)
    ap.add_argument("--nm", required=True)
    ap.add_argument("--objdump", required=True)
    args = ap.parse_args()

    print(f"run_plugin_gate_tests: {os.path.basename(args.cc)}")
    bad = 0
    for name, mutate, cflags, expect_fail, why in CASES:
        with tempfile.TemporaryDirectory() as work:
            rc, out = build(args.cc, args.nm, args.objdump, work, mutate, cflags)
        got = "link" if rc == 98 else ("build" if rc == 99 else
                                       ("gate" if rc != 0 else "accept"))
        if got != expect_fail:
            if got == "build":
                print(f"  FAIL {name:14s} did not compile -- "
                      f"{out.splitlines()[0] if out else ''}")
                bad += 1
                continue
            print(f"  FAIL {name:14s} expected {expect_fail}, got {got}"
                  f"\n        {out.strip()[:300]}")
            bad += 1
        else:
            print(f"  ok   {name:14s} {why}")

    if bad:
        print("run_plugin_gate_tests: FAILED", file=sys.stderr)
        return 1
    print("run_plugin_gate_tests: all fixtures behave as specified")
    return 0


if __name__ == "__main__":
    sys.exit(main())
