#!/usr/bin/env python3
"""Gate a plugin image before it is ever allowed near the device (issue #101).

A plugin is loaded from a rewritable blob and runs Secure and privileged, with
the same standing as board code.  The reviewed plan settled the trust boundary
as "apply the firmware's gates to the plugin too" rather than "declare the
plugin out of scope" -- the checks do not stop, their target widens.

[!] AND THIS DOES NOT PROVE MEMORY SAFETY.  It cannot see an ordinary
out-of-bounds write, a bad tensor pointer, scratch overrun, or wrong arithmetic
on a base pointer the vtable legitimately handed over.  It also only ever sees
the plugin the BUILD produced; a hand-written container never passes through
here at all.  A plugin is REVIEWED, TRUSTED NATIVE CODE.  Do not read a pass
here as isolation -- the same caveat AGENTS.md records for the NOR absence
check, with a wider reach.

Checks:
  1. no undefined symbols          -- a plugin resolves everything within itself
  2. allocated-section whitelist   -- nothing the loader would have to service
  3. no relocation sections        -- the loader fixes up nothing
  4. no forbidden symbols          -- the firmware's table, applied here
  5. (no MMIO check: not soundly possible here -- see the note in main)
  6. storage lives inside the declared segments, and no COMMON
  7. indirect branches only inside the named veneers
  8. a transitive stack bound per entry point, fail-closed
"""
import argparse
import re
import subprocess
import sys

# The reservation a plugin is prelinked for.  Stated here independently of the
# linker scripts for the same reason check_placement_budget.py states it: a gate
# that read the expected value out of what it is checking would pass anything.
PLUGIN_BASE = 0x341E0000
PLUGIN_END = 0x34200000

# Allocated sections a plugin image may contain.  Anything else either needs a
# loader service that does not exist (init_array wants constructors run, .got
# wants fixups, .ARM.exidx wants an unwinder) or is content in a place the
# format does not describe.
ALLOWED_ALLOC = {".text", ".data", ".bss"}

# Sections that must not exist at all, allocated or not.  Listed by name as well
# as caught by the whitelist above so the message can say WHY.
BANNED_SECTIONS = {
    ".init_array": "constructors: nothing runs them",
    ".fini_array": "destructors: nothing runs them",
    ".ARM.exidx": "unwind tables: there is no unwinder",
    ".ARM.extab": "unwind tables: there is no unwinder",
    ".eh_frame": "unwind tables: there is no unwinder",
    ".got": "a global offset table: the loader fixes up nothing",
    ".plt": "a procedure linkage table: the loader fixes up nothing",
    ".dynamic": "dynamic linking: there is no dynamic linker",
    ".tdata": "thread-local storage: a plugin has no thread of its own",
    ".tbss": "thread-local storage: a plugin has no thread of its own",
}

# Vendor entry points a plugin may never reach.  The firmware keeps the same
# table; the NOR write path is the one that matters most, because that flash
# holds the bootloader.
FORBIDDEN_SYMBOLS = {
    "hx_lib_qspi_eeprom_erase_sector",
    "hx_lib_qspi_eeprom_write",
    "hx_lib_qspi_eeprom_erase_all",
    "hx_lib_qspi_eeprom_word_write",
    "hx_lib_spi_eeprom_erase_sector",
    "hx_lib_spi_eeprom_write",
    "hx_lib_spi_eeprom_erase_all",
    "hx_lib_spi_eeprom_word_write",
    "Send_Op_code",
    "Send_Op_Read_Data",
    "hx_lib_pm_enter_lp",
    "hx_lib_pm_enter_ulp",
    "EPII_NVIC_SetVector",
    "NVIC_EnableIRQ",
    "NVIC_DisableIRQ",
    "SCB_EnableDCache",
    "SCB_DisableDCache",
    "SCB_InvalidateICache",
    "ARM_MPU_Enable",
    "ARM_MPU_Disable",
    "ARM_MPU_SetRegion",
}

# The ONLY functions allowed to contain an indirect branch.  Every base, painter
# and printer call funnels through one of these so that the stack analyser sees
# an ordinary direct edge and exactly one accounted indirect site.  See
# plugin_base.c.
VENEERS = {
    "pl_base_log",
    "pl_base_to_frame",
    "pl_paint_rect",
    "pl_paint_fill_rect",
    "pl_paint_blit",
    "pl_print_write",
}

# What the base itself may spend below a veneer, per slot, worst case.  The
# analyser adds this at each veneer because it cannot see across the boundary.
# Deliberately generous: it is added once per veneer and the whole point is that
# a plugin must not be sized against an optimistic guess.
VENEER_BASE_COST = 256

# [!] REGISTER NAMES, NOT NUMBERS.  objdump spells r12 as `ip`, r13 `sp`, r14
# `lr` and r15 `pc`, and it used exactly that spelling for the painter veneer's
# tail call (`bx ip`).  A scan written as r[0-9]+ misses those and reports an
# image with unaccounted indirect branches as clean -- which is what the first
# draft of this file did, on this very plugin.
REG = r"(?:r\d+|ip|sp|lr|pc|fp|sl)"
# [!] `bx lr` IS A RETURN, NOT A CALL.  Flagging it made the first run report
# every leaf function in the decoder as containing an indirect branch -- forty
# findings, none of them real, from a check that was one register name away from
# being useless noise.  An indirect CALL is `blx <reg>`, or `bx <reg>` to
# anything but lr (a tail call).
INDIRECT_RE = re.compile(r"\bblx\s+" + REG + r"\b|\bbx\s+(?!lr\b)" + REG + r"\b")
DIRECT_CALL_RE = re.compile(r"\bbl\s+[0-9a-f]+\s+<([^>+]+)")
TAIL_CALL_RE = re.compile(r"\bb(?:\.[nw])?\s+[0-9a-f]+\s+<([^>+]+)")


def run(cmd):
    return subprocess.run(cmd, check=True, capture_output=True,
                          text=True).stdout


def sections(objdump, elf):
    """{name: (vma, size, {flags})}"""
    out = run([objdump, "-h", elf])
    lines = out.splitlines()
    secs = {}
    for i, line in enumerate(lines):
        m = re.match(r"^\s*\d+\s+(\S+)\s+([0-9a-f]{8})\s+([0-9a-f]{8})", line)
        if not m or i + 1 >= len(lines):
            continue
        flags = {f.strip() for f in lines[i + 1].split(",")}
        secs[m.group(1)] = (int(m.group(3), 16), int(m.group(2), 16), flags)
    return secs


def disassemble(objdump, elf):
    """{function: [text lines]} plus the literal words objdump decoded."""
    out = run([objdump, "-d", "-j", ".text", elf])
    funcs, cur = {}, None
    for line in out.splitlines():
        m = re.match(r"^[0-9a-f]+ <([^>]+)>:", line)
        if m:
            cur = m.group(1)
            funcs[cur] = []
        elif cur and "\t" in line:
            funcs[cur].append(line)
    return funcs


def stack_usage(su_files):
    """{function: (frame, qualifier)} from -fstack-usage output."""
    frames = {}
    for path in su_files:
        with open(path) as fh:
            for line in fh:
                parts = line.rstrip("\n").split("\t")
                if len(parts) < 3:
                    continue
                name = parts[0].rsplit(":", 1)[-1]
                frames[name] = (int(parts[1]), parts[2])
    return frames


def bound_stack(entry, funcs, frames, errors):
    """Transitive stack bound below `entry`, fail-closed on anything unclear."""
    seen = set()

    def walk(fn, path):
        if fn in path:
            errors.append(f"stack: recursion through {fn} ({' -> '.join(path)})")
            return 0
        if fn not in frames:
            errors.append(f"stack: no frame recorded for {fn} -- the .su input "
                          "does not match the linked image")
            return 0
        frame, qual = frames[fn]
        if qual != "static":
            errors.append(f"stack: {fn} has a {qual} frame (alloca/VLA); a "
                          "bound cannot be stated")
            return 0
        seen.add(fn)
        worst = 0
        for line in funcs.get(fn, []):
            if INDIRECT_RE.search(line):
                # Only ever legal inside a veneer, which check 7 enforces; the
                # base's own worst case is charged here.
                worst = max(worst, VENEER_BASE_COST)
                continue
            m = DIRECT_CALL_RE.search(line) or TAIL_CALL_RE.search(line)
            if m and m.group(1) != fn:
                callee = m.group(1)
                if callee in funcs or callee in frames:
                    worst = max(worst, walk(callee, path + [fn]))
        return frame + worst

    return walk(entry, [])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("elf")
    ap.add_argument("--nm", required=True)
    ap.add_argument("--objdump", required=True)
    ap.add_argument("--su", nargs="*", default=[])
    ap.add_argument("--entry", nargs="*", default=[],
                    help="functions to bound the stack below, name=limit")
    args = ap.parse_args()

    errors = []
    secs = sections(args.objdump, args.elf)
    funcs = disassemble(args.objdump, args.elf)

    # 1. undefined symbols
    und = [l.split()[-1] for l in run([args.nm, "-u", args.elf]).splitlines()
           if l.strip()]
    for u in und:
        errors.append(f"undefined symbol {u} -- a plugin resolves everything "
                      "within itself (libgcc does not provide memset; libc does)")

    # 2/3. sections
    for name, (vma, size, flags) in secs.items():
        base = name.split(".")[0:2]
        stem = "." + base[1] if len(base) > 1 else name
        if name in BANNED_SECTIONS:
            errors.append(f"section {name}: {BANNED_SECTIONS[name]}")
        if name.startswith(".rel"):
            errors.append(f"section {name}: relocations survive; the loader "
                          "services none")
        if "ALLOC" in flags and size and stem not in ALLOWED_ALLOC:
            errors.append(f"allocated section {name} is not one of "
                          f"{sorted(ALLOWED_ALLOC)}")
        if "ALLOC" in flags and size:
            if vma < PLUGIN_BASE or vma + size > PLUGIN_END:
                errors.append(f"section {name} [0x{vma:08x},0x{vma + size:08x}) "
                              "is outside the plugin reservation")

    # 4. forbidden symbols, defined or referenced
    for line in run([args.nm, args.elf]).splitlines():
        parts = line.split()
        if parts and parts[-1] in FORBIDDEN_SYMBOLS:
            errors.append(f"forbidden symbol {parts[-1]}")

    # 5. THERE IS NO MMIO CHECK, AND THAT IS A FINDING, NOT AN OMISSION.
    #
    # The trust boundary as planned listed "no MMIO address constants".  It
    # cannot be implemented soundly on this part, for two independent reasons,
    # and a check that cannot do its job is worse than none: it gets believed.
    #
    #   a. A literal-pool word is a CONSTANT, not necessarily an address.  The
    #      first draft flagged 0x447a0000 and 0x3c000000 -- 1000.0f and
    #      0.0078125f, the decoder's own scale factors -- and the bytes of the
    #      string "blazeface: decode refused".  Nothing distinguishes those from
    #      a pointer by inspection.
    #   b. Even a perfect address decoder would have nothing to compare against:
    #      on the HX6538 the peripherals live at 0x34001000, 0x34080000,
    #      0x340C0000, 0x34100000 -- INSIDE the same 0x34 window as the SRAM the
    #      plugin legitimately occupies.  There is no aperture to test for.
    #
    # This is the same call issue #42/#66 made when the MVE predication scan
    # turned out to decode nothing: the gate was deleted rather than kept as
    # reassurance.  Reaching MMIO from a plugin is therefore NOT prevented here,
    # and AGENTS.md says so.

    # 6. storage stays in the declared segments, and nothing is COMMON
    for line in run([args.nm, "-S", args.elf]).splitlines():
        parts = line.split()
        if len(parts) >= 2 and parts[-2] in ("C", "c"):
            errors.append(f"COMMON symbol {parts[-1]} -- it belongs to no "
                          "section and the manifest cannot describe it")

    # 7. indirect branches only inside the veneers
    for fn, lines in funcs.items():
        for line in lines:
            if INDIRECT_RE.search(line) and fn not in VENEERS:
                errors.append(f"{fn} contains an indirect branch ({line.strip()})"
                              " -- only the named veneers may, or the stack "
                              "bound is not a bound")

    # 8. stack bounds
    frames = stack_usage(args.su) if args.su else {}
    bounds = {}
    if args.entry:
        if not frames:
            errors.append("stack: --entry given without --su; a bound cannot be "
                          "derived from the ELF alone")
        for spec in args.entry:
            name, _, limit = spec.partition("=")
            got = bound_stack(name, funcs, frames, errors)
            bounds[name] = got
            if limit and got > int(limit):
                errors.append(f"stack: {name} needs {got} B, limit {limit} B")

    if errors:
        print("check_plugin_image: FAIL", file=sys.stderr)
        for e in errors:
            print("  - " + e, file=sys.stderr)
        return 1

    t = secs.get(".text", (0, 0, set()))[1]
    d = secs.get(".data", (0, 0, set()))[1]
    b = secs.get(".bss", (0, 0, set()))[1]
    summary = ", ".join(f"{k} {v} B" for k, v in sorted(bounds.items()))
    print(f"check_plugin_image: OK (text {t} B, data {d} B, bss {b} B"
          + (f"; stack {summary}" if summary else "") + ")")
    return 0


if __name__ == "__main__":
    sys.exit(main())
