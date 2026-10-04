#!/usr/bin/env python3
"""Negative tests for cmake/check_f746_layout.py's backend residents (issue #130).

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
FAKE_OBJDUMP = '#!/bin/sh\nfor a; do last=$a; done\ncat "$last.vec"\n'


def run(tmp, case, syms, args):
    stem = os.path.join(tmp, case)
    with open(stem + ".nm", "w") as f:
        for addr, typ, name in syms:
            f.write(f"{addr:08x} {typ} {name}\n")
    with open(stem + ".vec", "w") as f:
        f.write(vectors(syms))
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

    def expect(case, syms, args, rc, *needles):
        got_rc, out = run(tmp, case, syms, args)
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

    if failures:
        print(f"run_layout_tests: {len(failures)} case(s) FAILED: {', '.join(failures)}",
              file=sys.stderr)
        return 1
    print("run_layout_tests: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
