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
  0. the board's target word describes what the image was built for
  1. no undefined symbols          -- a plugin resolves everything within itself
  2. allocated-section whitelist   -- nothing the loader would have to service
  3. no relocation sections        -- the loader fixes up nothing
  4. no forbidden symbols          -- the board's table, applied here
  5. (no MMIO check: not soundly possible -- see the note in main)
  6. storage lives inside the declared segments, and no COMMON
  7. indirect branches only inside the named veneers
  8. a transitive stack bound per entry point, fail-closed -- emitted in
     parts that do not contain the board's veneer cost (issue #111)
"""
import argparse
import re
import struct
import subprocess
import sys

# ---- the target word (issue #108) ------------------------------------------
#
# The board declares one word (svc/plugin_abi.h, plugin_target_id()) and hands
# it to the packer, the host container verifier and its firmware policy.  Until
# #108 nothing checked the word itself: the three agreed because they read the
# same CMake variable.  This half derives what the PLUGIN IMAGE says about
# itself and compares; the firmware's half (svc/plugin_target.h) derives the
# environment from its own predefined macros.
#
# [!] THESE VALUES ARE THE ABI'S, TRANSCRIBED -- and pinned.  The gate runs at
# plugin link time with no host C compiler guaranteed, so it cannot read
# plugin_abi.h itself; cmake/fixtures/run_plugin_gate_tests.py compiles a
# program against the real header and fails if any entry here disagrees.  Keyed
# by the header's own names so that comparison is mechanical.
ABI = {
    "PLUGIN_CPU_CORTEX_M7": 1,
    "PLUGIN_CPU_CORTEX_M55": 2,
    "PLUGIN_FPU_NONE": 0,
    "PLUGIN_FPU_FPV5_SP_D16": 1,
    "PLUGIN_FPU_FPV5_D16": 2,
    "PLUGIN_FPU_FP_ARMV8": 3,
    "PLUGIN_FLOAT_ABI_SOFT": 0,
    "PLUGIN_FLOAT_ABI_HARD": 1,
    "PLUGIN_TARGET_CPU_SHIFT": 0,
    "PLUGIN_TARGET_CPU_MASK": 0x000000FF,
    "PLUGIN_TARGET_FPU_SHIFT": 8,
    "PLUGIN_TARGET_FPU_MASK": 0x00000F00,
    "PLUGIN_TARGET_FLOAT_SHIFT": 12,
    "PLUGIN_TARGET_FLOAT_MASK": 0x00003000,
    "PLUGIN_TARGET_BIG_ENDIAN": 0x00004000,
    "PLUGIN_TARGET_CMSE": 0x00008000,
    "PLUGIN_TARGET_RESERVED_MASK": 0xFFFF0000,
    # What the walk below MEANS (issue #111): stamped into --emit-stacks and from
    # there into the manifest, and compared for equality by the loader.  Raise it
    # in svc/plugin_abi.h -- and here, where the fixture will insist on it -- when
    # the walk changes what it charges or where it stops.
    "PLUGIN_STACK_ACCOUNTING": 1,
    # The ABI each plugin TU records (svc/plugin_abi.h, pl_abi_mark): the
    # image is refused unless every record is this (issue #111).
    "PLUGIN_ABI_VERSION": 2,
}

# (Tag_CPU_arch, Tag_FP_arch, single-precision only) -> (the ABI's CPU, the ABI's
# FPU, the Tag_CPU_name the image must ALSO carry, or None).
#
# [!] THE KEY IS THE ARCHITECTURE AND THE FPU, NOT THE CPU NAME, and the first
# version of this table (issue #108) had it the other way round.  Measured with
# the pinned GCC 15.2: for -mcpu=cortex-m7 the compiler emits `.cpu cortex-m7`
# and then `.arch armv7e-m`, and the second wins -- a Cortex-M7 object records
# Tag_CPU_name "7E-M", exactly what a Cortex-M4 records.  The name-keyed table
# refused every M7 plugin (which is the direction it should fail in), and a
# comment beside it claimed the name "does distinguish them".  Nothing had ever
# run it on an M7 image.
#
# What does distinguish them on v7E-M is the FPU: the Cortex-M4's is FPv4
# (Tag_FP_arch 6), and FPv5 on v7E-M exists only on the Cortex-M7.  On v8.1-M the
# reverse holds -- a Cortex-M85 records the same architecture and the same FPU as
# the M55 -- but there the compiler DOES record the core ("cortex-m55" /
# "cortex-m85"), so the name is required.  A key not listed here is refused
# rather than guessed, which includes any image with no FPU at all (a soft-float
# M7 is indistinguishable from an M4).
TARGET_BY_ATTR = {
    (13, 8, True):  ("PLUGIN_CPU_CORTEX_M7", "PLUGIN_FPU_FPV5_SP_D16", None),
    (13, 8, False): ("PLUGIN_CPU_CORTEX_M7", "PLUGIN_FPU_FPV5_D16", None),
    (21, 8, False): ("PLUGIN_CPU_CORTEX_M55", "PLUGIN_FPU_FP_ARMV8",
                     "cortex-m55"),
}
ARCH_NAME = {13: "v7E-M", 21: "v8.1-M.mainline"}


def _uleb(buf, pos):
    val = shift = 0
    while True:
        b = buf[pos]
        pos += 1
        val |= (b & 0x7F) << shift
        shift += 7
        if not b & 0x80:
            return val, pos


def _ntbs(buf, pos):
    end = buf.index(b"\0", pos)
    return buf[pos:end].decode("ascii", "replace"), end + 1


def elf_target(path):
    """({tag: value} from .ARM.attributes' aeabi File scope, EI_DATA).

    Read out of the ELF directly rather than from `readelf -A` text: a gate
    that parses a tool's prose is a gate that depends on that tool's vocabulary
    (issues #42/#66), and this format is small and specified (ARM IHI 0045).
    """
    raw = open(path, "rb").read()
    if raw[:4] != b"\x7fELF" or raw[4] != 1:
        raise ValueError("not a 32-bit ELF")
    ei_data = raw[5]
    e = "<" if ei_data == 1 else ">"
    shoff, = struct.unpack_from(e + "I", raw, 0x20)
    shentsize, shnum, shstrndx = struct.unpack_from(e + "HHH", raw, 0x2E)
    shdrs = [struct.unpack_from(e + "IIIIIIIIII", raw, shoff + i * shentsize)
             for i in range(shnum)]
    stro = shdrs[shstrndx][4]
    attrs = None
    for sh in shdrs:
        name, _ = _ntbs(raw, stro + sh[0])
        if name == ".ARM.attributes":
            attrs = raw[sh[4]:sh[4] + sh[5]]
    if attrs is None:
        raise ValueError("no .ARM.attributes section")
    if attrs[:1] != b"A":
        raise ValueError(".ARM.attributes is not format version 'A'")

    tags = {}
    pos = 1
    while pos < len(attrs):
        sublen, = struct.unpack_from(e + "I", attrs, pos)
        vendor, vpos = _ntbs(attrs, pos + 4)
        end = pos + sublen
        if vendor == "aeabi":
            q = vpos
            while q < end:
                scope, q2 = _uleb(attrs, q)
                sslen, = struct.unpack_from(e + "I", attrs, q2)
                ssend = q + sslen
                if scope != 1:
                    # Section/symbol scoped attributes would make the image's
                    # answer depend on WHERE -- not something to summarise.
                    raise ValueError("section- or symbol-scoped attributes")
                r = q2 + 4
                while r < ssend:
                    tag, r = _uleb(attrs, r)
                    if tag in (4, 5, 67) or (tag > 32 and tag % 2 == 1):
                        val, r = _ntbs(attrs, r)
                    elif tag == 32:                 # Tag_compatibility
                        _, r = _uleb(attrs, r)
                        val, r = _ntbs(attrs, r)
                    else:
                        val, r = _uleb(attrs, r)
                    tags[tag] = val
                q = ssend
        pos = end
    return tags, ei_data


def abi_marks(path):
    """[(value, address)] of every pl_abi_mark in the image's symbol table.

    Read out of the ELF itself, like elf_target(): the symbol table for where
    each TU's record is, and the loadable section bytes for what it says."""
    raw = open(path, "rb").read()
    if raw[:4] != b"\x7fELF" or raw[4] != 1 or raw[5] != 1:
        raise ValueError("not a 32-bit little-endian ELF")
    shoff, = struct.unpack_from("<I", raw, 0x20)
    shentsize, shnum, _ = struct.unpack_from("<HHH", raw, 0x2E)
    shdrs = [struct.unpack_from("<10I", raw, shoff + i * shentsize)
             for i in range(shnum)]
    out = []
    for sh in shdrs:
        if sh[1] != 2:                              # SHT_SYMTAB
            continue
        strtab = shdrs[sh[6]]
        for off in range(sh[4], sh[4] + sh[5], 16):
            name_off, value, size, _info, _other, shndx = struct.unpack_from(
                "<IIIBBH", raw, off)
            name, _ = _ntbs(raw, strtab[4] + name_off)
            if name != "pl_abi_mark":
                continue
            if size != 4 or shndx == 0 or shndx >= len(shdrs):
                raise ValueError(f"pl_abi_mark at 0x{value:08x} is not a "
                                 "defined 4-byte object")
            s = shdrs[shndx]
            if s[1] == 8 or not s[3] <= value <= s[3] + s[5] - 4:  # NOBITS
                raise ValueError(f"pl_abi_mark at 0x{value:08x} has no bytes")
            v, = struct.unpack_from("<I", raw, s[4] + value - s[3])
            out.append((v, value))
    return out


def check_target(elf, word, errors):
    """Compare the board's word with what the image says; return a summary."""
    a = ABI
    if word & a["PLUGIN_TARGET_RESERVED_MASK"]:
        errors.append(f"target: 0x{word:08x} sets reserved bits")
        return None
    try:
        tags, ei_data = elf_target(elf)
    except (ValueError, IndexError, struct.error) as exc:
        errors.append(f"target: cannot read the image's attributes ({exc}); "
                      "the word cannot be checked, so it is not accepted")
        return None

    arch = tags.get(6)
    fp_arch = tags.get(10, 0)
    # Tag_ABI_HardFP_use: 1 = single precision only; 0/3 = as Tag_FP_arch.
    sp_only = tags.get(27, 0) == 1
    cpu_name = tags.get(5)
    hit = TARGET_BY_ATTR.get((arch, fp_arch, sp_only))
    if hit is None:
        errors.append(f"target: Tag_CPU_arch {arch} with Tag_FP_arch {fp_arch}"
                      f"{' (SP only)' if sp_only else ''} is not a core and FPU "
                      "any board here builds plugins for -- the word cannot be "
                      "derived from it")
        return None
    cpu, fpu, must_name = hit
    if must_name is not None and cpu_name != must_name:
        errors.append(f"target: {ARCH_NAME.get(arch, arch)} image names its "
                      f"core {cpu_name!r}, not {must_name!r} -- on this "
                      "architecture the name is what tells the cores apart")
        return None
    vfp_args = tags.get(28, 0)
    if vfp_args not in (0, 1):
        errors.append(f"target: Tag_ABI_VFP_args {vfp_args} is neither the "
                      "base nor the VFP calling convention")
        return None
    fabi = "PLUGIN_FLOAT_ABI_HARD" if vfp_args == 1 else "PLUGIN_FLOAT_ABI_SOFT"
    if ei_data not in (1, 2):
        errors.append(f"target: EI_DATA {ei_data} is neither endianness")
        return None
    big = ei_data == 2

    derived = (((a[cpu] << a["PLUGIN_TARGET_CPU_SHIFT"])
                & a["PLUGIN_TARGET_CPU_MASK"])
               | ((a[fpu] << a["PLUGIN_TARGET_FPU_SHIFT"])
                  & a["PLUGIN_TARGET_FPU_MASK"])
               | ((a[fabi] << a["PLUGIN_TARGET_FLOAT_SHIFT"])
                  & a["PLUGIN_TARGET_FLOAT_MASK"])
               | (a["PLUGIN_TARGET_BIG_ENDIAN"] if big else 0))
    # [!] THE CMSE BIT IS MASKED OUT, AND SAYING SO IS PART OF THE CHECK.  It
    # describes the environment the plugin is loaded into, not the plugin, and
    # .ARM.attributes carries no trace of it.  Claiming to derive it would be
    # the same false comment #108 removed from plugin_abi.h.
    given = word & ~a["PLUGIN_TARGET_CMSE"]
    what = f"{cpu[len('PLUGIN_CPU_'):].lower()} / " \
           f"{fpu[len('PLUGIN_FPU_'):].lower()} / " \
           f"{'hard' if vfp_args == 1 else 'soft'} / " \
           f"{'big' if big else 'little'}"
    if given != derived:
        errors.append(f"target: the board's word 0x{word:04x} does not "
                      f"describe this image, which is {what} = "
                      f"0x{derived:04x} (CMSE bit excluded: no image records "
                      "it)")
        return None
    return (f"target 0x{word:04x} = {what}; CMSE bit "
            f"{'set' if word & a['PLUGIN_TARGET_CMSE'] else 'clear'} and NOT "
            "checked here (no image records it -- the firmware asserts it)")

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

# The plugin's own printer sink (asset/common/plugin_text.c).  A plugin that
# formats into its own buffer hands pl_print_write a printer whose write is THIS,
# so the far side of the printer veneer is sometimes the plugin itself.  Its
# bound is emitted as S, and the loader charges max(c, S) at a crossing.
SINK = "pl_sbuf_write"

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


def bound_stack(entry, funcs, frames, errors):
    """The plugin's own stack below `entry`, as (A0, A1), fail-closed.

    [!] NO VENEER COST GOES IN HERE (issue #111).  Until then this took the
    board's cost c and added it at every indirect call, returning one number --
    and that number went into the manifest, where the firmware could not tell
    which c it had been made with.  The walk stops at an indirect call (only a
    veneer has one, which check 7 enforces), so a path crosses into the base at
    most once and the old number decomposes exactly:

        old(entry) = max(A0, A1 + c)            (the second term only if A1)

      A0  the deepest the plugin's OWN frames go, over every path -- a path
          that reaches a crossing counts up to and including the veneer's frame;
      A1  the deepest its own frames go on a path that REACHES a crossing, or
          None when no path does.  A1 <= A0 by construction.

    The firmware adds its own c at load time (svc/plugin_load.c), so neither
    number goes stale when c moves.  Proof that this is the old walk and not a
    new one: old(f) = frame + max(c, old(g)...) = max(frame + A0(g),
    frame + c, frame + A1(g) + c) = max(A0(f), A1(f) + c), with A0(f) =
    frame + max(0, A0(g)...) and A1(f) = frame + max(0 if f crosses, A1(g)...).
    cmake/fixtures/run_plugin_gate_tests.py checks it against a transcription of
    the old walk at three values of c.
    """
    def walk(fn, path):
        if fn in path:
            errors.append(f"stack: recursion through {fn} ({' -> '.join(path)})")
            return 0, None
        su = frame_of(fn, frames)
        if su is None:
            errors.append(f"stack: no frame recorded for {fn} -- the .su input "
                          "does not match the linked image")
            return 0, None
        frame, qual = su
        if qual != "static":
            errors.append(f"stack: {fn} has a {qual} frame (alloca/VLA); a "
                          "bound cannot be stated")
            return 0, None
        own, cross = 0, None
        for line in funcs.get(fn, []):
            if INDIRECT_RE.search(line):
                # Only ever legal inside a veneer, which check 7 enforces.  The
                # far side is charged by whoever knows it: the firmware adds c.
                cross = 0 if cross is None else max(cross, 0)
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
                    o, x = walk(callee, path + [fn])
                    own = max(own, o)
                    if x is not None:
                        cross = x if cross is None else max(cross, x)
        return frame + own, (None if cross is None else frame + cross)

    return walk(entry, [])


def bound_at(own, cross, cost):
    """What a slot needs once `cost` is charged at its crossing."""
    return own if cross is None else max(own, cross + cost)


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
    ap.add_argument("--target-id", required=True, type=lambda v: int(v, 0),
                    help="the board's plugin target word (svc/plugin_abi.h)")
    args = ap.parse_args()
    if args.end <= args.base:
        print("check_plugin_image: FAIL\n  - --end 0x%08x is not above --base "
              "0x%08x" % (args.end, args.base), file=sys.stderr)
        return 1
    forbidden = set(args.forbid)

    errors = []
    secs = sections(args.objdump, args.elf)
    funcs = disassemble(args.objdump, args.elf)

    # 0. the target word
    target_note = check_target(args.elf, args.target_id, errors)

    # 0b. the ABI every TU was compiled against (issue #111)
    try:
        marks = abi_marks(args.elf)
    except (ValueError, IndexError, struct.error) as exc:
        errors.append(f"abi: cannot read the image's ABI records ({exc})")
        marks = None
    if marks is not None:
        want = ABI["PLUGIN_ABI_VERSION"]
        if not marks:
            errors.append("abi: no pl_abi_mark in the image -- it was not "
                          "compiled by add_plugin() (PLUGIN_IMAGE_BUILD), so "
                          "which ABI its code expects is unknown")
        for v, at in marks:
            if v != want:
                errors.append(f"abi: a TU of this image was compiled against "
                              f"ABI {v} (pl_abi_mark at 0x{at:08x}), the "
                              f"header is ABI {want} -- a stale object; the "
                              "packer would stamp the header's ABI over code "
                              "that expects another")

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
    parts = {}
    if args.entry:
        if not frames:
            errors.append("stack: --entry given without --su; a bound cannot be "
                          "derived from the ELF alone")
        for spec in args.entry:
            name, _, limit = spec.partition("=")
            own, cross = bound_stack(name, funcs, frames, errors)
            parts[name] = (own, cross)
            # The limit is still checked at THIS board's cost, as it always
            # was: the build refuses what this firmware would refuse.
            got = bound_at(own, cross, args.veneer_base_cost)
            bounds[name] = got
            if limit and got > int(limit):
                errors.append(f"stack: {name} needs {got} B, limit {limit} B")

    # [!] S IS A NUMBER THE LOADER ADDS, SO IT MUST NOT DEPEND ON c.  The sink is
    # reached through the printer veneer; if it crossed a veneer itself, its
    # bound would be one more c-dependent sum, and the manifest would be back to
    # carrying the firmware's cost.  Refused rather than folded in.
    sink = None
    if args.emit_stacks:
        if SINK not in parts:
            errors.append(f"stack: --emit-stacks needs --entry {SINK}=<limit>: "
                          "the sink bound S is part of the declaration, and an "
                          "unmeasured one cannot be declared")
        elif parts[SINK][1] is not None:
            errors.append(f"stack: {SINK} reaches a veneer itself; its bound "
                          "would contain the board's cost, which a manifest "
                          "no longer carries")
        else:
            sink = parts[SINK][0]

    if errors:
        print("check_plugin_image: FAIL", file=sys.stderr)
        for e in errors:
            print("  - " + e, file=sys.stderr)
        return 1

    # [!] THE MANIFEST'S STACK NUMBERS ARE THIS ANALYSIS, not a second opinion.
    # Handing the packer the parts derived here is what makes "the manifest
    # agrees with the gate" true by construction; the check that still has teeth
    # is the DEVICE adding its own c and comparing against what its threads can
    # spare, which no host knows.  `bound` is informational -- it is at THIS
    # build's cost, and nothing downstream may pack it.
    if args.emit_stacks:
        import json
        out = {
            "accounting": ABI["PLUGIN_STACK_ACCOUNTING"],
            "sink": sink,
            "veneer_base_cost": args.veneer_base_cost,
            "entries": {
                k: {"own": o, "cross": 0 if x is None else x,
                    "crossing": x is not None, "bound": bounds[k]}
                for k, (o, x) in parts.items()},
        }
        with open(args.emit_stacks, "w") as fh:
            json.dump(out, fh, indent=2, sort_keys=True)

    t = secs.get(".text", (0, 0, set()))[1]
    d = secs.get(".data", (0, 0, set()))[1]
    b = secs.get(".bss", (0, 0, set()))[1]
    summary = ", ".join(
        f"{k} {v} B" + (f" (A0 {parts[k][0]} / A1 {parts[k][1]})"
                        if parts[k][1] is not None else "")
        for k, v in sorted(bounds.items()))
    print(f"check_plugin_image: OK (text {t} B, data {d} B, bss {b} B"
          + (f"; stack {summary}" if summary else "") + ")")
    print(f"check_plugin_image: {target_note}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
