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
     uncalled function is dropped -- so if EPII_Set_Systick_* (the SDK's
     SysTick pokers) or console_getchar/console_putchar (the SDK clib console
     this port does not link) are PRESENT in the image, something references
     them, which violates the port's design.

Stdlib-only; POST_BUILD.
"""

import argparse
import re
import subprocess
import sys

ITCM = (0x10000000, 0x40000)
DTCM = (0x30000000, 0x40000)
SRAM = (0x3401F000, 0x1E1000)

ITCM_MIN_HEADROOM = 8 * 1024
DTCM_MIN_HEADROOM = 8 * 1024      # gap between __HeapLimit and __StackLimit

VECTORS = 496 * 4

FORBIDDEN = [
    "EPII_Set_Systick_load",
    "EPII_Set_Systick_enable",
    "console_getchar",
    "console_putchar",
]


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
        elif inside(DTCM, vma, end) or inside(SRAM, vma, end):
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
