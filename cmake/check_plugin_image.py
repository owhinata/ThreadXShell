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

Everything else -- the section whitelist, the veneer set and the stack
analysis (thumb_stack_analysis.py since issue #124, shared with the
firmware-side check) -- is the ABI's and is the same for every board.

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
  7. indirect branches only inside the named veneers -- decoded from the
     instruction bytes, image-wide (issue #124)
  8. a transitive stack bound per entry point, fail-closed -- emitted in
     parts that do not contain the board's veneer cost (issue #111), from
     frames counted in the bytes with the compiler's record as a witness
     (issue #124; see stack_analysis)
  9. S, the bound of every plugin function the base may be handed, found in
     the relocations of the link's objects (issue #124; see Handed)
"""
import argparse
import os
import re
import struct
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
# The decoder, the analysis of one body and the walk (issue #124).
import thumb_stack_analysis as tsa  # noqa: E402

# ---- the target word (issue #108) ------------------------------------------
#
# The board declares one word (svc/plugin/plugin_abi.h, plugin_target_id()) and hands
# it to the packer, the host container verifier and its firmware policy.  Until
# #108 nothing checked the word itself: the three agreed because they read the
# same CMake variable.  This half derives what the PLUGIN IMAGE says about
# itself and compares; the firmware's half (svc/plugin/plugin_target.h) derives the
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
    # in svc/plugin/plugin_abi.h -- and here, where the fixture will insist on it -- when
    # the walk changes what it charges or where it stops.
    "PLUGIN_STACK_ACCOUNTING": 2,
    # The ABI each plugin TU records (svc/plugin/plugin_abi.h, pl_abi_mark): the
    # image is refused unless every record is this (issue #111).
    "PLUGIN_ABI_VERSION": 2,
    # The slot indices, for cmake/check_policy_probe.py to read a policy's
    # stack_limit[] by slot NAME against the board's table (issue #126).  Not
    # used by this gate, which names a slot by its symbol.
    "PLUGIN_SLOT_ENTRY": 0,
    "PLUGIN_SLOT_SHAPES_OK": 1,
    "PLUGIN_SLOT_DECODE": 2,
    "PLUGIN_SLOT_DRAW": 3,
    "PLUGIN_SLOT_REPORT": 4,
    "PLUGIN_SLOT_PARAM_SET": 5,
    "PLUGIN_SLOT_PARAM_GET": 6,
    "PLUGIN_SLOT_COUNT": 7,
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
# plugin_base.c.  [!] THIS IS THE SET'S ONLY DECLARATION: the firmware-side
# check imports it, and the shared analysis takes it as an argument.
VENEERS = {
    "pl_base_log",
    "pl_base_to_frame",
    "pl_paint_rect",
    "pl_paint_fill_rect",
    "pl_paint_blit",
    "pl_print_write",
}

# [!] NO SINK IS NAMED HERE (issue #124).  The far side of the printer veneer
# is sometimes the plugin itself -- its own string sink, pl_sbuf_write, put in a
# printer by asset/common/plugin_text.c -- and until #124 S was that one
# function's bound, found by this name.  S is now derived from the relocations
# over every function the base may be handed (Handed, below).


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


# ---- the stack bound (issue #124: the shared analysis) ----------------------
#
# [!] THE FIRMWARE-SIDE CHECK'S ANALYSIS, NOT A SECOND ONE.  Until #124 this
# gate had its own: frames were the compiler's -fstack-usage numbers taken as
# the answer, edges were regular expressions over objdump's text, and functions
# were known by NAME.  Each of those was measured to miss something: two
# `static` functions of one name in two files merged into one (a 920 B path
# answered as 16 B), a conditional branch used as a tail call was not an edge,
# `mov lr, rX; bx lr`, `ldr pc, [...]` and a `pop {pc}` with lr never saved
# were not indirect transfers, and a record that under-reports (a variadic save
# area, a naked function) was believed.  The decoder, the analysis of one body
# and the walk are now thumb_stack_analysis.py's, the same code that bounds the
# firmware below each veneer (check_veneer_base_cost.py), and the two differ in
# one thing only: WHERE THE WALK STOPS.  Here it stops at the register branch of
# a named veneer, and returns the two halves the manifest declares.
#
#   functions  ELF FUNC symbols, by ADDRESS.  A name is used only to find an
#              --entry (exactly one body, or refused) and a veneer (at most
#              one body per name, or refused).
#   frame      the sum of every stack decrement in the body's BYTES.  The
#              compiler's record is a WITNESS: a scan below the record of the
#              body's own object file is refused (the decoder missed
#              something), and so is a body with no record, an ambiguous one,
#              or a dynamic one.  There is NO prebuilt exemption here: every
#              body of a plugin is compiled by add_plugin(), so every one has a
#              record or the image is refused.
#   refused    anywhere in the image, not only below an entry: an indirect
#              transfer outside a veneer (a register branch, a load or move
#              into pc, tbb/tbh), a return or tail call not proven to go
#              back to the caller with sp where it was (the saved lr's word,
#              sp at the entry -- issue #124, D4), a branch into the middle of
#              another function, an SP change that is not a constant or sits
#              on a loop, a security state transition (sg / bxns / blxns,
#              inside a veneer too), an encoding the decoder does not know.
#              Below an entry also recursion, found by address.  Inside a
#              veneer exactly one thing is allowed that is refused elsewhere:
#              `bx rN` / `blx rN` (r0-r12), the crossing into the base -- and
#              `bx rN` is a tail call, held to the same sp and lr rule.
#
# [!] WHAT IT DOES NOT PROVE is what the shared analysis does not prove for the
# firmware either (check_veneer_base_cost.py, "AND WHAT IS NOT COMPARED WITH
# THE SAVED LR"): that a store whose position is not sp plus a known constant
# misses the saved lr.  That is memory safety, which the gate does not claim.
#
# [!] switch JUMP TABLES (tbb/tbh) ARE REFUSED BY THE DECODER, UNCONDITIONALLY.
# add_plugin() compiles with -fno-jump-tables so that a plugin's switch does not
# produce one; that flag is a way to pass, not a replacement for the refusal --
# a plugin that forced a table back (a later flag, hand-written asm) is still
# refused here.  Nothing follows a table's entries.


def resolve(image, name):
    """[addr] of every body that carries `name`."""
    return sorted(a for a, n in image.nodes.items() if name in n.names)


# ---- the functions the base may be handed (issue #124, D5) -----------------
#
# [!] S IS THE BOUND OF EVERY PLUGIN FUNCTION THE BASE CAN BE GIVEN, NOT OF ONE
# FUNCTION NAMED HERE.  The printer veneer (pl_print_write) calls whatever
# function pointer the printer it is handed carries; the plugin's own string
# sink (pl_sbuf_write) is one such function, and until #124 S was its bound,
# found by name.  Any other plugin function whose address the plugin takes
# could be put in a printer just the same, and nothing measured it.  So the
# list is derived: every plugin function whose address is taken for any reason
# other than a branch, read out of the RELOCATIONS of the objects the link
# consumed, and S is the deepest of them (a function that reaches a veneer
# itself is refused -- its bound would contain the board's cost).
#
#   read      every relocation of every allocated input section the map
#             places, in every object the link LOADed.  A data word or a
#             literal-pool word (R_ARM_ABS32), either half of a MOVW/MOVT pair
#             (each half names its own symbol + addend), a section symbol plus
#             an addend, a symbol another object or the link script defines
#             (through the image's own global of that name), an alias: each is
#             resolved to the LINKED address of what it names, with the Thumb
#             bit dropped, and a function start there puts that function on
#             the list.
#   skipped   branch relocations (a call or tail call is an edge, walked as
#             such); the relocations INSIDE the slot table (the loader reaches
#             a slot through the manifest, never through a pointer the base is
#             handed); sections --gc-sections discarded (not in the image); and
#             unallocated sections (debug information is not loaded).
#   refused   a relocation type not listed below; a reference to code that is
#             not a function's first byte; ANY reference to the slot table from
#             outside it -- code that reads a slot out of the table gets a
#             function pointer no relocation shows, so the table is not
#             readable at all; an object that cannot be read as an ARM
#             relocatable; an object the link loaded that the gate was not
#             given, or one it was given that the link did not load; an input
#             section the map places from a file that is not one of them; an
#             allocated section the map neither places nor discards.
#
# [!] WHAT IT CANNOT SEE: A FUNCTION ADDRESS MADE FROM A NUMBER.  Code that
# writes the address as an integer constant (or computes it from one) carries
# no relocation, so nothing here can find it.  That is on the same side of the
# line as memory safety: a plugin is reviewed, trusted native code, and this
# gate proves a stack bound for the code as the compiler and linker describe
# it, not against a plugin that hides a pointer from them.  cmake/README.md
# states the same line.
R_ARM_NONE = 0
R_ARM_ABS32 = 2
R_ARM_THM_CALL = 10
R_ARM_THM_JUMP24 = 30
R_ARM_THM_MOVW_ABS_NC = 47
R_ARM_THM_MOVT_ABS = 48
R_ARM_THM_JUMP19 = 51
R_ARM_THM_JUMP11 = 102
R_ARM_THM_JUMP8 = 103
BRANCH_RELOCS = {R_ARM_THM_CALL: "R_ARM_THM_CALL",
                 R_ARM_THM_JUMP24: "R_ARM_THM_JUMP24",
                 R_ARM_THM_JUMP19: "R_ARM_THM_JUMP19",
                 R_ARM_THM_JUMP11: "R_ARM_THM_JUMP11",
                 R_ARM_THM_JUMP8: "R_ARM_THM_JUMP8"}
ADDRESS_RELOCS = {R_ARM_ABS32: "R_ARM_ABS32",
                  R_ARM_THM_MOVW_ABS_NC: "R_ARM_THM_MOVW_ABS_NC",
                  R_ARM_THM_MOVT_ABS: "R_ARM_THM_MOVT_ABS"}
SHT_RELA, SHT_REL, SHT_NOBITS = 4, 9, 8
SHF_MERGE = 0x10
ET_REL = 1
SHN_UNDEF, SHN_LORESERVE, SHN_ABS, SHN_COMMON = 0, 0xFF00, 0xFFF1, 0xFFF2
STB_GLOBAL, STB_WEAK = 1, 2
# The slot table: the input section that holds it, and the names that point at
# it.  [!] THE TABLE IS NOT READABLE: see "refused" above.
SLOT_SECTION = ".plugin_slots"
SLOT_SYMBOLS = {"plugin_slot_table", "__plugin_slots_start",
                "__plugin_slots_end"}
# The linker's own input, which has no object to read and no relocations.
LINKER_STUBS = "linker stubs"


def _thumb_imm16(hw1, hw2):
    v = ((hw1 & 0xF) << 12) | (((hw1 >> 10) & 1) << 11) | \
        (((hw2 >> 12) & 7) << 8) | (hw2 & 0xFF)
    return v - 0x10000 if v & 0x8000 else v


class Handed:
    """The plugin functions whose address the plugin takes (issue #124 D5).

    `addrs` is {function addr: [where each reference is]}; `errors` is every
    refusal.  Built from the linked image (functions, by address), its map
    (where each input section landed, what was discarded, what was loaded)
    and the objects the link consumed (the relocations)."""

    def __init__(self, image, map_path, objects, out_names):
        self.image = image
        self.addrs = {}
        self.errors = []
        link_dir = os.path.dirname(os.path.abspath(map_path))

        def norm(p):
            return os.path.realpath(os.path.join(link_dir, p))

        try:
            placed, discarded, loads = tsa.map_sections(map_path, out_names)
        except tsa.InputError as exc:
            self.errors.append(f"[input] {exc}")
            return
        loads = {norm(p) for p in loads if p != LINKER_STUBS}
        given = {}
        for p in objects:               # as the command line names them
            given.setdefault(os.path.realpath(p), p)

        # [!] BOTH DIRECTIONS.  What the link LOADed and what the gate was
        # given must be the same set: an object linked and not read would
        # contribute code whose relocations nobody looked at.
        for p in sorted(loads - set(given)):
            self.errors.append(f"{p}: the link loaded it and the gate was not "
                               "given it -- its relocations were not read")
        for p in sorted(set(given) - loads):
            self.errors.append(f"{given[p]}: given to the gate, but the link "
                               "did not load it")
        self.place = {}             # (file, section name) -> [(addr, size)]
        self.slot_ranges = []
        self.code_inputs = []       # placed executable input ranges
        for name, addr, size, what, _out in placed:
            if what == LINKER_STUBS:
                continue
            f = norm(what)
            if f not in loads:
                self.errors.append(f"{what}: the map places {name} from it, "
                                   "and it is not an object the link loaded "
                                   "-- its relocations were not read")
                continue
            self.place.setdefault((f, name), []).append((addr, size))
            if name == SLOT_SECTION:
                self.slot_ranges.append((addr, addr + size))
        self.discarded = {(norm(w), n) for n, w in discarded
                          if w != LINKER_STUBS}

        self.reported = set()
        self.objs = {}
        for f in sorted(loads & set(given)):
            try:
                o = tsa.Elf(f)
            except tsa.InputError as exc:
                self.errors.append(f"{given[f]}: cannot read its relocations "
                                   f"({exc})")
                continue
            if o.etype != ET_REL:
                self.errors.append(f"{given[f]}: not a relocatable object "
                                   f"(e_type {o.etype}) -- its relocations "
                                   "cannot be read")
                continue
            self.objs[f] = o
        if len(self.objs) != len(loads & set(given)):
            return
        for f, o in sorted(self.objs.items()):
            for s in o.sections:
                if s.flags & tsa.SHF_ALLOC and s.size:
                    a = self._placed(f, o, s)
                    if a is not None and s.flags & tsa.SHF_EXECINSTR:
                        self.code_inputs.append((a, a + s.size))
        self.code_inputs.sort()
        for f in sorted(self.objs):
            self._object(f, self.objs[f])

    # --- where an input section is ------------------------------------------
    def _placed(self, f, o, s):
        """The linked address of object section `s`, or None if it is not in
        the image.  [!] AN ALLOCATED SECTION THE MAP NEITHER PLACES NOR
        DISCARDS IS REFUSED: its relocations would otherwise be skipped as if
        it had been garbage-collected."""
        at = self.place.get((f, s.name))
        if at is None:
            if (f, s.name) not in self.discarded and (f, s.name) not in \
                    self.reported:
                self.reported.add((f, s.name))
                self.errors.append(f"{os.path.basename(f)}: section {s.name} "
                                   "is neither placed nor discarded by the "
                                   "map")
            return None
        if len(at) != 1 or sum(1 for x in o.sections
                               if x.name == s.name) != 1:
            if (f, s.name) not in self.reported:
                self.reported.add((f, s.name))
                self.errors.append(f"{os.path.basename(f)}: section {s.name} "
                                   "cannot be matched to one place in the map "
                                   "by its name")
            return None
        addr, size = at[0]
        if size != s.size and not s.flags & SHF_MERGE:
            if (f, s.name) not in self.reported:
                self.reported.add((f, s.name))
                self.errors.append(f"{os.path.basename(f)}: section {s.name} "
                                   f"is {s.size} B and the map places "
                                   f"{size} B")
            return None
        return addr

    # --- one object ---------------------------------------------------------
    def _object(self, f, o):
        base = os.path.basename(f)
        for rs in o.sections:
            if rs.type == SHT_RELA:
                self.errors.append(f"{base}: {rs.name} is SHT_RELA -- an ARM "
                                   "object's relocations are SHT_REL, and "
                                   "these are not read")
                continue
            if rs.type != SHT_REL:
                continue
            if rs.info >= len(o.sections) or rs.link != o.symtab_index or \
                    rs.entsize != 8 or rs.size % 8:
                self.errors.append(f"{base}: {rs.name} cannot be read as "
                                   "relocations of one section against the "
                                   "symbol table")
                continue
            tgt = o.sections[rs.info]
            if not tgt.flags & tsa.SHF_ALLOC:
                continue                    # debug information: not loaded
            if self._placed(f, o, tgt) is None:
                continue                    # garbage-collected (or refused)
            in_slots = tgt.name == SLOT_SECTION
            code = bool(tgt.flags & tsa.SHF_EXECINSTR)
            for off in range(rs.offset, rs.offset + rs.size, 8):
                r_off, r_info = struct.unpack_from("<II", o.raw, off)
                self._reloc(f, o, tgt, in_slots, code, r_off, r_info >> 8,
                            r_info & 0xFF)

    def _reloc(self, f, o, tgt, in_slots, code, r_off, sym, typ):
        base = os.path.basename(f)
        where = f"{base}: {tgt.name}+0x{r_off:x}"
        if typ in BRANCH_RELOCS:
            if not code:
                self.errors.append(f"{where}: {BRANCH_RELOCS[typ]} outside "
                                   "code")
            return                          # an edge, walked as one
        if typ not in ADDRESS_RELOCS:
            self.errors.append(f"{where}: relocation type {typ} is not one "
                               "this gate reads -- what it makes of a "
                               "function's address is unknown")
            return
        if sym >= len(o.symbols):
            self.errors.append(f"{where}: symbol index {sym} out of range")
            return
        if tgt.type == SHT_NOBITS or r_off + 4 > tgt.size:
            self.errors.append(f"{where}: outside the bytes of its section")
            return
        raw_at = tgt.offset + r_off
        if typ == R_ARM_ABS32:
            addend, = struct.unpack_from("<I", o.raw, raw_at)
        else:
            hw1, hw2 = struct.unpack_from("<HH", o.raw, raw_at)
            want = 0xF240 if typ == R_ARM_THM_MOVW_ABS_NC else 0xF2C0
            if not code or (hw1 & 0xFBF0) != want or hw2 & 0x8000:
                self.errors.append(f"{where}: {ADDRESS_RELOCS[typ]} on "
                                   "something that is not that instruction")
                return
            addend = _thumb_imm16(hw1, hw2)
        name, value, _sz, styp, shndx = o.symbols[sym]
        label = name or (o.sections[shndx].name
                         if 0 < shndx < len(o.sections) else "?")
        why = f"{where} ({ADDRESS_RELOCS[typ]} {label}" + \
            (f"{addend:+#x}" if addend else "") + ")"
        # The table's own names.  [!] __plugin_slots_end IS ONE PAST THE
        # TABLE, so where it points cannot say what it is: only its name can.
        if name in SLOT_SYMBOLS:
            if not in_slots:
                self._slot_ref(why)
            return
        if shndx == SHN_UNDEF:
            # Defined in another object, or by the link script: what the link
            # bound it to is the image's own global of that name.  An alias
            # is just another name for an address, and is classified by it.
            elf = self.image.elf
            vals = {(v & ~1) if t == tsa.STT_FUNC else v
                    for k, (n, v, _s, t, sh) in enumerate(elf.symbols)
                    if n == name and sh != SHN_UNDEF
                    and elf.binds[k] in (STB_GLOBAL, STB_WEAK)}
            if len(vals) != 1:
                self.errors.append(f"{why}: {name} resolves to "
                                   f"{len(vals)} addresses in the image")
                return
            self._addr((vals.pop() + addend) & 0xFFFFFFFF, in_slots, why)
            return
        self._target(f, o, value, shndx, addend, in_slots, why, styp)

    def _target(self, f, o, value, shndx, addend, in_slots, why, styp=None):
        # S is the symbol's ADDRESS: a Thumb function's value carries the
        # Thumb bit, which the relocation ORs back in (AAELF32 (S + A) | T).
        if styp == tsa.STT_FUNC:
            value &= ~1
        if shndx == SHN_ABS:
            self._addr((value + addend) & 0xFFFFFFFF, in_slots, why)
            return
        if shndx == SHN_COMMON:
            return                          # data (and refused as COMMON)
        if shndx >= SHN_LORESERVE or shndx >= len(o.sections):
            self.errors.append(f"{why}: symbol in section index {shndx}")
            return
        s = o.sections[shndx]
        code = bool(s.flags & tsa.SHF_EXECINSTR)
        # [!] DATA IS CLASSIFIED BY ADDRESS TOO, not dismissed by its section:
        # a data symbol plus an addend that lands in code or in the slot table
        # is the same reference as one that names it.  Only a merged string
        # section has no byte-for-byte address to give (and holds no code).
        if not code and (s.flags & SHF_MERGE or s.size == 0):
            return
        at = self._placed(f, o, s)
        if at is None:
            if code:
                self.errors.append(f"{why}: names code that is not in the "
                                   "image")
            return
        # The slot table is found by the address this makes (an alias, or a
        # section symbol of .plugin_slots, is an address in it).
        self._addr((at + value + addend) & 0xFFFFFFFF, in_slots, why)

    def _slot_ref(self, why):
        self.errors.append(f"{why}: refers to the slot table -- a slot read "
                           "out of the table is a function pointer no "
                           "relocation shows, so nothing outside the table "
                           "may refer to it")

    def _addr(self, addr, in_slots, why):
        """Classify one resolved, linked address."""
        if any(lo <= addr < hi for lo, hi in self.slot_ranges):
            if not in_slots:
                self._slot_ref(why)
            return
        if in_slots:
            return                  # what the loader reaches by the manifest
        a = addr & ~1
        if a in self.image.nodes:
            self.addrs.setdefault(a, []).append(why)
            return
        n = self.image.node_containing(a)
        if n is not None:
            self.errors.append(f"{why}: points into {n.name}+0x{a - n.addr:x}"
                               ", not at a function's first instruction")
            return
        if any(lo <= a < hi for lo, hi in self.code_inputs):
            self.errors.append(f"{why}: points at code 0x{a:08x} that is "
                               "not a function's first instruction")


def stack_analysis(elf_path, map_path, entries, objects, errors):
    """({entry: (A0, A1 or None)} for each name in `entries`,
    {addr: (name, A0, [where its address is taken])} for each function the
    base may be handed (Handed, issue #124 D5), {entry: addr}), fail-closed.

    [!] NO VENEER COST GOES IN HERE (issue #111).  The walk stops at a
    crossing (only a veneer has one), so a path crosses into the base at most
    once and what the firmware charges decomposes as max(A0, A1 + c) -- the
    loader adds its own c (svc/plugin/plugin_load.c), so neither part goes
    stale when c moves.  cmake/fixtures/run_plugin_gate_tests.py checks the
    parts against a transcription of the pre-#124 walk at three values of c."""
    try:
        elf = tsa.Elf(elf_path)
        image = tsa.Image(elf, *tsa.parse_map(
            map_path, {s.name for s in elf.code}))
    except tsa.InputError as exc:
        errors.append(f"stack: [input] {exc}")
        return {}, {}, {}

    veneers = set()
    for v in sorted(VENEERS):
        at = resolve(image, v)
        if len(at) > 1:
            errors.append(f"stack: veneer {v} names {len(at)} bodies ("
                          + ", ".join(f"0x{a:08x}" for a in at)
                          + ") -- a veneer must be exactly one")
        veneers.update(at)
    walk = tsa.Walk(image, crossings=veneers)

    # Every body of the image, reachable or not: a plugin is one program, all
    # of it compiled here, and what no entry reaches today a later edge does.
    for a in sorted(image.nodes):
        node = image.nodes[a]
        info = image.analyse(node)
        errs, _ = walk.body_errors(a)
        indirect = {id(err) for _at, _why, err in info.indirect}
        for err in errs:
            hint = (" -- only the named veneers may, or the stack bound is "
                    "not a bound") if id(err) in indirect else ""
            errors.append(f"stack: {node.name}: {err[1]}{hint}")

    # The witnesses.  [!] NO --prebuilt-root: nothing in a plugin is exempt.
    wit = tsa.Witnesses(image, os.path.dirname(os.path.abspath(map_path)),
                        [], [])
    errors.extend(f"stack: [witness] {e}" for e in wit.errors)
    for a in sorted(image.nodes):
        node = image.nodes[a]
        info = image.analyse(node)
        state, detail = wit.records(node)
        if state == "none":
            # 'stub': the linker's own code, which has no record by nature.
            if info.veneer is None:
                errors.append(f"stack: {node.name}: linker-generated code that "
                              "is not a long-branch veneer")
            continue
        if state == "missing":
            errors.append(f"stack: [witness] {node.name}: {detail} -- every "
                          "body of a plugin is compiled by add_plugin() and "
                          "must have its own object's record")
            continue
        if state != "ok":
            errors.append(f"stack: [witness] {node.name}: {detail}")
            continue
        frame, where = detail
        if not info.frame_errs and info.frame < frame:
            errors.append(f"stack: [witness] {node.name}: the scan found "
                          f"{info.frame} B, the compiler's record ({where}) "
                          f"says {frame} B -- the decoder missed a stack "
                          "adjustment")

    out = {}
    out_at = {}
    for name in entries:
        at = resolve(image, name)
        if not at:
            errors.append(f"stack: no function {name} in the image")
            continue
        if len(at) > 1:
            errors.append(f"stack: {name} names {len(at)} bodies -- an entry "
                          "must be exactly one")
            continue
        own, cross, _complete = walk.parts(at[0])
        out[name] = (own, cross)
        out_at[name] = at[0]

    # The functions the base may be handed, and each one's bound.  [!] A
    # FUNCTION ON THIS LIST THAT REACHES A VENEER IS REFUSED: S is a number
    # the loader adds at a crossing, so it must not contain c (issue #111).
    handed = Handed(image, map_path, objects,
                    {s.name for s in elf.sections if s.flags & tsa.SHF_ALLOC})
    errors.extend(f"handed: {e}" for e in handed.errors)
    listed = {}
    for a in sorted(handed.addrs):
        node = image.nodes[a]
        own, cross, _complete = walk.parts(a)
        if cross is not None:
            errors.append(f"handed: {node.name} can be handed to the base "
                          f"({handed.addrs[a][0]}) and reaches a veneer "
                          "itself; its bound would contain the board's cost, "
                          "which a manifest no longer carries")
            continue
        listed[a] = (node.name, own, handed.addrs[a])
    errors.extend(f"stack: {msg}" for _stage, msg in walk.errors)
    return out, listed, out_at


def bound_at(own, cross, cost):
    """What a slot needs once `cost` is charged at its crossing."""
    return own if cross is None else max(own, cross + cost)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("elf")
    ap.add_argument("--nm", required=True)
    ap.add_argument("--objdump", required=True)
    ap.add_argument("--map", required=True,
                    help="the GNU ld map of this link: which object each body "
                         "came from, and so which -fstack-usage record "
                         "witnesses it (<object>.su beside the object)")
    ap.add_argument("--entry", nargs="*", default=[],
                    help="functions to bound the stack below, name=limit")
    ap.add_argument("--emit-stacks",
                    help="write the derived bounds here, for the packer")
    ap.add_argument("--objects", nargs="+", required=True,
                    help="the objects the link consumed: their relocations "
                         "say which plugin functions have their address "
                         "taken (checked against the map's LOADs, both ways)")
    ap.add_argument("--printer-limit", required=True, type=int,
                    help="the bound on every plugin function the base may be "
                         "handed (the board's SBUF_WRITE_MAX), in bytes")
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
                    help="the board's plugin target word (svc/plugin/plugin_abi.h)")
    args = ap.parse_args()
    if args.end <= args.base:
        print("check_plugin_image: FAIL\n  - --end 0x%08x is not above --base "
              "0x%08x" % (args.end, args.base), file=sys.stderr)
        return 1
    forbidden = set(args.forbid)

    errors = []
    secs = sections(args.objdump, args.elf)

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

    # 7. indirect branches only inside the veneers, and
    # 8. stack bounds -- one analysis since issue #124 (stack_analysis above):
    # an indirect transfer anywhere but a veneer's crossing is one of its
    # refusals, so the two checks cannot disagree about what a branch is.
    if args.printer_limit <= 0:
        errors.append(f"--printer-limit {args.printer_limit}: a bound must be "
                      "a positive byte count")
    bounds = {}
    specs = [spec.partition("=") for spec in args.entry]
    # No entry still analyses the image: check 7 is not optional.
    parts, listed, key_at = stack_analysis(args.elf, args.map,
                                   [name for name, _, _ in specs],
                                   args.objects, errors)
    for name, _, limit in specs:
        if name not in parts:
            continue
        own, cross = parts[name]
        # The limit is still checked at THIS board's cost, as it always
        # was: the build refuses what this firmware would refuse.
        got = bound_at(own, cross, args.veneer_base_cost)
        bounds[name] = got
        if limit and got > int(limit):
            errors.append(f"stack: {name} needs {got} B, limit {limit} B")

    # [!] S IS THE DEEPEST FUNCTION THE BASE MAY BE HANDED (issue #124 D5),
    # and every one of them is held to the printer limit.  It does not depend
    # on c: a listed function that crosses a veneer is refused above.  An
    # image that hands the base nothing declares S = 0, a measurement.  Each
    # listed function is also emitted beside the slots, by name, so the
    # declaration shows what S was taken over (the packer reads the slots
    # only).
    sink = 0
    for a, (name, own, why) in sorted(listed.items()):
        if own > args.printer_limit:
            errors.append(f"handed: {name} can be handed to the base "
                          f"({why[0]}) and needs {own} B, printer limit "
                          f"{args.printer_limit} B")
        sink = max(sink, own)
        key = name
        if key in key_at and key_at[key] != a:
            key = f"{name}@0x{a:08x}"
        key_at[key] = a
        parts[key] = (own, None)
        bounds[key] = own

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
    # What S was taken over, so that a person (and the fixtures) can see the
    # list and not only its maximum.
    print(f"check_plugin_image: S = {sink} B over the functions the base may "
          "be handed: ["
          + ", ".join(f"{n} {o} B" for _a, (n, o, _w) in sorted(
              listed.items(), key=lambda kv: (kv[1][0], kv[0])))
          + "]")
    return 0


if __name__ == "__main__":
    sys.exit(main())
