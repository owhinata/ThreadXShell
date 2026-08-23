#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 ThreadX Shell Project
"""Gate 2: placement + budget checks on the linked shell ELF.

  1. Region budget: every ALLOC section must lie inside a known region
     (ITCM / DTCM / SRAM window), and each region must keep a minimum
     headroom.  For DTCM the meaningful slack is the gap between the heap's
     ceiling (__HeapLimit) and the MSP stack's floor (__StackLimit) -- the
     statics, heap and stack are all placed from the two ends.
  2. Vector table residency: .table at the very start of ITCM, 496 * 4 bytes.
  3. Static stacks: every *_stack-named data/bss symbol lives in DTCM.
  4. Forbidden survivors: with -ffunction-sections + --gc-sections, an
     uncalled function is dropped -- so if any FORBIDDEN name is PRESENT in
     the image, something references it, which violates the port's design.
     [!] For the NOR write path this is DEFENCE IN DEPTH, not proof (issue
     #87): the read path already links raw SPI/DMA primitives from which a
     program or erase can be assembled without naming anything on the list.
  5. Measurement-buffer residency (issue #25): each benchmark buffer must sit
     in the memory whose name `membench` prints beside its numbers, in a
     NOBITS section.
  6. Required survivors (issue #42): the mirror of check 4.  --gc-sections
     drops an uncalled function, so a name that is ABSENT was not called by
     anything -- which is how a precondition enforced before kernel entry
     would disappear without a word.  This replaces the MVE predication scan
     deleted in #42; cmake/fixtures/ removes the call and watches this fire,
     because a required-symbol check nobody has seen fail is worth as little
     as the scan it replaced (issue #66).

Stdlib-only; POST_BUILD.
"""

import argparse
import re
import subprocess
import sys

ITCM = (0x10000000, 0x40000)
DTCM = (0x30000000, 0x40000)
# The SRAM window is two regions since issue #29.  SRAM_LDR is the part the
# 2nd-stage bootloader executes from while it loads us, so it may hold NOLOAD
# reservations only; SRAM is the part where a section with CONTENTS is safe.
SRAM_LDR = (0x3401F000, 0x2E000)
SRAM     = (0x3404D000, 0x1B3000)
# Whole window, for the checks that do not care which half a section is in.
SRAM_ALL = (SRAM_LDR[0], SRAM_LDR[1] + SRAM[1])

ITCM_MIN_HEADROOM = 8 * 1024
DTCM_MIN_HEADROOM = 8 * 1024      # gap between __HeapLimit and __StackLimit

VECTORS = 496 * 4

FORBIDDEN = [
    # QSPI NOR write path (issue #44).  lib_spi_eeprom.a is linked for one
    # reason -- enabling the memory-mapped READ window the model is parsed
    # through -- and this flash holds the bootloader and the firmware image.
    # With -ffunction-sections + --gc-sections an uncalled function is dropped,
    # so any of these SURVIVING means something references it, and the only
    # thing that could is code that writes to it.  Same shape as the wio port's
    # sector-0 rule: the read path is fine, the write path must not exist.
    "hx_lib_spi_eeprom_erase_all",
    "hx_lib_spi_eeprom_erase_sector",
    "hx_lib_spi_eeprom_write",
    "hx_lib_spi_eeprom_word_write",
    "hx_lib_spi_eeprom_clear_write_protect",
    # NOT barred: hx_lib_spi_eeprom_setWriteEnable.  It IS in the image, pulled
    # in from inside the archive (spi_eeprom_peri.o) by the quad-enable path --
    # putting a Winbond-class part into QUAD mode means writing the QE bit in
    # its status register, and that write needs the WEL latch first.  So it is
    # part of configuring the READ path, not of writing the array.
    #
    # [!] AND THE OLD JUSTIFICATION FOR ALLOWING IT WAS FALSE (issue #87).  It
    # said the latch is harmless because "every entry point that issues a
    # program or erase opcode is on this list and verified absent".  It is not.
    # These are all in the shipped image, pulled in by the read/XIP path:
    #
    #     hx_drv_spi_mst_get_dev, hx_drv_dmac_get_dev
    #     hx_lib_spi_eeprom_DMA_send, DMA_send_recv, set_DMA_config, waitWIP
    #
    # A first-party translation unit can take those two handles and DMA an
    # arbitrary opcode buffer -- WREN then chip erase -- without naming one
    # symbol on this list.  They cannot be barred, because the read path needs
    # them.
    #
    # So this check is DEFENCE IN DEPTH AND NOT EXHAUSTIVE, and saying otherwise
    # is worse than saying nothing: it invites the next reader to treat "the
    # list passes" as "there is no write capability".  A gate over the write
    # path that could establish that is issue #88, and issue #88's own finding
    # is that no symbol-level check on this image can.
    #
    # [!] AND TWO OF THESE FOUR ARE ON BORROWED TIME (issue #88).  The seam in
    # port/sdk_seam/nor_seam.c wraps all four, but only erase_sector and write
    # call __real_ -- so as long as nothing in the firmware calls THEM, the
    # linker drops the vendor implementations and this list keeps passing
    # unchanged.  That is true today because Part C's writer does not exist yet;
    # when it lands, erase_sector, write and hx_lib_spi_eeprom_clear_write_
    # protect (which erase_sector calls across objects) become present, and
    # these two names have to come off this list.  erase_all and word_write do
    # NOT: their wrappers refuse without naming __real_, which is what keeps
    # them collectable and keeps this rule covering them for good.
    #
    # What replaces the two is cmake/check_nor_seam.py, which asks the question
    # this list cannot once the code is present: not "is it in the image" but
    # "who may reach it".
    "hx_lib_qspi_eeprom_erase_all",
    "hx_lib_qspi_eeprom_erase_sector",
    "hx_lib_qspi_eeprom_write",
    "hx_lib_qspi_eeprom_word_write",
    # [!] Arbitrary-opcode transports (issue #87).  These take a caller-supplied
    # byte buffer and put it on the wire, so one of them is every program and
    # erase opcode at once.  Nothing references them today, which is exactly why
    # barring them costs nothing -- and why it was worth doing before something
    # did.  Found by an adversarial review of the issue #49 plan.
    "hx_lib_spi_eeprom_Send_Op_code",
    "hx_lib_qspi_eeprom_Send_Op_code",
    # [!] AND THE ONES THAT SAY "READ".  The first pass at this list barred the
    # two functions whose names say "send opcode" and missed the two whose names
    # say "read data" -- but Send_Op_Read_Data takes the SAME caller-supplied op
    # buffer, up to 256 bytes, and sending WREN followed by a chip erase does not
    # stop being a write because the caller then reads a reply and throws it
    # away.  Barred on what they can do, not on what they are called.
    "hx_lib_spi_eeprom_Send_Op_Read_Data",
    "hx_lib_qspi_eeprom_Send_Op_Read_Data",
    # SDK SysTick pokers: ThreadX owns SysTick on this port.
    "EPII_Set_Systick_load",
    "EPII_Set_Systick_enable",
    # SDK clib console: this port supplies its own _write.
    "console_getchar",
    "console_putchar",
]

# Whole vendor APIs that must not appear, matched by prefix.  The optional
# third element is the set of names inside the prefix that ARE allowed.
FORBIDDEN_PREFIXES = [
    # [!] TIMER2 belongs to the execution profile kit (issue #25).
    # port/threadx/tx_glue.c programs it directly over MMIO as a free-running
    # all-ones-reload counter with its interrupt disabled, and it is the only
    # code in the firmware that may touch that timer.
    #
    # Blocking the whole vendor timer API rather than the TIMER_ID_2 wrappers
    # (hx_drv_timer_cm55m_*) is deliberate.  A name list would leave the
    # GENERIC entry points open -- hx_drv_timer_hw_start(TIMER_ID_2, ...) does
    # the same damage as hx_drv_timer_cm55m_start(): it recomputes RELOAD from
    # a period in milliseconds and sets the CTRL interrupt-enable bit, breaking
    # both the up-counter identity the kit's time source depends on and the
    # "no TIMER2 IRQ" property -- and no name-based check can tell which timer
    # id an argument carries.  Nothing in this port calls any of them, so the
    # whole prefix can simply be barred.
    #
    # hx_drv_timer_init is the one exception: the SDK's platform_driver_init()
    # calls it for all nine timers and it only records a base address and
    # derives g_timer_clk[] -- it starts nothing and writes no timer register.
    #
    # This is build-time defence in depth, not the only defence:
    # tx_glue_profile_ok() re-reads TIMER2's configuration on every query, so
    # a timer clobbered at runtime (by anything, named or not) downgrades cpu%
    # to "--" rather than producing a plausible wrong number.
    ("hx_drv_timer_", "Himax timer", {"hx_drv_timer_init"}),

    # [!] Power management (issue #25).  TX_ENABLE_WFI makes the idle path a
    # plain WFI, and tx_glue.c enforces SCR.SLEEPDEEP == 0 so that WFI gates
    # only the CPU clock -- which is what keeps SysTick ticking and TIMER2
    # (the cpu% time source) counting through idle.  Anything driving the
    # Himax PMU could put the part into a state where those assumptions stop
    # holding, silently and only while idle.  libpwrmgmt.a is linked but
    # nothing references it, so --gc-sections keeps the image clean.
    ("hx_lib_pm_", "Himax power management", set()),
]

# Symbols that must SURVIVE.  See check 6: with -ffunction-sections and
# --gc-sections, presence is evidence of a reference, so this catches a
# precondition whose only caller was deleted.
#
# [!] Nothing here may be force-retained (KEEP, __attribute__((used)), an
# EXTERN in the linker script).  Retaining the symbol independently would let
# an UNCALLED function satisfy this check, which is the one thing it exists to
# rule out -- and would quietly turn it into the same kind of gate as the scan
# it replaced.
REQUIRED = [
    # issue #42: FPCCR.ASPEN is what makes the hardware stack VPR and the rest
    # of the caller-saved vector state.  The prebuilt driver archives execute
    # MVE, so this is not optional and never was; the judgement lives in
    # port/threadx/fp_enforce.c and is called from _tx_initialize_low_level().
    ("fp_enforce_judge", "the FP context precondition (issue #42)"),
]

# Benchmark buffers -- issue #25.  membench labels every row with a memory
# name; a buffer that silently landed somewhere else (a section-attribute typo,
# a linker script edit, a truncated array) would make the command measure that
# other memory and report it under the old label, which is worse than not
# measuring at all.
#
# So the binding is checked all the way down, not just "the address is in the
# right region": symbol -> exact size -> the whole [addr, addr+size) span lies
# inside the named section -> that section is NOBITS and lies in the named
# region.  Sizes come from the command's own #defines and CoreMark's
# TOTAL_DATA_SIZE; a mismatch here means the two have drifted apart.
#
# (symbol, size, section or None, region name, region)
RESIDENCY = [
    ("itcm_bench_buf",  4 * 1024, ".itcm_bench", "ITCM", ITCM),
    ("dtcm_bench_buf",  4 * 1024, ".dtcm_bench", "DTCM", DTCM),
    ("sram_bench_buf", 64 * 1024, ".sram_bench", "SRAM", SRAM),
    # ST7789 framebuffer (issue #30).  Same binding as the bench buffers, and
    # for a sharper reason: a framebuffer that landed in ITCM or DTCM would not
    # fault -- the DMA controller cannot reach TCM on this part, so the panel
    # would simply stay blank while every call reported success.
    # Since issue #29 it is also the resident of the loader window: NOLOAD, so
    # the loader never writes it, and it is what keeps those 188 KB from being
    # dead space.  Pinned to that region specifically -- if it drifted up into
    # the loadable region it would silently cost .rodata and the NN arena the
    # room they were given.
    ("lcd_fb", 240 * 320 * 2, ".lcd_fb", "SRAM_LDR", SRAM_LDR),
    # Camera buffers (issues #35, #59).  The WDMA3 landing buffers -- TWO
    # frames since #59, alternated so capture overlaps the CPU's work -- are
    # written by the datapath's DMA and the pipeline slots are read by the SPI
    # DMA, so both inherit the framebuffer's failure mode exactly: in TCM they
    # would not fault, the transfer would simply never happen.  The raw arena
    # has a second reason to be pinned -- the CPU invalidates CAM_RAW_BYTES of
    # cache from one buffer's start before every frame, so an arena shorter
    # than the gate believes would have the second buffer's invalidate (and
    # the DMA behind it) running past the reservation's end.
    ("cam_raw_buf",   320 * 240 * 3 * 2, ".cam_raw",   "SRAM", SRAM),
    ("cam_slot_mem",  320 * 240 * 2 * 2, ".cam_slots", "SRAM", SRAM),
    # NN tensor arena (issue #44).  Pinned for the same reason as the camera
    # buffers -- the Ethos-U55 is a bus master and TCM reachability is
    # unverified -- plus one of its own: the CPU cleans and invalidates exactly
    # this many bytes from this symbol around every inference, so an arena
    # shorter than the gate believes would have the maintenance running off the
    # end of it.
    ("nn_arena", 450 * 1024, ".nn_arena", "SRAM", SRAM),
    # CoreMark MEM_STATIC working set: a plain .bss array, so there is no
    # dedicated section to pin it to -- only the region matters.
    ("static_memblk",   2 * 1000, None,          "DTCM", DTCM),
]

# The benchmark buffers must stay NOBITS: they are sized in tens of kilobytes
# and a LOADable one would be flashed as that many bytes of zeros (and would
# then also have to satisfy the image-coherence gate).
NOBITS_SECTIONS = [".itcm_bench", ".dtcm_bench", ".sram_bench", ".lcd_fb",
                   ".cam_raw", ".cam_slots", ".nn_arena"]


def run(cmd):
    return subprocess.run(cmd, check=True, capture_output=True,
                          text=True).stdout


def elf_sections(objdump, elf):
    out = run([objdump, "-h", elf])
    secs = {}
    lines = out.splitlines()
    for i, line in enumerate(lines):
        m = re.match(r"^\s*\d+\s+(\S+)\s+([0-9a-f]{8})\s+([0-9a-f]{8})\s+"
                     r"([0-9a-f]{8})\s+([0-9a-f]{8})", line)
        if not m or i + 1 >= len(lines):
            continue
        flags = {f.strip() for f in lines[i + 1].split(",")}
        secs[m.group(1)] = (int(m.group(3), 16), int(m.group(2), 16), flags)
    return secs


def nm_all(nm, elf):
    """[(addr, type, name)] for defined symbols."""
    out = run([nm, elf])
    syms = []
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 3:
            syms.append((int(parts[0], 16), parts[1], parts[2]))
    return syms


def nm_sizes(nm, elf):
    """{name: (addr, size)} for defined symbols that carry a size."""
    out = run([nm, "-S", "--defined-only", elf])
    sized = {}
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 4:
            sized[parts[3]] = (int(parts[0], 16), int(parts[1], 16))
    return sized


def inside(region, lo, hi):
    return region[0] <= lo and hi <= region[0] + region[1]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--nm", required=True)
    ap.add_argument("--objdump", required=True)
    ap.add_argument("elf")
    args = ap.parse_args()

    errors = []
    secs = elf_sections(args.objdump, args.elf)
    syms = nm_all(args.nm, args.elf)
    by_name = {n: a for a, _t, n in syms}

    # 1. every ALLOC section inside a known region; per-region high-water mark
    itcm_end = ITCM[0]
    for name, (vma, size, flags) in secs.items():
        if "ALLOC" not in flags or size == 0:
            continue
        end = vma + size
        if inside(ITCM, vma, end):
            itcm_end = max(itcm_end, end)
        elif inside(DTCM, vma, end) or inside(SRAM_ALL, vma, end):
            pass
        else:
            errors.append(f"section {name} [0x{vma:08x},0x{end:08x}) outside "
                          "every known region")

    itcm_free = ITCM[0] + ITCM[1] - itcm_end
    if itcm_free < ITCM_MIN_HEADROOM:
        errors.append(f"ITCM headroom {itcm_free} B < {ITCM_MIN_HEADROOM} B")

    heap_limit = by_name.get("__HeapLimit")
    stack_limit = by_name.get("__StackLimit")
    stack_top = by_name.get("__StackTop")
    if heap_limit is None or stack_limit is None or stack_top is None:
        errors.append("__HeapLimit/__StackLimit/__StackTop missing")
        dtcm_gap = 0
    else:
        dtcm_gap = stack_limit - heap_limit
        if dtcm_gap < DTCM_MIN_HEADROOM:
            errors.append(f"DTCM heap..stack gap {dtcm_gap} B < "
                          f"{DTCM_MIN_HEADROOM} B")
        if stack_top != DTCM[0] + DTCM[1]:
            errors.append(f"__StackTop 0x{stack_top:08x} is not the DTCM top")

    # 2. vector table
    table = secs.get(".table")
    if table is None:
        errors.append(".table section missing")
    else:
        if table[0] != ITCM[0]:
            errors.append(f".table at 0x{table[0]:08x}, not ITCM base")
        if table[1] < VECTORS:
            errors.append(f".table {table[1]} B < vector table {VECTORS} B")

    # 3. static stacks in DTCM (thread stacks are *_stack arrays in .bss/.data)
    for addr, typ, name in syms:
        if typ.lower() in ("b", "d") and name.endswith("_stack"):
            if not (DTCM[0] <= addr < DTCM[0] + DTCM[1]):
                errors.append(f"stack symbol {name} @0x{addr:08x} not in DTCM")

    # 4. forbidden survivors
    for bad in FORBIDDEN:
        if bad in by_name:
            errors.append(f"forbidden symbol {bad} survived gc-sections "
                          "(something references it)")
    for prefix, what, allowed in FORBIDDEN_PREFIXES:
        hits = sorted(n for n in by_name
                      if n.startswith(prefix) and n not in allowed)
        if hits:
            errors.append(f"{what} API is linked in ({len(hits)} symbol(s), "
                          f"e.g. {hits[0]}); nothing in this port may drive it")

    # 6. required survivors -- the mirror of 4
    for want, what in REQUIRED:
        if want not in by_name:
            errors.append(f"required symbol {want} is NOT in the image: "
                          f"{what} is not called by anything")

    # 5. measurement buffers live where their labels claim, whole and alone
    sized = nm_sizes(args.nm, args.elf)
    for sym, want_size, sec_name, region_name, region in RESIDENCY:
        if sym not in sized:
            errors.append(f"benchmark buffer {sym} is missing from the image")
            continue
        addr, size = sized[sym]
        end = addr + size
        if size != want_size:
            errors.append(f"benchmark buffer {sym} is {size} B, expected "
                          f"{want_size} B -- the gate and the command have "
                          "drifted apart")
        if not (region[0] <= addr and end <= region[0] + region[1]):
            errors.append(f"benchmark buffer {sym} [0x{addr:08x},0x{end:08x}) "
                          f"is not entirely in {region_name} -- it would "
                          "measure the wrong memory under that name")
        if sec_name is not None:
            sec = secs.get(sec_name)
            if sec is None:
                errors.append(f"section {sec_name} is missing")
            elif not (sec[0] <= addr and end <= sec[0] + sec[1]):
                errors.append(f"benchmark buffer {sym} "
                              f"[0x{addr:08x},0x{end:08x}) is not inside its "
                              f"own section {sec_name} "
                              f"[0x{sec[0]:08x},0x{sec[0] + sec[1]:08x})")
    for name in NOBITS_SECTIONS:
        sec = secs.get(name)
        if sec is None:
            errors.append(f"section {name} is missing")
        elif "CONTENTS" in sec[2]:
            errors.append(f"section {name} is LOADable ({sec[1]} B); benchmark "
                          "buffers must stay NOLOAD")

    # 6. no two allocated sections overlap.  A NOLOAD reservation that shares
    #    address space with something else still passes every per-section check
    #    above while the benchmark quietly measures (and overwrites) whatever
    #    the other tenant is.
    placed = sorted(((vma, vma + size, name)
                     for name, (vma, size, flags) in secs.items()
                     if "ALLOC" in flags and size > 0),
                    key=lambda s: s[0])
    for (lo1, hi1, n1), (lo2, hi2, n2) in zip(placed, placed[1:]):
        if lo2 < hi1:
            errors.append(f"sections {n1} [0x{lo1:08x},0x{hi1:08x}) and {n2} "
                          f"[0x{lo2:08x},0x{hi2:08x}) overlap")

    # 6b. nothing with CONTENTS may land in the loader window (issue #29).
    #
    #     This is the check the linker script cannot make.  ld knows where a
    #     section goes but not whether it is NOBITS, so the region split can
    #     express the intent and an ASSERT can cover the one section the script
    #     names -- neither can state the actual rule.  Here the flags are read
    #     back off the linked ELF, so it holds for any section anyone adds
    #     later, including ones this file has never heard of.
    #
    #     The hazard is specific: the 2nd-stage bootloader executes from
    #     0x3401F000 spanning 0x18000 while it loads the application, so a
    #     LOADABLE section down there has the loader writing over the code it
    #     is currently running.  A NOLOAD reservation is fine -- nothing is
    #     written until the app runs, long after the loader is done, which is
    #     what .lcd_fb relies on.
    #
    #     Checked against the whole loader region rather than the 0x18000 the
    #     loader actually occupies: the SDK's own NN script rounds the hazard
    #     up to 0x3404D000, and buying margin costs nothing that is not already
    #     spent on .lcd_fb.
    for name, (vma, size, flags) in secs.items():
        if "ALLOC" not in flags or size == 0 or "CONTENTS" not in flags:
            continue
        if vma < SRAM[0] and inside(SRAM_ALL, vma, vma + size):
            errors.append(
                f"section {name} [0x{vma:08x},0x{vma + size:08x}) has CONTENTS "
                f"and starts below the loadable-SRAM floor 0x{SRAM[0]:08x} -- "
                "the 2nd bootloader executes there while loading")

    # 7. the `free` command's region accounting still covers everything
    #    (issue #26).  `free` reports SRAM usage as __sram_end - ORIGIN and
    #    DTCM statics as __HeapBase - ORIGIN.  Both are high-water marks that
    #    only stay honest while nothing is placed past them -- and the bug this
    #    check exists to prevent is exactly that: a section was added to a
    #    region and `free` went on reporting the old number (0 B of a 64 KB
    #    SRAM reservation) with nothing to catch it.
    sram_end = by_name.get("__sram_end")
    sram_ldr_end = by_name.get("__sram_ldr_end")
    heap_base = by_name.get("__HeapBase")
    if sram_end is None:
        errors.append("__sram_end missing; `free` cannot report SRAM usage")
    if sram_ldr_end is None:
        errors.append("__sram_ldr_end missing; `free` cannot report the "
                      "loader window")
    if heap_base is None:
        errors.append("__HeapBase missing; `free` cannot report DTCM statics")
    for lo, hi, name in placed:
        if sram_end is not None and inside(SRAM, lo, hi) and hi > sram_end:
            errors.append(f"section {name} ends at 0x{hi:08x}, past __sram_end "
                          f"0x{sram_end:08x} -- `free` would under-report SRAM")
        if (sram_ldr_end is not None and inside(SRAM_LDR, lo, hi)
                and hi > sram_ldr_end):
            errors.append(f"section {name} ends at 0x{hi:08x}, past "
                          f"__sram_ldr_end 0x{sram_ldr_end:08x} -- `free` "
                          "would under-report the loader window")
        if (heap_base is not None and inside(DTCM, lo, hi)
                and lo < heap_base < hi):
            errors.append(f"section {name} [0x{lo:08x},0x{hi:08x}) straddles "
                          f"__HeapBase 0x{heap_base:08x} -- `free` would "
                          "mis-report DTCM statics")

    if errors:
        print("check_placement_budget: FAIL", file=sys.stderr)
        for e in errors:
            print(f"  - {e}", file=sys.stderr)
        return 1

    print(f"check_placement_budget: OK (ITCM used {itcm_end - ITCM[0]} B, "
          f"free {itcm_free} B; DTCM heap..stack gap {dtcm_gap} B)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
