#!/usr/bin/env python3
"""Post-link guard for the .plugin reservation in AXI-SRAM (issue #108).

WHY THIS EXISTS
---------------
A plugin is PRELINKED for a fixed address: the loader services no relocations,
so the address is baked into every container ever built for this board.  The
linker script pins the reservation at the top of AXI-SRAM and ASSERTs that the
sequential sections have not grown into it -- and those ASSERTs name section
boundaries, not functions, so LTO cannot rename them away the way it defeats the
.itcm ones.  They are still the linker checking its own script.  This checks the
LINKED IMAGE against a statement made HERE, independently:

  1. the reservation exists as a NOLOAD section at exactly RESERVATION_BASE,
     exactly RESERVATION_SIZE long, and ends at the top of AXI-SRAM;
  2. no other allocated section overlaps it (ld refuses that itself in a
     normal link, NOLOAD included -- measured; this is what still holds when
     the link runs with --no-check-sections);
  3. no symbol other than its own boundary markers lies inside it;
  4. the newlib heap's ceiling (__heap_end, src/retarget.c) is at or below its
     base, and the heap's floor (`end`) is at or below the ceiling;
  5. AND `_sbrk` -- the code that enforces that ceiling -- actually uses it.

[!] THE ADDRESS HERE IS A LITERAL, AND IT MUST STAY ONE.  It is also stated in
the linker script, in the plugin MEMORY fragment and in board.cmake's arguments
to the image gate.  A gate that read its expected value out of what it is
checking would pass anything; generating these from one variable would turn four
statements that can disagree into one that cannot.

[!] WHY THE HEAP IS PART OF IT.  Before issue #108 the heap's ceiling was
__ram_end, the top of AXI-SRAM -- which is now inside the reservation.  A heap
still bounded that way could grow into a plugin's code, and no ASSERT about
section placement would notice, because the heap is not a section.

[!] AND CHECK 4 ALONE WOULD NOT HAVE CAUGHT THAT.  It validates a SYMBOL: put
`_sbrk` back on __ram_end and __heap_end is still defined, still at the base,
and check 4 still passes (the #108 adversarial review).  So check 5 reads the
implementation -- `_sbrk`'s own 32-bit constants (literal-pool words and
movw/movt pairs) -- and requires that it names the ceiling's address and names
nothing above it inside AXI-SRAM.  It is text over objdump's output, so it is
written to fail CLOSED: a disassembly it cannot read yields no constants, and
"the ceiling is not among them" is a refusal, not a pass.

[!] WHAT CHECK 5 DOES NOT PROVE: that the ceiling GOVERNS the comparison.  An
_sbrk that names __heap_end for some other purpose while taking its real bound
through a call or a variable, or that does arithmetic on the ceiling after
loading it, passes (round 2 of the same review).  A disassembly is the wrong
tool for data flow, so that half is checked where it can be -- by behaviour:
test/test_sbrk.c compiles the real src/retarget.c on the host with __ram_end
placed above the ceiling and requires the break to stop at the ceiling.  The
two together cover what either alone does not: the source behaves, and the
linked image names the right bound and nothing above it.

Exit status 0 = pass; 1 = a placement failure; 2 = the check could not be
performed (which is also a failure -- a gate that skips itself reports the same
silence as a passing one).
"""

import argparse
import re
import subprocess
import sys

CANNOT_CHECK = 2

# The reservation, stated here as its own literals.  See the module docstring.
RESERVATION_BASE = 0x24048000
RESERVATION_SIZE = 0x8000
# AXI-SRAM (D1).  The reservation is anchored at its top.
AXI_END = 0x24000000 + 320 * 1024

# The only symbols allowed at or inside [base, base + size): the section's own
# markers, and the heap ceiling, which is DEFINED as the base.
MARKERS = {"__plugin_start", "__plugin_end", "__heap_end"}


def run(cmd):
    return subprocess.run(cmd, check=True, capture_output=True,
                          text=True).stdout


def sections(objdump, elf):
    """[(name, vma, size, {flags})]"""
    lines = run([objdump, "-h", elf]).splitlines()
    out = []
    for i, line in enumerate(lines):
        m = re.match(r"^\s*\d+\s+(\S+)\s+([0-9a-f]{8})\s+([0-9a-f]{8})", line)
        if not m or i + 1 >= len(lines):
            continue
        flags = {f.strip() for f in lines[i + 1].split(",")}
        out.append((m.group(1), int(m.group(3), 16), int(m.group(2), 16),
                    flags))
    return out


def symbols(nm, elf):
    """{name: address} for every symbol nm reports with an address."""
    out = {}
    for line in run([nm, elf]).splitlines():
        parts = line.split()
        if len(parts) == 3:
            out.setdefault(parts[2], []).append(int(parts[0], 16))
    return out


def function_constants(objdump, elf, name):
    """The 32-bit constants `name` materialises, or None if it is not there.

    Two encodings: a literal-pool word (objdump prints `.word 0x...` inside the
    function, since the pool sits after its code) and a movw/movt pair on one
    register.  Anything else yields nothing -- which the caller treats as the
    ceiling NOT being found, i.e. a refusal.
    """
    out = run([objdump, "-d", "-j", ".text", elf])
    body, inside = [], False
    for line in out.splitlines():
        m = re.match(r"^[0-9a-f]+ <([^>]+)>:", line)
        if m:
            if inside:
                break
            inside = m.group(1) == name
            continue
        if inside:
            body.append(line)
    if not inside and not body:
        return None
    consts, low = set(), {}
    for line in body:
        m = re.search(r"\.word\s+0x([0-9a-f]+)", line)
        if m:
            consts.add(int(m.group(1), 16))
            continue
        m = re.search(r"\bmovw\s+(\w+),\s*#(\d+)", line)
        if m:
            low[m.group(1)] = int(m.group(2))
            continue
        m = re.search(r"\bmovt\s+(\w+),\s*#(\d+)", line)
        if m and m.group(1) in low:
            consts.add((int(m.group(2)) << 16) | low[m.group(1)])
    return consts


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("elf")
    ap.add_argument("--nm", required=True)
    ap.add_argument("--objdump", required=True)
    args = ap.parse_args()

    base, end = RESERVATION_BASE, RESERVATION_BASE + RESERVATION_SIZE
    try:
        secs = sections(args.objdump, args.elf)
        syms = symbols(args.nm, args.elf)
    except (OSError, subprocess.CalledProcessError) as exc:
        print(f"check_plugin_reservation: CANNOT CHECK ({exc})",
              file=sys.stderr)
        return CANNOT_CHECK

    errors = []

    # 1. the reservation itself
    plug = [s for s in secs if s[0] == ".plugin"]
    if len(plug) != 1:
        errors.append(f".plugin: expected exactly one section, found "
                      f"{len(plug)}")
    else:
        _, vma, size, flags = plug[0]
        if vma != base or size != RESERVATION_SIZE:
            errors.append(f".plugin is [0x{vma:08x}, 0x{vma + size:08x}); "
                          f"this gate states [0x{base:08x}, 0x{end:08x}) -- "
                          "a prelinked address may not move")
        if "CONTENTS" in flags or "LOAD" in flags:
            errors.append(".plugin carries contents; it must be NOLOAD (the "
                          "image arrives at run time)")
    if end != AXI_END:
        errors.append(f"the reservation ends at 0x{end:08x}, not at the top of "
                      f"AXI-SRAM (0x{AXI_END:08x})")

    # 2. nothing else allocated overlaps it
    for name, vma, size, flags in secs:
        if name == ".plugin" or "ALLOC" not in flags or size == 0:
            continue
        if vma < end and vma + size > base:
            errors.append(f"section {name} [0x{vma:08x}, 0x{vma + size:08x}) "
                          "overlaps the .plugin reservation")

    # 3. no symbol inside it but the markers
    for name, addrs in syms.items():
        if name in MARKERS:
            continue
        for a in addrs:
            if base <= a < end:
                errors.append(f"symbol {name} at 0x{a:08x} lies inside the "
                              ".plugin reservation")

    # 4. the heap stays below it
    def one(name):
        addrs = syms.get(name, [])
        if len(addrs) != 1:
            errors.append(f"symbol {name}: expected one definition, found "
                          f"{len(addrs)} -- the heap bound cannot be checked")
            return None
        return addrs[0]

    heap_end = one("__heap_end")
    heap_floor = one("end")
    if heap_end is not None and heap_end > base:
        errors.append(f"__heap_end 0x{heap_end:08x} is above the reservation's "
                      f"base 0x{base:08x}: the heap could grow into a plugin")
    if heap_end is not None and heap_floor is not None and \
            heap_floor > heap_end:
        errors.append(f"the heap starts at 0x{heap_floor:08x}, above its own "
                      f"ceiling 0x{heap_end:08x}")

    # 5. _sbrk uses that ceiling, and nothing above it
    try:
        consts = function_constants(args.objdump, args.elf, "_sbrk")
    except subprocess.CalledProcessError as exc:
        print(f"check_plugin_reservation: CANNOT CHECK ({exc})",
              file=sys.stderr)
        return CANNOT_CHECK
    if consts is None:
        errors.append("_sbrk: not in the image -- the heap's bound cannot be "
                      "checked where it is enforced")
    elif heap_end is not None:
        if heap_end not in consts:
            errors.append(f"_sbrk does not name __heap_end (0x{heap_end:08x}) "
                          "-- whatever bounds the heap, it is not the ceiling "
                          "below the reservation")
        above = sorted(c for c in consts if base < c <= AXI_END)
        if above:
            errors.append("_sbrk names " + ", ".join(f"0x{c:08x}" for c in above)
                          + f", above the heap ceiling 0x{base:08x} -- the heap "
                          "could grow into a plugin")

    if errors:
        print("check_plugin_reservation: FAIL", file=sys.stderr)
        for e in errors:
            print("  - " + e, file=sys.stderr)
        return 1

    gap = heap_end - heap_floor
    print(f"check_plugin_reservation: OK (.plugin 0x{base:08x}+0x"
          f"{RESERVATION_SIZE:x}, heap 0x{heap_floor:08x}..0x{heap_end:08x} = "
          f"{gap} B, bounded by _sbrk)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
