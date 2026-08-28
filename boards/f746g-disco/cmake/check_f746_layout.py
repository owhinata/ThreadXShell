#!/usr/bin/env python3
"""Post-link placement, vector-table and float-runtime gate for f746g-disco.

WHY THIS EXISTS
---------------
ldscript/STM32F746NGHx_FLASH.ld already carries most of this board's placement
invariant as ASSERTs, and they are good ones -- but they can only see the section
BOUNDARIES, and this script emits those boundaries unconditionally.  `.sdram` is a
single output section in which _ssdram_cam / _ssdram_eth / _ssdram_ai are placed by
`. = ORIGIN(SDRAM) + <offset>` whether or not any input landed in them.  So:

  * An object that loses its `section(".sdram.cam")` attribute -- a refactor, an
    #ifdef, a renamed pattern -- moves to .bss.  Every ASSERT still passes.  The
    camera DMA arena is then simply not where port/camera/camera.c believes it is,
    and the DCMI writes 2 MB into whatever is at that address.
  * The same is true in the other direction for the buffers whose whole reason to
    be in SDRAM is that the region is MPU non-cacheable (the ETH descriptors, the
    LTDC scan-out surface).  In cached SRAM they work until they do not.

This checks the linked image for the SYMBOLS instead: each required object must
resolve to an address inside its region, and a name that matches NOTHING is a
failure rather than a vacuous pass -- that is the vanished symbol the ASSERTs
cannot see.

It also guards two things the linker script never could:

  * THE THREE HANDLERS THIS FIRMWARE MUST OWN.  The stock CMSIS startup file
    declares PendSV_Handler, SysTick_Handler and USART1_IRQHandler `.weak` and
    aliases all three to Default_Handler (an infinite loop), so a build that lost
    ThreadX's PendSV or the UART backend's ISR still LINKS and still has all three
    "defined".  A firmware whose PendSV is an infinite loop does not context
    switch; a firmware whose USART1 ISR is one has a dead console.  Being defined
    proves nothing, so each is required to be a STRONG text symbol, at an address
    different from Default_Handler, actually installed in its .isr_vector slot.

  * THE DOUBLE-PRECISION SOFTWARE ROUTINES.  This part has the single-precision
    FPU (fpv5-sp-d16), so CoreMark's `%f` score line runs on __aeabi_d* and on
    newlib's _printf_float, which -u _printf_float pulls in explicitly.  "No
    unresolved references" says nothing here -- that is just what a successful
    link means -- so this is a presence check.

Exit status 0 = pass; 1 = a failure; 2 = the check could not be performed (which
is also a failure: a gate that skips itself reports the same silence as a passing
one).
"""

import argparse
import re
import subprocess
import sys

CANNOT_CHECK = 2   # never confused with pass

# --- Memory map.  Mirrors the MEMORY block + .sdram bank split of
#     ldscript/STM32F746NGHx_FLASH.ld.  [start, end).
DTCM = (0x20000000, 0x20010000)          # 64 KB, not D-cached
SRAM1 = (0x20010000, 0x2004C000)         # 240 KB; SRAM2 follows to 0x20050000
SDRAM_BANK0 = (0xC0000000, 0xC0200000)   # FMC internal bank0: display / fixed
SDRAM_BANK1 = (0xC0200000, 0xC0400000)   # bank1: camera DMA arena
SDRAM_BANK2 = (0xC0400000, 0xC0600000)   # bank2: ETH descriptors + pools
SDRAM_BANK3 = (0xC0600000, 0xC0800000)   # bank3: NN arena
MODEL_WINDOW = (0xC0700000, 0xC0800000)  # bank3 upper half: reloc exec window

VECTOR_BASE = 0x08000000
ESTACK = 0x20050000

# Objects that exist in EVERY configuration of this firmware, and the region each
# one must be in.  Configuration-dependent residents are NOT listed here -- they
# arrive from board.cmake via --require-*, because a name in this list that the
# build never compiled reports as "no such object in the image", which reads
# exactly like a placement regression and is not one.
#
# The reason each one is where it is:
#   g_log            svc/log.c.  DTCM bypasses the D-cache, so a log write from
#                    fault context is committed with no cache maintenance and is
#                    always visible over SWD.  NOLOAD, survives reset.
#   dtcm_bench_buf   cmds/cmd_membench.c.  The instrument's DTCM row; in SRAM it
#                    would measure a different memory and report it as DTCM.
#   sd_bounce        port/sd/sd_card.c.  SDMMC DMA target, 32 B aligned in SRAM1
#                    so the D-cache clean/invalidate acts on this buffer alone.
#   ltdc_fb          port/ltdc/ltdc_display.c.  LTDC scan-out READ surface, pinned
#                    at the front of bank0 so the display reads and the camera
#                    writes (bank1) each keep their own FMC row open.
#   cam_frame        port/camera/camera.c.  DCMI-written, CPU-read; coherent only
#                    because the SDRAM window is MPU non-cacheable.
#   sdram_bench_buf  cmds/cmd_membench.c.  The instrument's SDRAM row.
#   cam_arena        port/camera/camera.c.  The 2 MB DCMI ring arena; bank1 exactly.
#   g_tx_desc        port/eth/eth_link.c.  ETH DMA descriptors.  The new HAL_ETH
#   g_rx_desc        does NO cache maintenance, so these MUST be non-cacheable.
#   tx_coalesce      port/netxduo/nx_eth_driver.c.  ETH DMA reads it.
#   eth_pool_mem     port/netxduo/nx_glue.c.  NetX packet pool the ETH DMA fills.
REQUIRED = (
    ("g_log", DTCM, ".log_noinit (DTCM)"),
    ("dtcm_bench_buf", DTCM, ".dtcm_bench (DTCM)"),
    ("sd_bounce", SRAM1, ".sram1_dma (SRAM1)"),
    ("ltdc_fb", SDRAM_BANK0, ".sdram.fixed.ltdc (SDRAM bank0)"),
    ("cam_frame", SDRAM_BANK0, ".sdram.fixed (SDRAM bank0)"),
    ("sdram_bench_buf", SDRAM_BANK0, ".sdram.fixed (SDRAM bank0)"),
    ("cam_arena", SDRAM_BANK1, ".sdram.cam (SDRAM bank1)"),
    ("g_tx_desc", SDRAM_BANK2, ".sdram.eth (SDRAM bank2)"),
    ("g_rx_desc", SDRAM_BANK2, ".sdram.eth (SDRAM bank2)"),
    ("tx_coalesce", SDRAM_BANK2, ".sdram.eth (SDRAM bank2)"),
    ("eth_pool_mem", SDRAM_BANK2, ".sdram.eth (SDRAM bank2)"),
)

# Handler name -> vector table index.  ARMv7-M: slot 0 is the initial SP, 1 is
# Reset, and external interrupt n is at 16+n.  USART1_IRQn is 37 on the
# STM32F746 (lib/cmsis_device_f7/Include/stm32f746xx.h).
HANDLERS = (
    ("PendSV_Handler", 14, "ThreadX supplies this; without it there is no context switch"),
    ("SysTick_Handler", 15, "the ThreadX tick and HAL_IncTick both hang off it"),
    ("USART1_IRQHandler", 16 + 37, "the CLI UART backend's RX/TX ISR: the console"),
)

# Symbol types nm reports for data objects; used when matching REQUIRED names so a
# same-named function could never satisfy a residency requirement.
DATA_TYPES = "bBdDrRgGsS"

# GCC clone/localisation suffixes; they stack, so stripping repeats.  Same set as
# the wio-lite-ai checkers, kept in step with them deliberately.
CLONE_SUFFIX_RE = re.compile(
    r"\.(?:isra|constprop|part|lto_priv|cold|localalias)(?:\.\d+)?$")


def strip_clone_suffixes(name):
    """`foo.lto_priv.0` -> `foo`."""
    while True:
        stripped = CLONE_SUFFIX_RE.sub("", name)
        if stripped == name:
            return name
        name = stripped


def die(message):
    """Exit with CANNOT_CHECK: the guard did not run, which is not the same as passing."""
    print(f"check_f746_layout: {message}", file=sys.stderr)
    sys.exit(CANNOT_CHECK)


def run(cmd):
    try:
        return subprocess.run(cmd, capture_output=True, text=True, check=True).stdout
    except (OSError, subprocess.CalledProcessError) as exc:
        die(f"cannot run {cmd[0]}: {exc}")


def read_symbols(nm, elf):
    """[(address, type, name)] for every symbol nm reports with an address."""
    syms = []
    for line in run([nm, elf]).splitlines():
        parts = line.split(maxsplit=2)
        if len(parts) != 3 or not re.fullmatch(r"[0-9a-fA-F]+", parts[0]):
            continue
        syms.append((int(parts[0], 16), parts[1], parts[2]))
    if not syms:
        die(f"{nm} reported no symbols for {elf} -- stripped image?")
    return syms


def read_vectors(objdump, elf):
    """The .isr_vector section as a list of 32-bit little-endian words.

    objdump -s prints `<addr> <w0> <w1> <w2> <w3>  <ascii>`, with TWO spaces before
    the ASCII gutter and one between the hex groups.  Splitting on the double space
    first is what keeps an ASCII column that happens to read as hex (`deadbeef`) out
    of the vector table.
    """
    out = run([objdump, "-s", "-j", ".isr_vector", elf])
    words = []
    for line in out.splitlines():
        m = re.match(r"\s*([0-9a-fA-F]+)\s(.*)$", line)
        if not m:
            continue
        hex_area = m.group(2).split("  ", 1)[0]
        groups = hex_area.split()
        if not groups or not all(re.fullmatch(r"[0-9a-fA-F]{2,8}", g) for g in groups):
            continue
        for group in groups:
            if len(group) != 8:
                continue        # a trailing partial word cannot be a vector entry
            words.append(int.from_bytes(bytes.fromhex(group), "little"))
    if not words:
        die(f"{objdump} found no .isr_vector contents in {elf} -- wrong linker script?")
    return words


def unique_address(syms, name, types=None):
    """The address of @name, or None.  Ambiguity is a failure, not a coin toss."""
    hits = {
        addr
        for addr, typ, sym in syms
        if strip_clone_suffixes(sym) == name and (types is None or typ in types)
    }
    if len(hits) > 1:
        die(f"{name} resolves to {len(hits)} different addresses -- cannot check")
    return hits.pop() if hits else None


def check_fixed(syms):
    failures = []
    vec = unique_address(syms, "g_pfnVectors")
    if vec is None:
        failures.append("g_pfnVectors: not in the image -- the CMSIS startup file "
                        "is not linked, so there is no vector table to install.")
    elif vec != VECTOR_BASE:
        failures.append(f"g_pfnVectors is at 0x{vec:08x}, not the reset vector "
                        f"address 0x{VECTOR_BASE:08x}.  The MCU fetches its initial "
                        f"SP and PC from 0x{VECTOR_BASE:08x}, so this image would "
                        f"not boot.")
    estack = unique_address(syms, "_estack")
    if estack is None:
        failures.append("_estack: not defined -- the linker script did not run?")
    elif estack != ESTACK:
        failures.append(f"_estack is 0x{estack:08x}, expected 0x{ESTACK:08x} (top of "
                        f"SRAM1+SRAM2).  src/retarget.c bounds the heap against this.")
    return failures


def check_command_table(syms):
    """The shell command table must exist and be non-empty, and its size is reported.

    The byte span and the registration-symbol count are what a reference build is
    compared against: a command file that silently stops being compiled produces a
    firmware that links, boots and is simply missing a command.
    """
    failures = []
    start = unique_address(syms, "__cli_root_cmds_start")
    end = unique_address(syms, "__cli_root_cmds_end")
    if start is None or end is None:
        failures.append("__cli_root_cmds_start/__cli_root_cmds_end: missing -- the "
                        ".shell_root_cmds section is not in the linker script.")
        return failures, 0, 0
    if end <= start:
        failures.append(f".shell_root_cmds is empty ({end - start} B): no command "
                        f"registered.  CLI_CMD_REGISTER entries are KEEP'd, so an "
                        f"empty table means the command files were not compiled.")
    registrations = {
        strip_clone_suffixes(sym)
        for _addr, typ, sym in syms
        if typ in DATA_TYPES and sym.startswith("__cli_cmd_")
    }
    return failures, end - start, len(registrations)


def check_float_runtime(syms):
    """The double-precision path CoreMark's %f score line needs."""
    failures = []
    defined = {sym for _addr, typ, sym in syms if typ not in "Uvw"}
    if not any(strip_clone_suffixes(s) == "_printf_float" for s in defined):
        failures.append("_printf_float is not in the image.  newlib-nano omits float "
                        "printf unless it is pulled in explicitly; without it "
                        "CoreMark's score line prints nothing useful.  board.cmake "
                        "passes -u _printf_float for exactly this.")
    if not any(s.startswith("__aeabi_d") for s in defined):
        failures.append("no __aeabi_d* routine is in the image.  This part has the "
                        "single-precision FPU (fpv5-sp-d16), so every double "
                        "operation is a call into these -- none present means the "
                        "double path was optimised away or the float ABI changed.")
    return failures


def check_handlers(syms, vectors):
    """Strong, distinct from Default_Handler, and actually in the vector slot."""
    failures = []
    default = unique_address(syms, "Default_Handler", types="Tt")
    if default is None:
        die("Default_Handler not found -- the CMSIS startup file is not linked, so "
            "the weak-alias check below cannot mean anything.")

    for name, slot, why in HANDLERS:
        matches = [(addr, typ) for addr, typ, sym in syms
                   if strip_clone_suffixes(sym) == name]
        if not matches:
            failures.append(f"{name}: not in the image at all ({why}).")
            continue
        strong = [addr for addr, typ in matches if typ == "T"]
        if not strong:
            kinds = ",".join(sorted({typ for _a, typ in matches}))
            failures.append(
                f"{name}: only weak/undefined definitions present (nm type "
                f"{kinds}).  The CMSIS startup file supplies all three of these as "
                f".weak aliases of Default_Handler, so this is what a build that "
                f"LOST the real implementation looks like -- {why}.")
            continue
        addr = strong[0]
        if addr == default:
            failures.append(
                f"{name}: resolves to Default_Handler (0x{addr:08x}), an infinite "
                f"loop.  {why[0].upper()}{why[1:]}.")
            continue
        if slot >= len(vectors):
            failures.append(f"{name}: vector slot {slot} is past the end of "
                            f".isr_vector ({len(vectors)} words).")
            continue
        installed = vectors[slot]
        if installed != (addr | 1):
            failures.append(
                f"{name}: defined at 0x{addr:08x} but .isr_vector[{slot}] holds "
                f"0x{installed:08x} (expected 0x{addr | 1:08x}).  The implementation "
                f"is in the image but the hardware would not reach it -- {why}.")
    return failures


def check_residency(syms, required):
    """Every named object must resolve inside its region; a missing name fails."""
    failures = []
    for name, (start, end), where in required:
        matches = [(addr, sym) for addr, typ, sym in syms
                   if typ in DATA_TYPES and strip_clone_suffixes(sym) == name]
        if not matches:
            failures.append(
                f"{name}: no such object in the image.  Either it was renamed beyond "
                f"the suffixes this script strips, or it was optimised away, or its "
                f"section attribute was dropped and --gc-sections then removed it. "
                f"Check its definition before relaxing this.")
            continue
        for addr, sym in matches:
            if not start <= addr < end:
                failures.append(
                    f"{name}: {sym} is at 0x{addr:08x}, outside {where} "
                    f"[0x{start:08x}, 0x{end:08x}).  Its placement attribute was "
                    f"dropped or its input-section pattern in "
                    f"ldscript/STM32F746NGHx_FLASH.ld stopped matching; the linker "
                    f"script's ASSERTs cannot see this because they only bound the "
                    f"section boundaries, which are emitted unconditionally.")
    return failures


def check_not_in_sdram(syms, forbidden):
    """Every symbol named here must live OUTSIDE external SDRAM.

    [!] THIS IS ABOUT INITIALISATION, NOT SPEED (issue #97).  The whole `.sdram`
    output section is NOLOAD, so an object with initialised fields placed there is
    never loaded -- and because NOLOAD keeps whatever the previous run left, it
    comes up holding stale values instead of obviously-wrong ones.  A "ready" flag
    that moved into SDRAM with the state it guards would survive a warm reset
    still saying ready, and initialisation would be skipped over stale data.

    Requiring the symbol to EXIST is deliberate: a list that silently guards
    nothing is the failure mode these gates are written against.
    """
    failures = []
    lo, hi = SDRAM_BANK0[0], SDRAM_BANK3[1]
    for name in forbidden:
        matches = [(addr, sym) for addr, typ, sym in syms
                   if typ in DATA_TYPES and strip_clone_suffixes(sym) == name]
        if not matches:
            failures.append(
                f"{name}: no such object in the image.  This list names state that "
                f"must stay in internal RAM; if it was renamed or removed, update "
                f"the list rather than letting the check guard nothing."
            )
            continue
        for addr, sym in matches:
            if lo <= addr < hi:
                failures.append(
                    f"{sym} at 0x{addr:08x} is in external SDRAM, which is NOLOAD.  "
                    f"Its initialisers are never loaded and it comes up holding the "
                    f"PREVIOUS run's bytes -- so it fails by appearing to work.  "
                    f"State belongs in internal RAM; only write-before-read scratch "
                    f"belongs in the carve-out."
                )
    return failures


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("elf")
    ap.add_argument("--nm", required=True)
    ap.add_argument("--objdump", required=True)
    ap.add_argument("--require-sdram-ai", action="append", metavar="SYMBOL", default=[],
                    help="backend-conditional object that must live in the NN arena "
                         "(SDRAM bank3); repeat once per object (board.cmake supplies these)")
    ap.add_argument("--require-model-window", action="append", metavar="SYMBOL", default=[],
                    help="object that must sit in the 0xC0700000 executable model "
                         "window (stedgeai_reloc only)")
    ap.add_argument("--forbid-sdram", action="append", metavar="SYMBOL", default=[],
                    help="object that must NOT be in external SDRAM at all -- for "
                         "state that has to be INITIALISED, since the whole .sdram "
                         "output section is NOLOAD (issue #97)")
    args = ap.parse_args()

    required = list(REQUIRED)
    for name in args.require_sdram_ai:
        required.append((name, SDRAM_BANK3, ".sdram.ai (SDRAM bank3)"))
    for name in args.require_model_window:
        required.append((name, MODEL_WINDOW, ".sdram.ai.model (bank3 exec window)"))

    syms = read_symbols(args.nm, args.elf)
    vectors = read_vectors(args.objdump, args.elf)

    failures = (check_fixed(syms)
                + check_handlers(syms, vectors)
                + check_float_runtime(syms))
    cmd_failures, cmd_bytes, cmd_count = check_command_table(syms)
    failures += cmd_failures
    failures += check_residency(syms, required)
    failures += check_not_in_sdram(syms, args.forbid_sdram)

    if failures:
        print(f"check_f746_layout: FAIL ({len(failures)} problem(s)) in {args.elf}",
              file=sys.stderr)
        for f in failures:
            print(f"  - {f}", file=sys.stderr)
        return 1

    print(f"check_f746_layout: OK -- {len(required)} placed objects in their regions, "
          f"3 strong handlers installed in .isr_vector, "
          f".shell_root_cmds {cmd_bytes} B / {cmd_count} registrations")
    return 0


if __name__ == "__main__":
    sys.exit(main())
