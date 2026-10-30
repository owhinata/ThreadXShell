#!/usr/bin/env python3
"""Negative tests for cmake/check_f746_layout.py's backend residents (issue #130)
and its .plugin reservation checks (issue #159).

[!] WHAT THIS PROVES AND WHAT IT DOES NOT.  The gate reads two tool outputs (nm
and `objdump -s -j .isr_vector`) and nothing else, so it is driven here by fake
tools that print a canned listing.  That checks the gate's DECISION on the names
board.cmake actually passes -- refused outside bank3, refused when absent,
required only in the configuration that compiles them.  It does NOT prove that
the compiler spells those names that way; the firmware build does, because a
name that matches nothing in the real image fails the same gate (see the tflm
branch in board.cmake).  The listing's type letters are the ones the pinned nm
prints for the real image.

[!] THE NAMES ARE READ FROM board.cmake, NOT WRITTEN AGAIN HERE.  A copy would
keep passing after board.cmake's spelling changed.  Finding none is a failure.

The control case PASSES; every refusal differs from it in one symbol and is
asserted on its own diagnostic, so a case that starts failing for some other
reason is a test failure rather than a pass.
"""

import os
import re
import stat
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
CMAKE_DIR = os.path.dirname(HERE)
BOARD_DIR = os.path.dirname(CMAKE_DIR)
GATE = os.path.join(CMAKE_DIR, "check_f746_layout.py")
BOARD_CMAKE = os.path.join(BOARD_DIR, "board.cmake")

sys.dont_write_bytecode = True   # importing the gate must not litter cmake/
sys.path.insert(0, CMAKE_DIR)
import check_f746_layout as gate  # noqa: E402  (REQUIRED / HANDLERS / regions)

BANK3 = gate.SDRAM_BANK3[0]


def backend_requires(backend):
    """The --require-sdram-ai names board.cmake adds for one backend."""
    text = open(BOARD_CMAKE).read()
    m = re.search(r'if\(CONFIG_NN_BACKEND STREQUAL "%s"\)(.*?)\nendif\(\)' % backend,
                  text[text.index("set(F746_LAYOUT_REQUIRED"):], re.S)
    if not m:
        sys.exit(f"run_layout_tests: no {backend} block after F746_LAYOUT_REQUIRED "
                 f"in {BOARD_CMAKE}")
    names = re.findall(r"--require-sdram-ai\s+(\S+?)\)?\s", m.group(1) + "\n")
    if not names:
        sys.exit(f"run_layout_tests: the {backend} block names no --require-sdram-ai")
    return names


def base_listing():
    """[(addr, type, name)] for an image that satisfies every fixed check."""
    syms = [
        (0x08000000, "R", "g_pfnVectors"),
        (gate.ESTACK, "R", "_estack"),
        (0x080081D0, "T", "Default_Handler"),
        (0x08000720, "T", "__aeabi_dmul"),
        (0x0807BD9C, "W", "_printf_float"),
        (0x080BC854, "r", "__cli_root_cmds_start"),
        (0x080BC9D4, "r", "__cli_cmd_nn"),
        (0x080BCB24, "r", "__cli_root_cmds_end"),
        (0x200260F0, "b", "nn_dec"),
        (0x200260EC, "b", "nn_dec_ready"),
        (0xC0660000, "b", "nn_dec_scratch"),
        (0x20048000, "B", "__plugin_start"),
        (0x20050000, "B", "__plugin_end"),
        (HEAP_FLOOR, "B", "end"),
    ]
    for i, (name, slot, _why) in enumerate(gate.HANDLERS):
        syms.append((0x08001000 + 0x100 * i, "T", name))
    for i, (name, (start, _end), _where) in enumerate(gate.REQUIRED):
        syms.append((start + 0x40 * i, "b", name))
    return syms


def vectors(syms):
    words = [0] * 128
    words[0] = gate.ESTACK
    for name, slot, _why in gate.HANDLERS:
        addr = next(a for a, t, n in syms if n == name and t == "T")
        words[slot] = addr | 1
    lines = ["", "Contents of section .isr_vector:"]
    for i in range(0, len(words), 4):
        hexes = " ".join(w.to_bytes(4, "little").hex() for w in words[i:i + 4])
        lines.append(f" {0x08000000 + 4 * i:08x} {hexes}  ................")
    return "\n".join(lines) + "\n"


FAKE_NM = '#!/bin/sh\ncat "$1.nm"\n'
# `objdump -h` (sections), `objdump -d -j .text` (the _sbrk disassembly) and
# `objdump -s -j .isr_vector` (the vector table): one canned listing each.
FAKE_OBJDUMP = ('#!/bin/sh\nfor a; do last=$a; done\n'
                'case "$1" in\n'
                '  -h) cat "$last.h" ;;\n'
                '  -d) cat "$last.d" ;;\n'
                '  *)  cat "$last.vec" ;;\n'
                'esac\n')

# --- the .plugin reservation (issue #159) ------------------------------------
# What the pinned objdump prints for the real tflm image, trimmed to the
# sections that matter.  (name, vma, size, flags)
PLUGIN = (".plugin", 0x20048000, 0x8000, "ALLOC")
SECTIONS = [
    (".isr_vector", 0x08000000, 0x1f8, "CONTENTS, ALLOC, LOAD, READONLY, DATA"),
    (".text", 0x08000200, 0xb0000, "CONTENTS, ALLOC, LOAD, READONLY, CODE"),
    (".data", 0x20010000, 0x1130, "CONTENTS, ALLOC, LOAD, DATA"),
    (".bss", 0x20011130, 0x19b24, "ALLOC"),
    (".sram1_dma", 0x2002ac60, 0x1000, "ALLOC"),
    PLUGIN,
    ("._user_heap_stack", 0x2002bc60, 0x600, "ALLOC"),
]
HEAP_FLOOR = 0x2002bc60
# _sbrk's literal pool as the real image has it: its static, _estack, `end`
# and _Min_Stack_Size.
SBRK_CONSTS = (0x20015d40, 0x20048000, HEAP_FLOOR, 0x400)


def objdump_h(sections):
    lines = ["", "shell.elf:     file format elf32-littlearm", "", "Sections:",
             "Idx Name          Size      VMA       LMA       File off  Algn"]
    for i, (name, vma, size, flags) in enumerate(sections):
        lines.append(f" {i:2d} {name:13s} {size:08x}  {vma:08x}  {vma:08x}  "
                     f"00001000  2**2")
        lines.append(f"                  {flags}")
    return "\n".join(lines) + "\n"


def objdump_d(consts, name="_sbrk"):
    lines = ["", "Disassembly of section .text:", "",
             "08010680 <Reset_Handler>:",
             " 8010670:\t4911      \tldr\tr1, [pc, #68]",
             " 8010674:\t20050000 \t.word\t0x20050000", "",
             f"08010680 <{name}>:",
             " 8010680:\t4911      \tldr\tr1, [pc, #68]\t@ (80106c8 <_sbrk+0x48>)",
             " 80106aa:\tbd08      \tpop\t{r3, pc}"]
    for i, c in enumerate(consts):
        lines.append(f" {0x80106c8 + 4 * i:x}:\t{c:08x} \t.word\t0x{c:08x}")
    lines += ["", "080106d8 <_close>:", " 80106d8:\tbd08      \tpop\t{r3, pc}"]
    return "\n".join(lines) + "\n"


def run(tmp, case, syms, args, sections=None, sbrk=SBRK_CONSTS, sbrk_name="_sbrk"):
    stem = os.path.join(tmp, case)
    with open(stem + ".nm", "w") as f:
        for addr, typ, name in syms:
            f.write(f"{addr:08x} {typ} {name}\n")
    with open(stem + ".vec", "w") as f:
        f.write(vectors(syms))
    with open(stem + ".h", "w") as f:
        f.write(objdump_h(SECTIONS if sections is None else sections))
    with open(stem + ".d", "w") as f:
        f.write(objdump_d(sbrk, sbrk_name))
    p = subprocess.run(
        [sys.executable, GATE, "--nm", os.path.join(tmp, "nm"),
         "--objdump", os.path.join(tmp, "objdump"), *args, stem],
        capture_output=True, text=True)
    return p.returncode, p.stdout + p.stderr


def main():
    tflm = backend_requires("tflm")
    # The tflm backend's whole bank3 footprint is these two objects; a block
    # that quietly lost one would still pass every case below for the other.
    for want in ("g_arena", "g_sd_model_buf"):
        if not any(re.search(r"\d%sE$" % want, n) for n in tflm):
            sys.exit(f"run_layout_tests: board.cmake's tflm block no longer "
                     f"requires {want} in bank3 ({tflm})")
    null = backend_requires("null")
    tflm_args = [a for n in tflm for a in ("--require-sdram-ai", n)]
    null_args = [a for n in null for a in ("--require-sdram-ai", n)]
    common = ["--require-sdram-ai", "nn_dec_scratch",
              "--forbid-sdram", "nn_dec", "--forbid-sdram", "nn_dec_ready"]

    failures = []

    def expect(case, syms, args, rc, *needles, **kw):
        got_rc, out = run(tmp, case, syms, args, **kw)
        ok = got_rc == rc and all(n in out for n in needles)
        print(f"  {'ok  ' if ok else 'FAIL'} {case}: rc {got_rc}"
              + ("" if ok else f" (want {rc}, needles {needles})\n{out}"))
        if not ok:
            failures.append(case)

    def tflm_image(where=None, drop=None, rename=None):
        syms = base_listing()
        for i, name in enumerate(tflm):
            if name == drop:
                continue
            addr = where.get(name, BANK3 + 0x60600 + 0x100000 * i) if where else \
                BANK3 + 0x60600 + 0x100000 * i
            syms.append((addr, "b", rename.get(name, name) if rename else name))
        return syms

    with tempfile.TemporaryDirectory() as tmp:
        for tool, body in (("nm", FAKE_NM), ("objdump", FAKE_OBJDUMP)):
            path = os.path.join(tmp, tool)
            with open(path, "w") as f:
                f.write(body)
            os.chmod(path, os.stat(path).st_mode | stat.S_IEXEC)

        print(f"run_layout_tests: tflm residents from board.cmake: {', '.join(tflm)}")
        expect("tflm-control", tflm_image(), common + tflm_args, 0, "OK")
        for name in tflm:
            # Another SDRAM bank: links, every linker ASSERT passes (they bound
            # only section edges), and the gate is the one thing that refuses.
            expect(f"tflm-{name}-in-bank0", tflm_image(where={name: 0xC0170800}),
                   common + tflm_args, 1, f"{name}: {name} is at 0xc0170800, outside")
            expect(f"tflm-{name}-in-bank2", tflm_image(where={name: 0xC0450780}),
                   common + tflm_args, 1, f"{name}: {name} is at 0xc0450780, outside")
            # Gone from the image, or spelled differently (renamed / moved out of
            # the anonymous namespace): a failure, never a vacuous pass.
            expect(f"tflm-{name}-absent", tflm_image(drop=name),
                   common + tflm_args, 1, f"{name}: no such object in the image")
            expect(f"tflm-{name}-renamed", tflm_image(rename={name: name + "_old"}),
                   common + tflm_args, 1, f"{name}: no such object in the image")
            # [!] Two objects with the same name, one in bank3 and one not.
            # Anonymous-namespace names are not unique across files: another .cc
            # defining the same object gets the same mangled name.  Every match
            # must be in the region; one in place must not excuse the other.
            for other, addr in (("bank0", 0xC0170800), ("bank2", 0xC0450780)):
                syms = tflm_image() + [(addr, "b", name)]
                expect(f"tflm-{name}-twice-one-in-{other}", syms,
                       common + tflm_args, 1,
                       f"{name}: {name} is at 0x{addr:08x}, outside")
            # A same-named FUNCTION must not satisfy a data residency requirement.
            syms = [s for s in tflm_image() if s[2] != name] + [(BANK3, "t", name)]
            expect(f"tflm-{name}-as-code", syms, common + tflm_args, 1,
                   f"{name}: no such object in the image")

        # [!] THE SCOPE.  A null image has none of the tflm residents and must
        # pass with the null requirements -- a gate that demanded them there
        # would stop a plain null build, which it does not protect.  And the
        # same image refused under the tflm requirements shows the condition,
        # not the listing, is what spares it.
        null_syms = base_listing() + [(BANK3 + 0x60600, "b", n) for n in null]
        expect("null-control", null_syms, common + null_args, 0, "OK")
        expect("null-image-under-tflm-requires", null_syms, common + tflm_args, 1,
               "no such object in the image")

        # --- the .plugin reservation (issue #159) ----------------------------
        # Every case below differs from tflm-control in ONE fact and is
        # asserted on its own sentence.  The linker ASSERTs refuse most of
        # these shapes before the gate gets a say; the gate is what still
        # holds the day an ASSERT is edited, and the heap ceiling is not a
        # section at all.
        ok_args = common + tflm_args
        img = tflm_image()

        def sections_with(**change):
            out = []
            for sec in SECTIONS:
                if sec[0] in change:
                    if change[sec[0]] is not None:
                        out.append(change[sec[0]])
                else:
                    out.append(sec)
            return out

        expect("res-moved", img, ok_args, 1,
               ".plugin is [0x20040000, 0x20048000); this gate states "
               "[0x20048000, 0x20050000)",
               sections=sections_with(**{".plugin": (".plugin", 0x20040000,
                                                     0x8000, "ALLOC")}))
        expect("res-shrunk", img, ok_args, 1,
               ".plugin is [0x20048000, 0x2004c000)",
               sections=sections_with(**{".plugin": (".plugin", 0x20048000,
                                                     0x4000, "ALLOC")}))
        expect("res-loaded", img, ok_args, 1, ".plugin carries contents",
               sections=sections_with(**{".plugin": (
                   ".plugin", 0x20048000, 0x8000, "CONTENTS, ALLOC, LOAD, DATA")}))
        expect("res-missing", img, ok_args, 1,
               ".plugin: expected exactly one section, found 0",
               sections=sections_with(**{".plugin": None}))
        expect("res-overlap", img, ok_args, 1,
               "section .bss [0x20011130, 0x20048100) overlaps the .plugin "
               "reservation",
               sections=sections_with(**{".bss": (".bss", 0x20011130,
                                                  0x20048100 - 0x20011130,
                                                  "ALLOC")}))
        expect("res-symbol-inside", img + [(0x20049000, "b", "stray_buf")],
               ok_args, 1, "symbol stray_buf at 0x20049000 lies inside")
        expect("res-estack-above",
               [x for x in img if x[2] != "_estack"] +
               [(0x20050000, "R", "_estack")], ok_args, 1,
               "_estack 0x20050000 is above the reservation's base")
        expect("res-sbrk-names-top", img, ok_args, 1,
               "_sbrk names 0x20050000 inside the .plugin reservation",
               sbrk=SBRK_CONSTS + (0x20050000,))
        expect("res-sbrk-no-ceiling", img, ok_args, 1,
               "_sbrk does not name _estack (0x20048000)",
               sbrk=(0x20015d40, HEAP_FLOOR, 0x400))
        expect("res-sbrk-absent", img, ok_args, 1,
               "_sbrk: not in the image", sbrk_name="_sbrk_old")
        expect("res-heap-floor-above",
               [x for x in img if x[2] != "end"] + [(0x20048010, "B", "end")],
               ok_args, 1, "the heap starts at 0x20048010, above its own "
               "ceiling source _estack 0x20048000",
               sbrk=SBRK_CONSTS)

    if failures:
        print(f"run_layout_tests: {len(failures)} case(s) FAILED: {', '.join(failures)}",
              file=sys.stderr)
        return 1
    print("run_layout_tests: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
