#!/usr/bin/env python3
"""Gate a plugin image before it is ever allowed near the device (issue #101).

The reviewed plan settled the trust boundary as "apply the firmware's gates to
the plugin too" rather than "declare the plugin out of scope" -- the checks do
not stop, their target widens.

[!] SHARED SINCE ISSUE #108, AND THE BOARD'S FACTS ARE ARGUMENTS.  Three things
differ per board and every one is required on the command line -- a board that
omits one fails before anything is checked, rather than inheriting another
board's value:

  --base/--end         the reservation the plugin is prelinked for.  Stated by
                       the board SEPARATELY from its MEMORY fragment -- a gate
                       that read the expected value out of what it is checking
                       would pass anything, so the two must never come from one
                       variable.
  --forbid             the board's table of entry points a plugin may never
                       reach.
  --veneer-base-cost   the stack charged for the firmware work behind an
                       indirect veneer.  That is the BASE's cost, not the
                       plugin's, so one board's number silently becoming
                       another's would make every bound derived here a guess.

Everything else -- the section whitelist, the veneer set, the disassembly and
the call-graph walk -- is the ABI's and is the same for every board.

[!] AND THIS DOES NOT PROVE MEMORY SAFETY.  It cannot see an ordinary
out-of-bounds write, a bad tensor pointer, scratch overrun, or wrong arithmetic
on a base pointer the vtable legitimately handed over.  It also only ever sees
the plugin the BUILD produced; a hand-written container never passes through
here at all.  A plugin is REVIEWED, TRUSTED NATIVE CODE, with the standing of
board code on whichever board runs it -- each board's README says what that
standing is there.  Do not read a pass here as isolation.

Checks:
  1. no undefined symbols          -- a plugin resolves everything within itself
  2. allocated-section whitelist   -- nothing the loader would have to service
  3. no relocation sections        -- the loader fixes up nothing
  4. no forbidden symbols          -- the board's table, applied here
  5. (no MMIO check: not soundly possible -- see the note in main)
  6. storage lives inside the declared segments, and no COMMON
  7. indirect branches only inside the named veneers
  8. a transitive stack bound per entry point, fail-closed
"""
import argparse
import re
import subprocess
import sys

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
# [!] THE OFFSET IS CAPTURED, BECAUSE `b <f+0x8>` AND `b <f>` ARE NOT THE SAME
# THING.  objdump renders an ordinary loop inside f as a branch to `<f+0x8>` and
# a self tail call as a branch to `<f>`, and the first draft of the walk below
# told them apart by skipping EVERY call whose target was the current function.
# That skipped genuine direct recursion too -- the one thing the walk exists to
# refuse -- and the recursion fixture in cmake/fixtures went on passing because
# the clone-naming bug above happened to refuse its image for an unrelated
# reason.  Two mistakes cancelling is how a gate ends up never having been seen
# to fail for its own reason.
DIRECT_CALL_RE = re.compile(r"\bbl\s+[0-9a-f]+\s+<([^>+]+)(\+0x[0-9a-f]+)?>")
TAIL_CALL_RE = re.compile(
    r"\bb(?:\.[nw])?\s+[0-9a-f]+\s+<([^>+]+)(\+0x[0-9a-f]+)?>")


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


# [!] GCC SPELLS AN INTERPROCEDURAL CLONE TWO WAYS, AND THIS IS NOT A GUESS.
# ipa-cp, ipa-sra and partial inlining emit extra bodies for a function; the ELF
# calls one `pl_utoa.constprop.0` and -fstack-usage calls the SAME body
# `pl_utoa.constprop`, without the index.  The analyser looked the ELF name up
# verbatim, found nothing and failed closed -- correct behaviour on a wrong
# premise, and it went unseen until a second plugin gave a static helper three
# call sites with a constant argument.  Matching a clone to its PARENT would be
# a guess; matching the two spellings of one clone is not.  Where a name really
# is ambiguous (two clones, one .su name) the larger frame is kept, which is the
# direction a bound may err in.
CLONE_SUFFIX_RE = re.compile(
    r"^(.*\.(?:constprop|isra|part|cold|lto_priv|localalias))\.\d+$")


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
                entry = (int(parts[1]), parts[2])
                prev = frames.get(name)
                if prev is None or su_rank(entry) > su_rank(prev):
                    frames[name] = entry
    return frames


def su_rank(entry):
    """Order two .su records for the same name; the larger one is kept.

    [!] A NON-STATIC QUALIFIER OUTRANKS EVERY BYTE COUNT.  Two clones can share
    one normalised name, and merging them by frame size alone throws away the
    only thing that matters if one of them is `dynamic` or `bounded`: that it
    has no static bound at all.  A small dynamic record losing to a large static
    one would hand `frame_of()` a finite number for a body that has none, and
    the gate would state a bound where there is not one -- which is the fail-open
    direction, and the reason a merge exists here at all is a naming quirk, not
    a licence to summarise evidence.
    """
    frame, qual = entry
    return (0 if qual == "static" else 1, frame)


def frame_of(fn, frames):
    """The .su entry for an ELF symbol, or None.  See CLONE_SUFFIX_RE."""
    if fn in frames:
        return frames[fn]
    m = CLONE_SUFFIX_RE.match(fn)
    if m and m.group(1) in frames:
        return frames[m.group(1)]
    return None


def bound_stack(entry, funcs, frames, errors, veneer_base_cost):
    """Transitive stack bound below `entry`, fail-closed on anything unclear.

    `veneer_base_cost` is what the base itself may spend below a veneer, worst
    case.  It is added at each veneer because the walk cannot see across the
    boundary, and it is the BOARD's number (see the module docstring).
    """
    seen = set()

    def walk(fn, path):
        if fn in path:
            errors.append(f"stack: recursion through {fn} ({' -> '.join(path)})")
            return 0
        su = frame_of(fn, frames)
        if su is None:
            errors.append(f"stack: no frame recorded for {fn} -- the .su input "
                          "does not match the linked image")
            return 0
        frame, qual = su
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
                worst = max(worst, veneer_base_cost)
                continue
            m = DIRECT_CALL_RE.search(line) or TAIL_CALL_RE.search(line)
            if m:
                callee = m.group(1)
                # A branch into the MIDDLE of this same function is a loop, not
                # a call; a branch to its entry is recursion and is walked, so
                # the `fn in path` test above refuses it.
                if callee == fn and m.group(2):
                    continue
                if callee in funcs or frame_of(callee, frames) is not None:
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
    ap.add_argument("--emit-stacks",
                    help="write the derived bounds here, for the packer")
    # The board's facts.  Required, every one: see the module docstring.
    ap.add_argument("--base", required=True, type=lambda v: int(v, 0),
                    help="the reservation's first byte, as the board states it")
    ap.add_argument("--end", required=True, type=lambda v: int(v, 0),
                    help="one past the reservation's last byte")
    ap.add_argument("--forbid", nargs="+", required=True,
                    help="entry points a plugin may never reach on this board")
    ap.add_argument("--veneer-base-cost", required=True, type=int,
                    help="stack the base may spend below one veneer, in bytes")
    args = ap.parse_args()
    if args.end <= args.base:
        print("check_plugin_image: FAIL\n  - --end 0x%08x is not above --base "
              "0x%08x" % (args.end, args.base), file=sys.stderr)
        return 1
    forbidden = set(args.forbid)

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
            if vma < args.base or vma + size > args.end:
                errors.append(f"section {name} [0x{vma:08x},0x{vma + size:08x}) "
                              "is outside the plugin reservation")

    # 4. forbidden symbols, defined or referenced
    for line in run([args.nm, args.elf]).splitlines():
        parts = line.split()
        if parts and parts[-1] in forbidden:
            errors.append(f"forbidden symbol {parts[-1]}")

    # 5. THERE IS NO MMIO CHECK, AND THAT IS A FINDING, NOT AN OMISSION.
    #
    # The trust boundary as planned listed "no MMIO address constants".  It
    # cannot be implemented soundly, and a check that cannot do its job is worse
    # than none: it gets believed.  A literal-pool word is a CONSTANT, not
    # necessarily an address -- the first draft flagged 0x447a0000 and
    # 0x3c000000, which are 1000.0f and 0.0078125f, the decoder's own scale
    # factors, and the bytes of a string.  Nothing distinguishes those from a
    # pointer by inspection.  Whether a board has a second, independent reason
    # (Grove does: its peripherals share the 0x34 window with the reservation)
    # is recorded in that board's README, not here.
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
            got = bound_stack(name, funcs, frames, errors,
                              args.veneer_base_cost)
            bounds[name] = got
            if limit and got > int(limit):
                errors.append(f"stack: {name} needs {got} B, limit {limit} B")

    if errors:
        print("check_plugin_image: FAIL", file=sys.stderr)
        for e in errors:
            print("  - " + e, file=sys.stderr)
        return 1

    # [!] THE MANIFEST'S STACK NUMBERS ARE THIS ANALYSIS, not a second opinion.
    # Handing the packer the bounds derived here is what makes "the manifest
    # agrees with the gate" true by construction; the check that still has teeth
    # is the DEVICE comparing those numbers against what its threads can spare,
    # which no host knows.
    if args.emit_stacks:
        import json
        with open(args.emit_stacks, "w") as fh:
            json.dump(bounds, fh, indent=2, sort_keys=True)

    t = secs.get(".text", (0, 0, set()))[1]
    d = secs.get(".data", (0, 0, set()))[1]
    b = secs.get(".bss", (0, 0, set()))[1]
    summary = ", ".join(f"{k} {v} B" for k, v in sorted(bounds.items()))
    print(f"check_plugin_image: OK (text {t} B, data {d} B, bss {b} B"
          + (f"; stack {summary}" if summary else "") + ")")
    return 0


if __name__ == "__main__":
    sys.exit(main())
