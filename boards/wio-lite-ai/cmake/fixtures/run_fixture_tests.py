#!/usr/bin/env python3
"""Negative tests for cmake/check_boot_safety.py.

WHY REAL IMAGES AND NOT UNIT TESTS
----------------------------------
The obvious negative test -- "point the gate at shell.elf and watch it fail" --
proves almost nothing.  An app image is rejected by C1 on the first check (its
vector table is at 0x08020000), so the option-key scan, the call-graph analysis
and the DFU check are never reached.  The parts of the gate that matter most
would sit permanently unexecuted, free to rot into passing everything.

So most fixtures here are REAL bootloader images: linked against the real ROM
linker script, out of the real bootloader objects, satisfying every check except
the one they are built to break.  Four conditions make that work, and all four
are verified rather than assumed:

  1. they pass C1/C2 -- vectors at 0x08000000, inside the 128 KB sector;
  2. they get past C6b -- an altered image can never match the golden hash, so
     the gate is invoked with the drift override on the COMMAND LINE.  The build
     tree is never configured with -DBOOT_ALLOW_IMAGE_DRIFT=ON, which would leave
     the real escape hatch propped open in the CMake cache;
  3. injected symbols survive --gc-sections -- each fixture names its roots and
     they are passed as -Wl,--undefined=;
  4. the baseline call graph is intact -- the two required edges and the DFU
     symbol are still there, so the fixture actually reaches the check it aims at.

Each fixture is then INDEPENDENTLY inspected (nm, objdump, a raw scan of the
image) before the gate runs on it.  Confirming a fixture's shape with the gate's
own diagnostics would be a circular test: a checker that had lost, say, the
address-equality half of the FLASH_IRQHandler rule would report exactly the same
ID for exactly the wrong reason.

Assertions are on the EXIT CODE and the DIAGNOSTIC ID, never on "non-zero".  A
fixture that starts failing for a different reason than it was built for is a
test failure, not a pass.

The link command and the compile flags are read out of the real build (Ninja and
compile_commands.json) rather than reconstructed here.  A hand-written copy would
drift, and then these fixtures would be testing an image the project does not
build.

Nothing is committed as a build product and nothing is written outside the build
directory: everything lands in <build>/boot-fixtures/.

Usage:
    python3 run_fixture_tests.py --build-dir build/wio-lite-ai \\
                                 --board-dir boards/wio-lite-ai
"""

import argparse
import json
import os
import re
import shlex
import shutil
import struct
import subprocess
import sys

OPT_KEY1 = 0x08192A3B
DBGMCU_BASE = 0x5C001000
FLASH_SLOT = 16 + 4                  # FLASH_IRQn = 4, vectors start at 16
AXI_BASE, AXI_TOP = 0x24000000, 0x24050000
DTCM_BASE, DTCM_TOP = 0x20000000, 0x20020000
WEAK_BINDINGS = ("W", "w", "V", "v")

# Prototypes instead of #include <stm32h7xx_hal.h>: the injected translation
# units only need the linker to resolve these names, and hand-declaring them
# keeps a fixture from breaking when a HAL header moves.
HAL_DECLS = """
extern int HAL_FLASH_Unlock(void);
extern int HAL_FLASH_Program(unsigned int t, unsigned int a, unsigned int d);
extern int HAL_FLASHEx_Erase(void *init, unsigned int *err);
"""


class TestFailure(Exception):
    """The FIXTURE is not what it claims to be -- reported separately from a gate
    verdict mismatch, because the two mean very different things."""


def run(argv, cwd=None, check=True):
    proc = subprocess.run(argv, cwd=cwd, capture_output=True, text=True)
    if check and proc.returncode != 0:
        raise TestFailure(
            f"command failed ({proc.returncode}): {' '.join(argv)}\n{proc.stderr.strip()}"
        )
    return proc


# ===========================================================================
#  Independent inspection helpers -- deliberately NOT the gate's own code
# ===========================================================================
def nm_symbols(nm, elf):
    syms = []
    for line in run([nm, elf]).stdout.splitlines():
        parts = line.split(maxsplit=2)
        if len(parts) == 3 and re.fullmatch(r"[0-9a-fA-F]+", parts[0]):
            syms.append((int(parts[0], 16), parts[1], parts[2]))
    return syms


def symbol(syms, name):
    """(address, binding) of @name, or None."""
    for addr, binding, sym in syms:
        if sym == name:
            return addr, binding
    return None


def raw_offsets(image, value):
    """Every BYTE offset at which @value appears little-endian -- not every fourth."""
    needle = struct.pack("<I", value)
    hits, start = [], image.find(needle)
    while start != -1:
        hits.append(start)
        start = image.find(needle, start + 1)
    return hits


def disassemble(objdump, elf, name):
    return run([objdump, "-d", "--disassemble=" + name, elf]).stdout


# ===========================================================================
#  Per-fixture verification (runs BEFORE the gate, on the fixture image)
# ===========================================================================
def v_optkey_pool(ctx):
    if not raw_offsets(ctx.image, OPT_KEY1):
        raise TestFailure("the option-byte key was optimised out of the image")


def v_optkey_movw(ctx):
    if raw_offsets(ctx.image, OPT_KEY1):
        raise TestFailure(
            "the key is ALSO present as a literal word, so the raw scan would fire "
            "first and the movw/movt path would never be exercised"
        )
    text = disassemble(ctx.objdump, ctx.elf, "fx_build_optkey")
    if "movw" not in text or "movt" not in text:
        raise TestFailure("fx_build_optkey holds no movw/movt pair")


def v_dbgmcu_pool(ctx):
    if not raw_offsets(ctx.image, DBGMCU_BASE):
        raise TestFailure("the DBGMCU base was optimised out of the image")


def v_dbgmcu_movw(ctx):
    if raw_offsets(ctx.image, DBGMCU_BASE):
        raise TestFailure("the DBGMCU base is also a literal word; the movw path is untested")
    text = disassemble(ctx.objdump, ctx.elf, "fx_build_dbgmcu")
    if "movw" not in text or "movt" not in text:
        raise TestFailure("fx_build_dbgmcu holds no movw/movt pair")


def v_symbol_present(name):
    def check(ctx):
        if symbol(ctx.syms, name) is None:
            raise TestFailure(f"{name} is not in the fixture image")

    return check


def v_irq_strong(ctx):
    handler, default = _irq_pair(ctx)
    if handler[1] in WEAK_BINDINGS:
        raise TestFailure("FLASH_IRQHandler is still weak -- the fixture broke nothing")
    if handler[0] & ~1 != default[0] & ~1:
        raise TestFailure("the address predicate is broken too: two violations at once")
    if ctx.vectors[FLASH_SLOT] & ~1 != default[0] & ~1:
        raise TestFailure("the slot predicate is broken too: two violations at once")


def v_irq_weak_other(ctx):
    handler, default = _irq_pair(ctx)
    if handler[1] not in WEAK_BINDINGS:
        raise TestFailure("FLASH_IRQHandler is no longer weak: two violations at once")
    if handler[0] & ~1 == default[0] & ~1:
        raise TestFailure("FLASH_IRQHandler still sits at Default_Handler's address")
    if ctx.vectors[FLASH_SLOT] & ~1 != default[0] & ~1:
        raise TestFailure("the slot predicate is broken too: two violations at once")


def v_irq_slot(ctx):
    handler, default = _irq_pair(ctx)
    if handler[1] not in WEAK_BINDINGS:
        raise TestFailure("FLASH_IRQHandler is no longer weak: two violations at once")
    if handler[0] & ~1 != default[0] & ~1:
        raise TestFailure("the address predicate is broken too: two violations at once")
    if ctx.vectors[FLASH_SLOT] & ~1 == default[0] & ~1:
        raise TestFailure("slot 20 still holds Default_Handler -- the fixture broke nothing")


def _irq_pair(ctx):
    handler = symbol(ctx.syms, "FLASH_IRQHandler")
    default = symbol(ctx.syms, "Default_Handler")
    if handler is None or default is None:
        raise TestFailure("FLASH_IRQHandler or Default_Handler is missing from the image")
    return handler, default


def v_msp_unaligned(ctx):
    if ctx.vectors[0] % 8 == 0:
        raise TestFailure(f"MSP 0x{ctx.vectors[0]:08x} is still 8-byte aligned")


def v_msp_out_of_range(ctx):
    msp = ctx.vectors[0]
    if msp % 8 != 0:
        raise TestFailure("the alignment predicate is broken too: two violations at once")
    if AXI_BASE < msp <= AXI_TOP or DTCM_BASE < msp <= DTCM_TOP:
        raise TestFailure(f"MSP 0x{msp:08x} is still inside an internal RAM")


def v_msp_at_base(ctx):
    if ctx.vectors[0] != AXI_BASE:
        raise TestFailure(f"MSP is 0x{ctx.vectors[0]:08x}, expected the AXI-SRAM base")


def v_calls(function, callee):
    def check(ctx):
        if callee not in disassemble(ctx.objdump, ctx.elf, function):
            raise TestFailure(f"{function} does not reference {callee}")

    return check


def v_tail_call(ctx):
    lines = [
        line
        for line in disassemble(ctx.objdump, ctx.elf, "fx_tail_caller").splitlines()
        if "HAL_FLASH_Unlock" in line
    ]
    if not lines:
        raise TestFailure("fx_tail_caller does not reach HAL_FLASH_Unlock")
    if any(re.search(r"\bbl(\.w)?\s", line) for line in lines):
        raise TestFailure(f"the tail call came out as a bl, proving nothing: {lines}")
    if not any(re.search(r"\bb(\.w)?\s", line) for line in lines):
        raise TestFailure(f"no branch to HAL_FLASH_Unlock: {lines}")


def _fnptr_offsets(ctx):
    erase = symbol(ctx.syms, "HAL_FLASHEx_Erase")
    if erase is None:
        raise TestFailure("HAL_FLASHEx_Erase is not in the image")
    return raw_offsets(ctx.image, erase[0] | 1) + raw_offsets(ctx.image, erase[0] & ~1)


def v_fnptr(ctx):
    if not _fnptr_offsets(ctx):
        raise TestFailure("the function pointer did not survive into the image")


def v_fnptr_unaligned(ctx):
    hits = _fnptr_offsets(ctx)
    if not hits:
        raise TestFailure("the packed function pointer did not survive into the image")
    if all(offset % 4 == 0 for offset in hits):
        raise TestFailure(
            f"every occurrence is 4-aligned ({[hex(h) for h in hits]}); the linker "
            "aligned the packed struct, so the unaligned scan is not being tested"
        )


def _verify_fnptr_movw(ctx):
    """The fixture must defeat BOTH of C4's other mechanisms, or it proves nothing."""
    target = symbol(ctx.syms, "HAL_FLASH_Program")
    if target is None:
        raise TestFailure("HAL_FLASH_Program is not in the image")
    text = disassemble(ctx.objdump, ctx.elf, "fx_indirect_caller")
    body = "\n".join(
        line.split(" @ ", 1)[0] for line in text.splitlines()
    )
    if "HAL_FLASH_Program" in body:
        raise TestFailure(
            "the disassembly still names HAL_FLASH_Program outside a comment, so the "
            "ordinary edge extractor would catch this and the movw/movt path is untested"
        )
    if "movw" not in text or "movt" not in text:
        raise TestFailure("fx_indirect_caller holds no movw/movt pair")
    if raw_offsets(ctx.image, target[0] | 1) or raw_offsets(ctx.image, target[0] & ~1):
        raise TestFailure(
            "the address also exists as a contiguous word, so the raw pointer scan "
            "would fire first"
        )


def v_no_call(function, callee):
    def check(ctx):
        if callee in disassemble(ctx.objdump, ctx.elf, function):
            raise TestFailure(f"{function} still calls {callee}")

    return check


def v_missing_dfu(ctx):
    if symbol(ctx.syms, "tud_dfu_finish_flashing") is not None:
        raise TestFailure("tud_dfu_finish_flashing is still in the image")
    if symbol(ctx.syms, "fx_dfu_finish_flashing") is None:
        raise TestFailure("the renamed symbol is gone too -- the DFU object was dropped")


# ===========================================================================
#  The fixture table
# ===========================================================================
# inject         extra C compiled with the bootloader's own flags and linked in
# roots          symbols kept alive with -Wl,--undefined= (they would otherwise
#                be swept away by --gc-sections, and the fixture would pass)
# source_patch   (path under the board dir, [(regex, replacement)]) -- recompiled
#                and swapped for the original object.  The tree on disk is never
#                touched; the copy lives in the build directory.
# startup_patch  the same, for the CMSIS startup assembly
# redefine_syms  ([object name suffixes], from, to) via objcopy --redefine-sym
# bin_patch      link nothing new; flip one byte of the .bin instead
POSTLINK_FIXTURES = [
    # --- C3: constants the image must not contain --------------------------
    dict(
        name="fx_optkey_pool",
        why="C3 raw scan: the option-byte unlock key in a literal pool",
        expect_exit=1,
        expect_id="BOOT-C3-OPTKEY-RAW",
        inject="""
__attribute__((used)) const unsigned int fx_optkeys[2] = { 0x08192A3Bu, 0x4C5D6E7Fu };
""",
        roots=["fx_optkeys"],
        verify=v_optkey_pool,
    ),
    dict(
        name="fx_optkey_movw",
        why="C3 movw/movt: the same key assembled in a register, invisible to a raw scan",
        expect_exit=1,
        expect_id="BOOT-C3-OPTKEY-MOVW",
        inject="""
__attribute__((used, noinline)) unsigned int fx_build_optkey(void)
{
    unsigned int v;
    /* 0x08192A3B = (0x0819 << 16) | 0x2A3B */
    __asm__ volatile("movw %0, #10811\\n\\tmovt %0, #2073" : "=r"(v));
    return v;
}
""",
        roots=["fx_build_optkey"],
        verify=v_optkey_movw,
    ),
    dict(
        name="fx_dbgmcu_pool",
        why="C3 raw scan: the DBGMCU base as a literal",
        expect_exit=1,
        expect_id="BOOT-C3-DBGMCU-RAW",
        inject="""
__attribute__((used)) volatile unsigned int *const fx_dbgmcu =
    (volatile unsigned int *)0x5C001000u;
""",
        roots=["fx_dbgmcu"],
        verify=v_dbgmcu_pool,
    ),
    dict(
        name="fx_dbgmcu_movw",
        why="C3 movw/movt: the DBGMCU base assembled in a register",
        expect_exit=1,
        expect_id="BOOT-C3-DBGMCU-MOVW",
        inject="""
__attribute__((used, noinline)) unsigned int fx_build_dbgmcu(void)
{
    /* 0x5C001000 = (0x5C00 << 16) | 0x1000 */
    unsigned int v;
    __asm__ volatile("movw %0, #4096\\n\\tmovt %0, #23552" : "=r"(v));
    return v;
}
""",
        roots=["fx_build_dbgmcu"],
        verify=v_dbgmcu_movw,
    ),
    # --- C3: symbols -------------------------------------------------------
    dict(
        name="fx_flash_unknown_sym",
        why="C3 allowlist: the HAL's own option-byte programmer becoming reachable",
        expect_exit=1,
        expect_id="BOOT-C3-FLASH-SYM",
        # No injected code: HAL_FLASHEx_OBProgram is already compiled into
        # boot_image's objects and merely dropped for lack of a caller.  Naming it
        # as a GC root reproduces the real regression exactly.
        roots=["HAL_FLASHEx_OBProgram"],
        verify=v_symbol_present("HAL_FLASHEx_OBProgram"),
    ),
    dict(
        name="fx_dbgmcu_sym",
        why="C3: anything that touches the debug configuration",
        expect_exit=1,
        expect_id="BOOT-C3-DBGMCU-SYM",
        roots=["HAL_DBGMCU_EnableDBGSleepMode"],
        verify=v_symbol_present("HAL_DBGMCU_EnableDBGSleepMode"),
    ),
    dict(
        name="fx_lowpower_sym",
        why="C3: a low-power entry, which can drop SWD to the one remaining board",
        expect_exit=1,
        expect_id="BOOT-C3-LOWPOWER-SYM",
        roots=["HAL_PWR_EnterSTANDBYMode"],
        verify=v_symbol_present("HAL_PWR_EnterSTANDBYMode"),
    ),
    # --- C3: the three FLASH_IRQHandler conditions, one fixture each -------
    # One fixture per predicate.  A single fixture breaking two at once would go
    # green against a checker that had only ever implemented one of them.
    dict(
        name="fx_irq_strong",
        why="C3 handler condition 1: weak binding, and nothing else, is broken",
        expect_exit=1,
        expect_id="BOOT-C3-IRQ-WEAK",
        startup_patch=[(r"\.weak\s+FLASH_IRQHandler", ".globl    FLASH_IRQHandler")],
        verify=v_irq_strong,
    ),
    dict(
        name="fx_irq_weak_other",
        why="C3 handler condition 2: address equality, and nothing else, is broken",
        expect_exit=1,
        expect_id="BOOT-C3-IRQ-ADDR",
        startup_patch=[
            # Aliased to Reset_Handler, not SysTick_Handler: .thumb_set is resolved
            # by the ASSEMBLER, and SysTick_Handler is itself a .thumb_set alias of
            # Default_Handler inside this same file -- so that spelling would have
            # produced Default_Handler's address and broken nothing.
            (
                r"\.thumb_set\s+FLASH_IRQHandler,\s*Default_Handler",
                ".thumb_set FLASH_IRQHandler,Reset_Handler",
            ),
            (r"\.word\s+FLASH_IRQHandler", ".word     Default_Handler"),
        ],
        verify=v_irq_weak_other,
    ),
    dict(
        name="fx_irq_slot_mismatch",
        why="C3 handler condition 3: the vector slot, and nothing else, is broken",
        expect_exit=1,
        expect_id="BOOT-C3-IRQ-SLOT",
        startup_patch=[(r"\.word\s+FLASH_IRQHandler", ".word     SysTick_Handler")],
        verify=v_irq_slot,
    ),
    # --- C1: the initial MSP ----------------------------------------------
    dict(
        name="fx_msp_unaligned",
        why="C1: vector[0] not 8-byte aligned",
        expect_exit=1,
        expect_id="BOOT-C1-MSP-ALIGN",
        startup_patch=[(r"\.word\s+_estack", ".word  _estack - 4")],
        verify=v_msp_unaligned,
    ),
    dict(
        name="fx_msp_out_of_range",
        why="C1: vector[0] outside every internal RAM",
        expect_exit=1,
        expect_id="BOOT-C1-MSP-RANGE",
        startup_patch=[(r"\.word\s+_estack", ".word  0x30000000")],
        verify=v_msp_out_of_range,
    ),
    dict(
        name="fx_msp_at_ram_base",
        why="C1: vector[0] at the RAM base -- a full-descending stack leaves the RAM "
        "on its first push (PM0253 sec 2.1.2), which is why the interval is "
        "start < msp <= end and not [start, end)",
        expect_exit=1,
        expect_id="BOOT-C1-MSP-RANGE",
        startup_patch=[(r"\.word\s+_estack", ".word  0x24000000")],
        verify=v_msp_at_base,
    ),
    # --- C4: the flash-writing call graph ---------------------------------
    dict(
        name="fx_extra_caller",
        why="C4: someone other than iflash_* reaching HAL_FLASH_Program",
        expect_exit=1,
        expect_id="BOOT-C4-EXTRA-CALLER",
        inject="""
__attribute__((used, noinline)) int fx_rogue_writer(unsigned int addr, unsigned int d)
{
    return HAL_FLASH_Program(2u, addr, d);
}
""",
        roots=["fx_rogue_writer"],
        verify=v_calls("fx_rogue_writer", "HAL_FLASH_Program"),
    ),
    dict(
        name="fx_extra_caller_unlock",
        why="C4: the same for HAL_FLASH_Unlock",
        expect_exit=1,
        expect_id="BOOT-C4-EXTRA-CALLER",
        inject="""
__attribute__((used, noinline)) int fx_rogue_unlock(void) { return HAL_FLASH_Unlock(); }
""",
        roots=["fx_rogue_unlock"],
        verify=v_calls("fx_rogue_unlock", "HAL_FLASH_Unlock"),
    ),
    dict(
        name="fx_tail_call",
        why="C4: reached by b.w, not bl -- proof the edge extractor is not an "
        "opcode allowlist",
        expect_exit=1,
        expect_id="BOOT-C4-EXTRA-CALLER",
        inject="""
__asm__(".section .text.fx_tail_caller,\\"ax\\",%progbits\\n"
        ".global fx_tail_caller\\n"
        ".type fx_tail_caller,%function\\n"
        ".thumb_func\\n"
        "fx_tail_caller:\\n"
        "    b.w HAL_FLASH_Unlock\\n"
        ".size fx_tail_caller,.-fx_tail_caller\\n");
""",
        roots=["fx_tail_caller"],
        verify=v_tail_call,
    ),
    dict(
        name="fx_fnptr",
        why="C4: a flash writer's address stored as data.  Exit 2, not 1 -- a data "
        "word does not name who loads it, so nothing is proven, only that the "
        "call graph is no longer the whole story",
        expect_exit=2,
        expect_id="BOOT-C4-FNPTR",
        inject="""
typedef int (*fx_erase_fn)(void *, unsigned int *);
__attribute__((used)) fx_erase_fn const fx_erase_table[1] = { HAL_FLASHEx_Erase };
""",
        roots=["fx_erase_table"],
        verify=v_fnptr,
    ),
    dict(
        name="fx_fnptr_unaligned",
        why="C4: the same word at an odd offset -- the fixture that notices if the "
        "scan ever goes back to stepping four bytes at a time",
        expect_exit=2,
        expect_id="BOOT-C4-FNPTR",
        inject="""
typedef int (*fx_erase_fn)(void *, unsigned int *);
struct __attribute__((packed)) fx_packed { unsigned char pad; fx_erase_fn fn; };
__attribute__((used, aligned(1), section(".rodata.fx_packed")))
const struct fx_packed fx_packed_table = { 0xA5u, HAL_FLASHEx_Erase };
""",
        roots=["fx_packed_table"],
        verify=v_fnptr_unaligned,
    ),
    dict(
        name="fx_fnptr_movw",
        why="C4: the API address assembled by movw/movt and reached indirectly.  No "
        "<symbol> survives in the disassembly and no contiguous pointer word exists "
        "for the raw scan, so both of C4's other mechanisms are blind to it",
        expect_exit=2,
        expect_id="BOOT-C4-FNPTR",
        inject="""
__asm__(".section .text.fx_indirect_caller,\\"ax\\",%progbits\\n"
        ".global fx_indirect_caller\\n"
        ".type fx_indirect_caller,%function\\n"
        ".thumb_func\\n"
        "fx_indirect_caller:\\n"
        "    movw r3, #:lower16:HAL_FLASH_Program\\n"
        "    movt r3, #:upper16:HAL_FLASH_Program\\n"
        "    bx   r3\\n"
        ".size fx_indirect_caller,.-fx_indirect_caller\\n");
""",
        roots=["fx_indirect_caller"],
        verify=lambda ctx: _verify_fnptr_movw(ctx),
    ),
    dict(
        name="fx_missing_edge_erase",
        why="C4: a required edge gone.  Exit 2 -- 'inlined away' and 'deleted' are "
        "indistinguishable in the image, so the answer is 'cannot check'",
        expect_exit=2,
        expect_id="BOOT-C4-MISSING-EDGE",
        source_patch=(
            "boot/iflash.c",
            [
                (
                    r"st = HAL_FLASHEx_Erase\(&erase, &bad_sector\);",
                    "st = (HAL_StatusTypeDef)((unsigned)&erase + (unsigned)&bad_sector);",
                )
            ],
        ),
        verify=v_no_call("iflash_erase_sector", "HAL_FLASHEx_Erase"),
    ),
    dict(
        name="fx_missing_edge_prog",
        why="C4: the other required edge gone",
        expect_exit=2,
        expect_id="BOOT-C4-MISSING-EDGE",
        source_patch=(
            "boot/iflash.c",
            [
                (
                    r"if \(HAL_FLASH_Program\(FLASH_TYPEPROGRAM_FLASHWORD,",
                    "if ((HAL_StatusTypeDef)(",
                )
            ],
        ),
        verify=v_no_call("iflash_program", "HAL_FLASH_Program"),
    ),
    # --- C5 ----------------------------------------------------------------
    dict(
        name="fx_missing_dfu",
        why="C5: a bootloader with no DFU class -- what an -I order regression that "
        "picks up the app's tusb_config.h (CFG_TUD_DFU=0) would produce",
        expect_exit=1,
        expect_id="BOOT-C5-NO-DFU",
        redefine_syms=[
            (
                ["dfu_device.c.obj", "dfu_callbacks.c.obj"],
                "tud_dfu_finish_flashing",
                "fx_dfu_finish_flashing",
            )
        ],
        verify=v_missing_dfu,
    ),
    # --- The ELF <-> bin join ---------------------------------------------
    dict(
        name="fx_object_lto",
        why="the no-LTO guarantee: an object that carries LTO IR while its recorded "
        "compile command has no -flto at all.  This is what a spec file, a compiler "
        "launcher, or an object rebuilt outside Ninja produces -- and the command-line "
        "audit is blind to every one of them",
        expect_exit=1,
        expect_id="BOOT-OBJ-LTO",
        # Uses the healthy ELF and .bin, so every other check passes and this one
        # is the only thing that can be firing.
        lto_object="boot/clock.c",
        verify=None,
    ),
    dict(
        name="fx_elf_bin_mismatch",
        why="the join: a healthy ELF with a substituted .bin.  Without this check it "
        "passes everything under the drift override, because C6b -- the only "
        "check that would have looked at the bytes -- is the one being excused",
        expect_exit=1,
        expect_id="BOOT-BIN-MISMATCH",
        bin_patch=True,
        verify=None,
    ),
]

# Fixtures for the pre-link half.  These need no image at all: they perturb the
# source tree (into a scratch copy -- the real one is never touched) or the
# compile database.
PRECHECK_FIXTURES = [
    dict(
        name="fx_src_tampered",
        why="C6a: a one-character comment edit.  The IMAGE is unchanged, so the "
        "golden hash cannot see this -- which is the whole reason C6a exists",
        expect_exit=1,
        expect_id="BOOT-C6A-HASH",
        tamper=("boot/clock.c", "comment"),
    ),
    dict(
        name="fx_src_added",
        why="C6a: an ADDED file.  Every manifest hash still matches, so a hash list "
        "on its own would wave this through",
        expect_exit=1,
        expect_id="BOOT-C6A-FILESET",
        add_file="boot/unused.c",
        forbid_id="BOOT-C6A-HASH",
    ),
    dict(
        name="fx_rsp_file",
        why="the LTO audit: -flto hidden in a @response file, which GCC expands and "
        "a scan of the command string would not see",
        expect_exit=2,
        expect_id="BOOT-CC-RESPONSE-FILE",
        cc_inject="@fx_flags.rsp",
    ),
    dict(
        name="fx_cc_lto",
        why="the LTO audit itself: without this the gate's LTO rule would never be "
        "executed by any test, since the real build never carries -flto",
        expect_exit=1,
        expect_id="BOOT-CC-LTO",
        cc_inject="-flto=auto",
    ),
    dict(
        name="fx_cc_specs",
        why="a spec file on a bootloader compile line: it rewrites what reaches cc1, so "
        "the command line stops being the thing that decides and the audit must stop",
        expect_exit=2,
        expect_id="BOOT-CC-SPECS",
        cc_inject="-specs=/dev/null",
    ),
    dict(
        name="fx_cc_empty",
        why="the LTO audit cannot be vacuous: with the bootloader's entries gone the "
        "gate must stop, not pass having inspected nothing",
        expect_exit=2,
        expect_id="BOOT-CC-TU-SET",
        cc_drop_boot=True,
    ),
]


class Context:
    """What a fixture's verifier is handed."""

    def __init__(self, elf, image, syms, vectors, objdump):
        self.elf = elf
        self.image = image
        self.syms = syms
        self.vectors = vectors
        self.objdump = objdump


class Runner:
    def __init__(self, build_dir, board_dir):
        self.build = build_dir
        self.board = board_dir
        self.work = os.path.join(build_dir, "boot-fixtures")
        self.checker = os.path.join(board_dir, "cmake", "check_boot_safety.py")
        self.manifest = os.path.join(board_dir, "cmake", "boot_manifest.sha256")
        cache = self._read_cache()
        self.nm = cache["CMAKE_NM"]
        self.objdump = cache["CMAKE_OBJDUMP"]
        self.objcopy = cache["CMAKE_OBJCOPY"]
        self.compile_entries = self._load_compile_entries()
        self.link_flags, self.link_objects = self._load_link_command()
        self._object_dir_override = None
        shutil.rmtree(self.work, ignore_errors=True)
        os.makedirs(self.work)

    # --- reading the real build -------------------------------------------
    def _read_cache(self):
        values = {}
        with open(os.path.join(self.build, "CMakeCache.txt"), encoding="utf-8") as handle:
            for line in handle:
                match = re.match(r"^([A-Za-z0-9_]+):[A-Z]+=(.*)$", line.strip())
                if match:
                    values[match.group(1)] = match.group(2)
        for key in ("CMAKE_NM", "CMAKE_OBJDUMP", "CMAKE_OBJCOPY"):
            if key not in values:
                raise TestFailure(f"{key} is not in the CMake cache")
        return values

    def _load_compile_entries(self):
        path = os.path.join(self.build, "compile_commands.json")
        with open(path, encoding="utf-8") as handle:
            database = json.load(handle)
        entries = {}
        for entry in database:
            command = entry.get("command", "")
            if "boot_image.dir" not in command:
                continue
            source = os.path.normpath(os.path.join(entry["directory"], entry["file"]))
            entries[source] = (entry["directory"], shlex.split(command))
        if not entries:
            raise TestFailure(
                f"{path} holds no boot_image entries -- build the boot target first"
            )
        return entries

    def _load_link_command(self):
        """(flags, objects) of boot_image's real link step, straight out of Ninja."""
        out = run(["ninja", "-t", "commands", "boot_image"], cwd=self.build).stdout
        for line in reversed(out.splitlines()):
            if "boot.elf" not in line:
                continue
            for segment in self._segments(shlex.split(line)):
                if any(
                    segment[i] == "-o" and segment[i + 1].endswith("boot.elf")
                    for i in range(len(segment) - 1)
                ):
                    return self._split_link(segment)
        raise TestFailure("boot_image's link command is not in `ninja -t commands`")

    @staticmethod
    def _segments(tokens):
        segment = []
        for token in tokens:
            if token in ("&&", ";", ":"):
                if segment:
                    yield segment
                segment = []
                continue
            segment.append(token)
        if segment:
            yield segment

    @staticmethod
    def _split_link(argv):
        objects, flags, skip = [], [], False
        for arg in argv:
            if skip:
                skip = False
                continue
            if arg == "-o":
                skip = True
                continue
            if arg.endswith(".obj") or arg.endswith(".o"):
                objects.append(arg)
                continue
            if arg.startswith("-Wl,-Map="):
                continue
            flags.append(arg)
        if not objects:
            raise TestFailure("boot_image's link command lists no objects")
        return flags, objects

    def _startup_source(self):
        """The CMSIS startup assembly, taken from the build rather than hard-coded."""
        assembly = [s for s in self.compile_entries if s.endswith(".s")]
        if len(assembly) != 1:
            raise TestFailure(f"expected one assembly source in boot_image, got {assembly}")
        return assembly[0]

    def _compile_template(self, source):
        """The real compile argv for @source, output arguments stripped out."""
        key = os.path.normpath(source)
        if key not in self.compile_entries:
            raise TestFailure(f"{source} has no compile_commands.json entry")
        directory, argv = self.compile_entries[key]
        cleaned, skip = [], False
        for arg in argv:
            if skip:
                skip = False
                continue
            if arg in ("-o", "-MT", "-MF"):
                skip = True
                continue
            if arg in ("-MD", "-c"):
                continue
            cleaned.append(arg)
        if cleaned and not cleaned[-1].startswith("-") and os.path.isabs(cleaned[-1]):
            cleaned.pop()               # the source itself, always last
        return directory, cleaned

    # --- building one fixture image ---------------------------------------
    def _build_image(self, spec):
        name = spec["name"]
        stem = os.path.join(self.work, name)
        os.makedirs(stem, exist_ok=True)
        elf = os.path.join(stem, f"{name}.elf")
        binary = os.path.join(stem, f"{name}.bin")
        objects = list(self.link_objects)
        extra_flags = []

        if spec.get("bin_patch"):
            # No relink at all: the healthy artefacts, with the .bin substituted.
            shutil.copy(os.path.join(self.build, "boot-reference", "boot.elf"), elf)
            shutil.copy(os.path.join(self.build, "boot-reference", "boot.bin"), binary)
            with open(binary, "r+b") as handle:
                handle.seek(0x2000)     # well past the vector table
                original = handle.read(1)
                handle.seek(0x2000)
                handle.write(bytes([original[0] ^ 0xFF]))
            return elf, binary

        if "lto_object" in spec:
            # Healthy artefacts again; what is perturbed is the OBJECT DIRECTORY
            # the gate audits.  Symlinks for the untouched objects, and one real
            # file compiled with -flto in place of the one named.
            return self._lto_object_fixture(spec, stem)

        if "source_patch" in spec:
            relative, patches = spec["source_patch"]
            objects = self._swap_patched(
                stem, os.path.join(self.board, relative), patches, objects
            )
        if "startup_patch" in spec:
            objects = self._swap_patched(
                stem, self._startup_source(), spec["startup_patch"], objects
            )
        if "inject" in spec:
            source = os.path.join(stem, f"{name}.c")
            with open(source, "w", encoding="utf-8") as handle:
                handle.write(HAL_DECLS + spec["inject"])
            obj = os.path.join(stem, f"{name}.c.obj")
            directory, template = self._compile_template(
                os.path.join(self.board, "boot", "main.c")
            )
            run(template + ["-c", source, "-o", obj], cwd=directory)
            objects.append(obj)
        # --gc-sections would sweep an injected object away, and a fixture whose
        # violation is not in the image tests nothing.  Naming the symbol with
        # --undefined makes it a GC root.  It doubles as the mechanism for the
        # fixtures that inject nothing at all: HAL functions like
        # HAL_DBGMCU_EnableDBGSleepMode are already compiled into boot_image's
        # objects and only dropped for lack of a caller, so naming one is exactly
        # the regression being modelled -- it became reachable.
        for root in spec.get("roots", []):
            extra_flags.append(f"-Wl,--undefined={root}")
        for suffixes, old, new in spec.get("redefine_syms", []):
            for suffix in suffixes:
                objects = self._redefine(stem, suffix, old, new, objects)

        run(
            self.link_flags
            + extra_flags
            + objects
            + ["-o", elf, f"-Wl,-Map={os.path.join(stem, name + '.map')},--cref"],
            cwd=self.build,
        )
        run([self.objcopy, "-O", "binary", "-S", elf, binary])
        return elf, binary

    def _lto_object_fixture(self, spec, stem):
        """A mirror of the object directory with one object rebuilt with -flto."""
        real = os.path.join(self.build, "CMakeFiles", "boot_image.dir")
        mirror = os.path.join(stem, "boot_image.dir")
        shutil.rmtree(mirror, ignore_errors=True)
        target_name = os.path.basename(spec["lto_object"]) + ".obj"
        replaced = None
        for root, _dirs, files in os.walk(real):
            rel = os.path.relpath(root, real)
            out = os.path.join(mirror, rel) if rel != "." else mirror
            os.makedirs(out, exist_ok=True)
            for name in files:
                if not name.endswith((".obj", ".o")):
                    continue
                if name == target_name and replaced is None:
                    source = os.path.join(self.board, spec["lto_object"])
                    directory, template = self._compile_template(source)
                    replaced = os.path.join(out, name)
                    run(template + ["-flto", "-c", source, "-o", replaced], cwd=directory)
                else:
                    os.symlink(os.path.join(root, name), os.path.join(out, name))
        if replaced is None:
            raise TestFailure(f"{target_name} is not among boot_image's objects")
        # Confirmed with objdump, not with the checker's own ELF reader: a fixture
        # verified by the code under test is not a test.
        sections = run([self.objdump, "-h", replaced]).stdout
        if ".gnu.lto_" not in sections and ".gnu.debuglto_" not in sections:
            raise TestFailure(
                f"{replaced} carries no LTO section, so the fixture proves nothing "
                "(did the toolchain stop emitting .gnu.lto_*?)"
            )
        self._object_dir_override = mirror
        return (
            os.path.join(self.build, "boot-reference", "boot.elf"),
            os.path.join(self.build, "boot-reference", "boot.bin"),
        )

    def _swap_patched(self, stem, source, patches, objects):
        """Recompile a patched COPY of @source and put its object in the link.

        A copy: the committed tree is never edited, because a test that edits
        sources and puts them back is one interrupted run away from a corrupted
        repository -- and this particular tree is the one that must not change.
        """
        source = os.path.normpath(source)
        with open(source, encoding="utf-8") as handle:
            text = handle.read()
        for pattern, replacement in patches:
            patched, count = re.subn(pattern, replacement, text, count=1)
            if count != 1:
                raise TestFailure(
                    f"patch {pattern!r} matched {count} times in {source} -- the "
                    "fixture would not be what it claims"
                )
            text = patched
        copy = os.path.join(stem, os.path.basename(source))
        with open(copy, "w", encoding="utf-8") as handle:
            handle.write(text)
        obj = copy + ".obj"
        directory, template = self._compile_template(source)
        run(template + ["-c", copy, "-o", obj], cwd=directory)
        target = os.path.basename(source) + ".obj"
        replaced = [o for o in objects if os.path.basename(o) != target]
        if len(replaced) != len(objects) - 1:
            raise TestFailure(f"expected exactly one {target} in the link, found "
                              f"{len(objects) - len(replaced)}")
        return replaced + [obj]

    def _redefine(self, stem, suffix, old, new, objects):
        matches = [o for o in objects if os.path.basename(o) == suffix]
        if len(matches) != 1:
            raise TestFailure(f"expected exactly one {suffix} in the link")
        source = matches[0]
        copy = os.path.join(stem, "redef_" + suffix)
        run(
            [self.objcopy, f"--redefine-sym={old}={new}",
             os.path.join(self.build, source), copy]
        )
        return [o for o in objects if o != source] + [copy]

    # --- running the gate --------------------------------------------------
    def _run_postlink(self, elf, binary):
        object_dir = self._object_dir_override or os.path.join(
            self.build, "CMakeFiles", "boot_image.dir"
        )
        return subprocess.run(
            [
                sys.executable, self.checker, "postlink",
                "--elf", elf, "--bin", binary,
                "--object-dir", object_dir,
                "--translation-units",
                os.path.join(self.build, "boot-reference", "boot_translation_units.txt"),
                "--nm", self.nm, "--objdump", self.objdump, "--objcopy", self.objcopy,
                # A deliberately impossible golden value with the override on: the
                # BUILD TREE is never configured with BOOT_ALLOW_IMAGE_DRIFT=ON.
                "--golden-sha256", "0" * 64, "--golden-size", "0",
                "--allow-image-drift",
            ],
            capture_output=True,
            text=True,
        )

    def _run_precheck(self, board_dir, compile_commands):
        return subprocess.run(
            [
                sys.executable, self.checker, "precheck",
                "--board-dir", board_dir,
                "--boot-dir", os.path.join(board_dir, "boot"),
                "--manifest", self.manifest,
                "--compile-commands", compile_commands,
                "--object-dir", "boot_image.dir",
                "--translation-units",
                os.path.join(self.build, "boot-reference", "boot_translation_units.txt"),
                "--stamp", os.path.join(self.work, "fixture.stamp"),
            ],
            capture_output=True,
            text=True,
        )

    # --- pre-link fixtures -------------------------------------------------
    def _scratch_board(self, name):
        """A copy of the parts of the board directory C6a looks at.

        A copy, not the real tree: a test that edits committed sources and puts
        them back is one interrupted run away from a corrupted repository.
        """
        root = os.path.join(self.work, name, "board")
        os.makedirs(root, exist_ok=True)
        for sub in ("boot", "ldscript"):
            shutil.copytree(
                os.path.join(self.board, sub),
                os.path.join(root, sub),
                dirs_exist_ok=True,
            )
        return root

    def _tampered_source_is_image_neutral(self, spec, board_root):
        """Compile the pristine and the tampered copy and compare the objects.

        This is what makes fx_src_tampered's claim -- "the golden hash cannot see
        this" -- a measurement rather than an assertion.  Both are compiled with
        -g0 from a directory whose basename matches, so the only thing that could
        differ is code.
        """
        relative = spec["tamper"][0]
        pristine_dir = os.path.join(self.work, spec["name"], "pristine")
        os.makedirs(pristine_dir, exist_ok=True)
        pristine = os.path.join(pristine_dir, os.path.basename(relative))
        shutil.copy(os.path.join(self.board, relative), pristine)
        tampered = os.path.join(board_root, relative)
        directory, template = self._compile_template(os.path.join(self.board, relative))
        objects = []
        for source in (pristine, tampered):
            obj = source + ".g0.obj"
            run(
                template + ["-g0", "-c", os.path.basename(source), "-o", obj],
                cwd=os.path.dirname(source),
            )
            objects.append(obj)
        with open(objects[0], "rb") as a, open(objects[1], "rb") as b:
            if a.read() != b.read():
                raise TestFailure(
                    "the tampered source produces a DIFFERENT object, so this fixture "
                    "no longer demonstrates what C6a catches that C6b cannot"
                )

    def _prepare_precheck(self, spec):
        compile_commands = os.path.join(self.build, "compile_commands.json")
        board_root = self.board
        if "tamper" in spec:
            board_root = self._scratch_board(spec["name"])
            path = os.path.join(board_root, spec["tamper"][0])
            with open(path, encoding="utf-8") as handle:
                lines = handle.read().splitlines(keepends=True)
            # A character appended INSIDE a block-comment body: guaranteed not to
            # reach the compiler, which is the property this fixture is about.  The
            # line must not close the comment, or the character would land in code.
            for index, line in enumerate(lines):
                if re.match(r"^\s*\*\s+\w", line) and "*/" not in line:
                    lines[index] = line.rstrip("\n") + ".\n"
                    break
            else:
                raise TestFailure(f"no block-comment body line to perturb in {path}")
            with open(path, "w", encoding="utf-8") as handle:
                handle.writelines(lines)
            self._tampered_source_is_image_neutral(spec, board_root)
        if "add_file" in spec:
            board_root = self._scratch_board(spec["name"])
            with open(
                os.path.join(board_root, spec["add_file"]), "w", encoding="utf-8"
            ) as handle:
                handle.write("/* a file that is not in the manifest */\n")
        if "cc_inject" in spec or spec.get("cc_drop_boot"):
            with open(compile_commands, encoding="utf-8") as handle:
                database = json.load(handle)
            if spec.get("cc_drop_boot"):
                database = [
                    e for e in database if "boot_image.dir" not in e.get("command", "")
                ]
            else:
                for entry in database:
                    if "boot_image.dir" in entry.get("command", ""):
                        entry["command"] = entry["command"].replace(
                            " -c ", f" {spec['cc_inject']} -c ", 1
                        )
                        break
                else:
                    raise TestFailure("no boot entry to perturb")
            compile_commands = os.path.join(self.work, spec["name"] + "_cc.json")
            with open(compile_commands, "w", encoding="utf-8") as handle:
                json.dump(database, handle)
        return board_root, compile_commands

    # --- the driver --------------------------------------------------------
    def run_all(self, only=None):
        results = []
        for spec in POSTLINK_FIXTURES:
            if only and spec["name"] != only:
                continue
            results.append(self._one_postlink(spec))
        for spec in PRECHECK_FIXTURES:
            if only and spec["name"] != only:
                continue
            results.append(self._one_precheck(spec))
        if not only:
            results.extend(self._baselines())

        print()
        failed = [r for r in results if not r[0]]
        for ok, name, detail in results:
            print(f"  {'PASS' if ok else 'FAIL'}  {name}{'' if ok else ': ' + detail}")
        print()
        if failed:
            print(f"check_boot_safety fixtures: {len(failed)} of {len(results)} FAILED")
            return 1
        print(f"check_boot_safety fixtures: all {len(results)} passed")
        return 0

    def _one_postlink(self, spec):
        name = spec["name"]
        print(f"[postlink] {name}: {spec['why']}")
        self._object_dir_override = None
        try:
            elf, binary = self._build_image(spec)
            with open(binary, "rb") as handle:
                image = handle.read()
            context = Context(
                elf,
                image,
                nm_symbols(self.nm, elf),
                list(struct.unpack_from("<" + "I" * 48, image, 0)),
                self.objdump,
            )
            if spec.get("verify"):
                spec["verify"](context)
        except TestFailure as exc:
            return (False, name, f"the fixture is not what it claims: {exc}")
        proc = self._run_postlink(elf, binary)
        return self._judge(spec, proc)

    def _one_precheck(self, spec):
        name = spec["name"]
        print(f"[precheck] {name}: {spec['why']}")
        try:
            board_root, compile_commands = self._prepare_precheck(spec)
        except TestFailure as exc:
            return (False, name, f"the fixture is not what it claims: {exc}")
        proc = self._run_precheck(board_root, compile_commands)
        return self._judge(spec, proc)

    @staticmethod
    def _judge(spec, proc):
        name = spec["name"]
        output = proc.stdout + proc.stderr
        if proc.returncode != spec["expect_exit"]:
            return (
                False,
                name,
                f"expected exit {spec['expect_exit']}, got {proc.returncode}\n"
                f"{output.strip()}",
            )
        if spec["expect_id"] not in output:
            return (
                False,
                name,
                f"exit {proc.returncode} was right but {spec['expect_id']} is not in "
                f"the output -- it failed for a DIFFERENT reason\n{output.strip()}",
            )
        if spec.get("forbid_id") and spec["forbid_id"] in output:
            return (
                False,
                name,
                f"{spec['forbid_id']} also fired; this fixture is supposed to be "
                "invisible to that check",
            )
        return (True, name, "")

    def _baselines(self):
        """The healthy image, the two app images, and malformed input."""
        reference = os.path.join(self.build, "boot-reference")
        cases = [
            (
                "baseline_healthy_boot",
                [reference + "/boot.elf", reference + "/boot.bin"],
                0,
                "OK -- sector 0 is safe",
                True,
            ),
            (
                "baseline_shell_elf",
                [self.build + "/shell.elf", self.build + "/shell.bin"],
                1,
                "BOOT-C1-VECTOR-VMA",
                False,
            ),
            (
                "baseline_blink_elf",
                [self.build + "/blink.elf", self.build + "/blink.bin"],
                1,
                "BOOT-C1-VECTOR-VMA",
                False,
            ),
            (
                "baseline_not_an_elf",
                [os.path.join(self.board, "boot", "main.c"), reference + "/boot.bin"],
                2,
                "BOOT-INPUT-NOT-ELF",
                False,
            ),
            (
                "baseline_missing_bin",
                [reference + "/boot.elf", os.path.join(self.work, "does-not-exist.bin")],
                2,
                "BOOT-INPUT-NO-BIN",
                False,
            ),
        ]
        results = []
        for name, (elf, binary), expect_exit, expect_text, golden in cases:
            print(f"[baseline] {name}")
            argv = [
                sys.executable, self.checker, "postlink",
                "--elf", elf, "--bin", binary,
                "--object-dir",
                os.path.join(self.build, "CMakeFiles", "boot_image.dir"),
                "--translation-units",
                os.path.join(self.build, "boot-reference", "boot_translation_units.txt"),
                "--nm", self.nm, "--objdump", self.objdump, "--objcopy", self.objcopy,
            ]
            if golden:
                # The healthy case is checked against the REAL golden values, with
                # no override: the suite would be worth little if it never once
                # confirmed that the gate passes the image the project ships.
                argv += [
                    "--golden-sha256", self._golden_sha(),
                    "--golden-size", str(self._golden_size()),
                ]
            else:
                argv += ["--golden-sha256", "0" * 64, "--golden-size", "0",
                         "--allow-image-drift"]
            proc = subprocess.run(argv, capture_output=True, text=True)
            output = proc.stdout + proc.stderr
            if proc.returncode != expect_exit:
                results.append(
                    (False, name, f"expected exit {expect_exit}, got {proc.returncode}\n"
                                  f"{output.strip()}")
                )
            elif expect_text not in output:
                results.append((False, name, f"{expect_text!r} not in the output"))
            else:
                results.append((True, name, ""))
        return results

    def _golden(self):
        text = open(os.path.join(self.board, "board.cmake"), encoding="utf-8").read()
        sha = re.search(r'BOOT_GOLDEN_SHA256\s*\n?\s*"([0-9a-f]{64})"', text)
        size = re.search(r"BOOT_GOLDEN_SIZE\s+(\d+)", text)
        if not sha or not size:
            raise TestFailure("the golden values are not readable from board.cmake")
        return sha.group(1), int(size.group(1))

    def _golden_sha(self):
        return self._golden()[0]

    def _golden_size(self):
        return self._golden()[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--build-dir", required=True)
    parser.add_argument("--board-dir", required=True)
    parser.add_argument("--only", help="run a single fixture by name")
    args = parser.parse_args()
    try:
        runner = Runner(os.path.abspath(args.build_dir), os.path.abspath(args.board_dir))
    except TestFailure as exc:
        print(f"fixtures: cannot set up: {exc}", file=sys.stderr)
        return 2
    return runner.run_all(only=args.only)


if __name__ == "__main__":
    sys.exit(main())
