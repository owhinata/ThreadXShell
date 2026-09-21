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
PLUGIN = os.path.join(REPO, "asset", "plugins", "blazeface")
# The link script, the base veneers and the freestanding libc remnant are shared
# by every plugin (issue #103), so a fixture assembles the two directories the
# real build does.
COMMON = os.path.join(REPO, "asset", "common")
# [!] THE LINK SCRIPT IS TWO FILES SINCE #106.  The SECTIONS half is the ABI's
# and lives beside the plugin sources; the MEMORY half is the board's.  A fixture
# that staged only the shared half would fail to link for a reason that has
# nothing to do with the check under test, so it stages both and writes the same
# absolute-path wrapper the real build generates.
GATE = os.path.join(REPO, "cmake", "check_plugin_image.py")

# The board facts the gate takes as arguments since issue #108, as each board
# states them in its board.cmake.  Only the ones a fixture can tell apart matter
# here: the forbidden table needs just the entry point m_forbidden reaches for.
BOARDS = {
    "grove": {
        "arch": ["-mcpu=cortex-m55", "-mthumb", "-mfloat-abi=hard"],
        "memory_ld": os.path.join(REPO, "boards", "grove-vision-ai-v2",
                                  "ldscript", "plugin_memory.ld"),
        "facts": {"--base": "0x341E0000", "--end": "0x34200000",
                  "--forbid": "hx_lib_qspi_eeprom_write",
                  "--veneer-base-cost": "256", "--target-id": "0x9302"},
    },
}
# Wio Lite AI (issue #108), as its board.cmake states itself to the gate.
BOARDS["wio"] = {
    "arch": ["-mcpu=cortex-m7", "-mthumb", "-mfpu=fpv5-d16",
             "-mfloat-abi=hard"],
    "memory_ld": os.path.join(REPO, "boards", "wio-lite-ai", "ldscript",
                              "plugin_memory.ld"),
    "facts": {"--base": "0x24048000", "--end": "0x24050000",
              "--forbid": "HAL_FLASH_Program",
              "--veneer-base-cost": "256", "--target-id": "0x1201"},
}
# Other cores, linked into Grove's reservation.  Only the TARGET check differs
# between them, so borrowing a reservation that exists keeps every other check
# passing and each refusal below about the one thing it tests.
_GROVE = BOARDS["grove"]
for _name, _arch, _word in (
        ("m7_dp", ["-mcpu=cortex-m7", "-mthumb", "-mfpu=fpv5-d16",
                   "-mfloat-abi=hard"], "0x1201"),
        ("m7_sp", ["-mcpu=cortex-m7", "-mthumb", "-mfpu=fpv5-sp-d16",
                   "-mfloat-abi=hard"], "0x1101"),
        ("m4", ["-mcpu=cortex-m4", "-mthumb", "-mfpu=fpv4-sp-d16",
                "-mfloat-abi=hard"], "0x1101"),
        ("m85", ["-mcpu=cortex-m85", "-mthumb", "-mfloat-abi=hard"],
         "0x1302")):
    BOARDS[_name] = {"arch": _arch, "memory_ld": _GROVE["memory_ld"],
                     "facts": dict(_GROVE["facts"], **{"--target-id": _word})}

BASE_FLAGS = [
    "-Os", "-std=c11", "-ffreestanding", "-fno-builtin", "-fno-common",
    "-ffunction-sections", "-fdata-sections", "-fno-stack-protector",
    "-fstack-usage",
]
NO_UNWIND = ["-fno-unwind-tables", "-fno-asynchronous-unwind-tables"]


def build(cc, nm, objdump, work, board, mutate=None, cflags=None, facts=None):
    """Build a plugin image for `board`, optionally mutated.

    `cflags` replaces the non-architecture flags; `facts` overrides what the
    gate is told.  Returns (rc, output)."""
    b = BOARDS[board]
    src = os.path.join(work, "src")
    shutil.copytree(PLUGIN, src)
    shutil.copytree(COMMON, src, dirs_exist_ok=True)
    shutil.copy(os.path.join(REPO, "svc", "blazeface.c"), src)
    shutil.copy(b["memory_ld"], os.path.join(src, "plugin_memory.ld"))
    if mutate:
        mutate(src)

    # The wrapper the real build generates, with the same absolute INCLUDEs --
    # so a fixture that mutates either half is linking what the board links.
    with open(os.path.join(src, "plugin_link.ld"), "w") as fh:
        fh.write('INCLUDE %s\nINCLUDE %s\n'
                 % (os.path.join(src, "plugin_memory.ld"),
                    os.path.join(src, "plugin.ld")))

    cflags = b["arch"] + (cflags if cflags is not None
                          else BASE_FLAGS + NO_UNWIND) + [
        "-I", os.path.join(REPO, "svc"), "-I", src]
    objs, sus = [], []
    # [!] KEEP THIS IN STEP WITH add_plugin()'s source list.  plugin_text.c
    # arrived with #105 and was added there and NOT here, which nothing noticed
    # for two issues because nothing ran this file (fixed in #106).  A fixture
    # that cannot link is not a fixture that passes -- it is one nobody asked.
    for name in ("plugin_main", "plugin_base", "plugin_fmt", "plugin_libc",
                 "plugin_text", "blazeface"):
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
                        "-T", os.path.join(src, "plugin_link.ld"),
                        "-Wl,--gc-sections", "-Wl,--no-warn-rwx-segments"]
                       + b["arch"] + objs + ["-o", elf],
                       capture_output=True, text=True, cwd=work)
    if r.returncode != 0:
        return 98, "link failed:\n" + r.stderr

    gate_facts = dict(b["facts"])
    gate_facts.update(facts or {})
    r = subprocess.run([sys.executable, GATE, elf, "--nm", nm,
                        "--objdump", objdump, "--su"] + sus
                       + ["--entry", "pl_draw=1024", "pl_decode=8192"]
                       + [x for kv in gate_facts.items() for x in kv],
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


def m_memory_moved(src):
    """[!] THE FRAGMENT AND THE GATE ARE TWO DECLARATIONS, AND THIS IS WHY.  A
    MEMORY fragment that says another address still LINKS -- the plugin is
    simply prelinked for the wrong window -- and the only thing that notices is
    the gate comparing the image with the board's own separate statement of the
    reservation.  Generated from one variable, both would move together and this
    would pass."""
    sub(os.path.join(src, "plugin_memory.ld"),
        "ORIGIN = 0x24048000", "ORIGIN = 0x24040000")


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
#
# Each case: (name, board, mutate, cflags, gate facts, expected, must-say, why).
# `must-say` is a substring the gate's output has to contain -- for an ACCEPT
# that is only honest if the gate says what it did not check.
CASES = [
    ("clean", "grove", None, None, None, "accept", None,
     "accepted (the plugin as built)"),
    ("veneer_bypass", "grove", m_veneer_bypass, None, None, "gate", None,
     "gate: an indirect call outside a veneer -- the stack bound would stop "
     "being a bound"),
    ("forbidden", "grove", m_forbidden, None, None, "link", None,
     "linker: a vendor NOR entry point resolves to nothing in a -nostdlib link"),
    ("undefined", "grove", m_undefined, None, None, "link", None,
     "linker: an unresolved symbol never reaches the gate"),
    ("unwind", "grove", None, BASE_FLAGS + ["-funwind-tables"], None, "link",
     None,
     "linker: unwind tables pull in a personality routine that does not exist"),
    ("recursion", "grove", m_recursion, None, None, "gate", None,
     "gate: a cycle the compiler could not flatten -- no bound exists"),
    ("dup_su", "grove", m_dup_su, None, None, "gate", None,
     "gate: a duplicate record with no static bound is not outvoted by a "
     "larger static one"),
    ("no_frame", "grove", m_no_frame, None, None, "gate", None,
     "gate: a function in the image with no measurement -- still fail-closed "
     "after the clone-name fix"),
    # --- the target word (issue #108) -----------------------------------------
    # A wrong bit the IMAGE can speak for is refused by the gate.  0x9301 claims
    # a Cortex-M7 for an image whose .ARM.attributes say cortex-m55.
    ("target_cpu", "grove", None, None, {"--target-id": "0x9301"}, "gate",
     "does not describe this image",
     "gate: a word claiming the wrong CPU -- derived from the attributes"),
    # [!] AND A WRONG CMSE BIT IS NOT, BY DESIGN -- but the gate must SAY it did
    # not look.  An accept here that stayed silent about it would be the false
    # claim #108 removed from plugin_abi.h, made again in a log line.  The
    # firmware's static assert is the check for this bit (see below).
    ("target_cmse", "grove", None, None, {"--target-id": "0x1302"}, "accept",
     "NOT checked here",
     "accepted, and says the CMSE bit was not checked (no image records it)"),
    # [!] THE M7 RECORDS NO CORE NAME, and the first version of the gate keyed
    # on it (issue #108): it refused every M7 plugin -- the safe direction, but a
    # table nobody had run on an M7 image.  These run it.
    # --- wio (issue #108) -----------------------------------------------------
    ("clean", "wio", None, None, None, "accept", "cortex_m7 / fpv5_d16",
     "accepted (the M7 plugin, against wio's own statement of the reservation)"),
    ("memory_moved", "wio", m_memory_moved, None, None, "gate",
     "is outside the plugin reservation",
     "gate: a MEMORY fragment at another address links; the gate refuses it"),
    ("target_grove", "wio", None, None, {"--target-id": "0x9302"}, "gate",
     "does not describe this image",
     "gate: Grove's word on an M7 image"),
    ("target_m7", "m7_dp", None, None, None, "accept", "cortex_m7 / fpv5_d16",
     "a Cortex-M7 is known by v7E-M + FPv5, not by Tag_CPU_name (\"7E-M\")"),
    ("target_m7_fpu", "m7_dp", None, None, {"--target-id": "0x1101"}, "gate",
     "does not describe this image",
     "gate: a single-precision claim for a double-precision image"),
    ("target_m7_sp", "m7_sp", None, None, None, "accept", "fpv5_sp_d16",
     "fpv5-sp-d16 is told apart by Tag_ABI_HardFP_use"),
    ("target_m4", "m4", None, None, None, "gate",
     "is not a core and FPU any board here builds plugins for",
     "gate: a Cortex-M4 records the same name as an M7; FPv4 is what refuses it"),
    ("target_m85", "m85", None, None, None, "gate", "names its core",
     "gate: an M85 has the M55's arch and FPU; on v8.1-M the NAME decides"),
]


# --- the firmware's half: svc/plugin_target.h (issue #108) --------------------
#
# The same word checked from the other end: a board adapter static-asserts its
# declared word against PLUGIN_TARGET_ID_HERE, derived from the compiler's own
# predefined macros.  Compiled here with each board's REAL firmware flags, so
# what is exercised is the mapping those flags reach, including the two traps
# the header records (__ARM_FP alone, and CMSE as defined() instead of == 3).
# (name, flags, word, expect "ok" | "refuse", why)
FW_M55 = ["-mcpu=cortex-m55", "-mthumb", "-mfloat-abi=hard"]
FW_M7 = ["-mcpu=cortex-m7", "-mthumb", "-mfpu=fpv5-d16", "-mfloat-abi=hard"]
ASSERT_CASES = [
    ("grove_word", FW_M55 + ["-mcmse"], "0x9302", "ok",
     "Grove's firmware flags describe 0x9302"),
    ("grove_no_cmse", FW_M55 + ["-mcmse"], "0x1302", "refuse",
     "a Grove word with the CMSE bit clear -- ONLY this check can see it"),
    ("grove_cpu", FW_M55 + ["-mcmse"], "0x9301", "refuse",
     "a Grove word naming the wrong CPU"),
    ("m85", ["-mcpu=cortex-m85", "-mthumb", "-mfloat-abi=hard", "-mcmse"],
     "0x9302", "refuse",
     "an M85 predefines the M55's arch and FP macros; PACBTI is what refuses it"),
    ("m55_without_mcmse", FW_M55, "0x9302", "refuse",
     "a Cortex-M55 build without -mcmse still predefines __ARM_FEATURE_CMSE "
     "(as 1); the bit must not follow it"),
    ("wio_word", FW_M7, "0x1201", "ok",
     "wio's firmware flags describe 0x1201"),
    ("wio_cmse", FW_M7, "0x9201", "refuse",
     "a wio word with the CMSE bit set -- the part has no Security Extension"),
    ("wio_fpu", FW_M7, "0x1301", "refuse",
     "a wio word naming Grove's FPU: __ARM_FP is 14 on both, __ARM_ARCH is not"),
    ("f746_unmapped", ["-mcpu=cortex-m7", "-mthumb", "-mfpu=fpv5-sp-d16",
                       "-mfloat-abi=hard"], "0x1101", "refuse",
     "f746's (7, 4) is also a Cortex-M4's, so it is refused rather than mapped"),
]


def fw_assert(cc, work, flags, word):
    src = os.path.join(work, "t.c")
    with open(src, "w") as fh:
        fh.write('#include "plugin_target.h"\n'
                 "_Static_assert(%s == PLUGIN_TARGET_ID_HERE, \"word\");\n"
                 % word)
    r = subprocess.run([cc] + flags + ["-std=c11", "-I",
                                       os.path.join(REPO, "svc"),
                                       "-c", src, "-o",
                                       os.path.join(work, "t.o")],
                       capture_output=True, text=True)
    return r.returncode, r.stderr


# --- the third end: the FIRMWARE image (cmake/check_target_word.py) ----------
#
# The macros cannot tell -mcpu=cortex-m85+nopacbti from an M55; the linked image
# can, because v8.1-M records the core's name.  Each case links a small image
# with a board's firmware flags and checks a word against it.
# (name, flags, word, expect "ok" | "refuse", why)
TARGET_WORD = os.path.join(REPO, "cmake", "check_target_word.py")
FW_ELF_CASES = [
    ("grove_image", FW_M55 + ["-mcmse"], "0x9302", "ok",
     "Grove's firmware image describes 0x9302 (CMSE left to the assert)"),
    ("m85_nopacbti_image", ["-mcpu=cortex-m85+nopacbti", "-mthumb",
                            "-mfloat-abi=hard", "-mcmse"], "0x9302", "refuse",
     "an M85 without PACBTI passes the macros; its image names the core"),
    ("wio_image", FW_M7, "0x1201", "ok",
     "wio's firmware image describes 0x1201"),
    ("wio_image_sp", FW_M7, "0x1101", "refuse",
     "a single-precision claim against wio's double-precision image"),
]


def fw_image(cc, work, flags, word):
    src = os.path.join(work, "t.c")
    with open(src, "w") as fh:
        fh.write("int start(void) { return 0; }\n")
    elf = os.path.join(work, "t.elf")
    r = subprocess.run([cc] + flags + ["-nostdlib", "-nostartfiles",
                                       "-Wl,-e,start", "-Wl,-Ttext=0x0",
                                       src, "-o", elf],
                       capture_output=True, text=True)
    if r.returncode != 0:
        return 99, r.stderr
    r = subprocess.run([sys.executable, TARGET_WORD, elf,
                        "--target-id", word], capture_output=True, text=True)
    return r.returncode, r.stdout + r.stderr


# --- the gate's copy of the ABI, pinned against the header --------------------
#
# The gate runs at plugin link time with no host C compiler guaranteed, so it
# carries the enum values it needs as a table.  That is a transcription, and a
# transcription nobody compares is the drift #106 found in this very file.  So
# it is compared: a program built against the REAL svc/plugin_abi.h prints every
# name the table uses, and one disagreement fails the run.
def abi_table_mismatches(work):
    sys.path.insert(0, os.path.dirname(GATE))
    import check_plugin_image as gate          # noqa: E402 -- path set above
    names = sorted(gate.ABI)
    prog = os.path.join(work, "abi.c")
    with open(prog, "w") as fh:
        fh.write('#include <stdio.h>\n#include "plugin_abi.h"\n'
                 "int main(void) {\n")
        for n in names:
            fh.write('  printf("%s %%lu\\n", (unsigned long)(%s));\n' % (n, n))
        fh.write("  return 0;\n}\n")
    exe = os.path.join(work, "abi")
    r = subprocess.run(["gcc", "-std=c11", "-I", os.path.join(REPO, "svc"),
                        prog, "-o", exe], capture_output=True, text=True)
    if r.returncode != 0:
        return ["cannot build the ABI probe:\n" + r.stderr]
    out = subprocess.run([exe], capture_output=True, text=True).stdout
    got = dict(ln.split() for ln in out.splitlines())
    return ["%s: gate says %d, plugin_abi.h says %s" % (n, gate.ABI[n], got[n])
            for n in names if int(got[n]) != gate.ABI[n]]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cc", required=True)
    ap.add_argument("--nm", required=True)
    ap.add_argument("--objdump", required=True)
    args = ap.parse_args()

    print(f"run_plugin_gate_tests: {os.path.basename(args.cc)}")
    bad = 0
    for name, board, mutate, cflags, facts, expect_fail, must_say, why in CASES:
        with tempfile.TemporaryDirectory() as work:
            rc, out = build(args.cc, args.nm, args.objdump, work, board,
                            mutate, cflags, facts)
        got = "link" if rc == 98 else ("build" if rc == 99 else
                                       ("gate" if rc != 0 else "accept"))
        label = f"{board}:{name}"
        if got != expect_fail:
            if got == "build":
                print(f"  FAIL {label:24s} did not compile -- "
                      f"{out.splitlines()[0] if out else ''}")
                bad += 1
                continue
            print(f"  FAIL {label:24s} expected {expect_fail}, got {got}"
                  f"\n        {out.strip()[:300]}")
            bad += 1
        elif must_say and must_say not in out:
            print(f"  FAIL {label:24s} {got} as expected, but the output does "
                  f"not say '{must_say}':\n        {out.strip()[:300]}")
            bad += 1
        else:
            print(f"  ok   {label:24s} {why}")

    for name, flags, word, expect, why in ASSERT_CASES:
        with tempfile.TemporaryDirectory() as work:
            rc, err = fw_assert(args.cc, work, flags, word)
        # [!] A REFUSAL COUNTS ONLY FOR ITS OWN REASON.  Any compile error
        # would otherwise "refuse" -- a missing include would pass every
        # negative case here while testing nothing.
        if rc == 0:
            got = "ok"
        elif ("static assertion failed" in err and '"word"' in err) or \
                "no plugin target is defined" in err:
            got = "refuse"
        else:
            got = "error"
        if got != expect:
            print(f"  FAIL {'firmware:' + name:24s} expected {expect}, got "
                  f"{got}\n        {err.strip()[:300]}")
            bad += 1
        else:
            print(f"  ok   {'firmware:' + name:24s} {why}")

    for name, flags, word, expect, why in FW_ELF_CASES:
        with tempfile.TemporaryDirectory() as work:
            rc, out = fw_image(args.cc, work, flags, word)
        if rc == 0:
            got = "ok"
        elif rc == 1 and "check_target_word: FAIL" in out:
            got = "refuse"
        else:
            got = "error"
        if got != expect:
            print(f"  FAIL {'image:' + name:24s} expected {expect}, got {got}"
                  f"\n        {out.strip()[:300]}")
            bad += 1
        else:
            print(f"  ok   {'image:' + name:24s} {why}")

    with tempfile.TemporaryDirectory() as work:
        mism = abi_table_mismatches(work)
    if mism:
        for m in mism:
            print(f"  FAIL {'abi_table':24s} {m}")
        bad += 1
    else:
        print(f"  ok   {'abi_table':24s} the gate's ABI values match "
              "svc/plugin_abi.h")

    if bad:
        print("run_plugin_gate_tests: FAILED", file=sys.stderr)
        return 1
    print("run_plugin_gate_tests: all fixtures behave as specified")
    return 0


if __name__ == "__main__":
    sys.exit(main())
