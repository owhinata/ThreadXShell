#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 ThreadX Shell Project
"""Gate 5: only the seam may reach the vendor's NOR write path (issue #88).

The external NOR carries the bootloader, the firmware image and the
bootloader's slot header, and issue #49 needs to WRITE part of it.  So unlike
the vendor timer API -- which is barred outright and whose seam never calls
__real_ (check_timer_seam.py) -- the vendor erase and program code IS in the
image here, and the claim is about WHO MAY REACH IT.

[!] AND THAT CLAIM IS NARROW.  It says nothing about whether the firmware can
write this flash some other way, because it demonstrably can: the read/XIP path
already links hx_drv_spi_mst_get_dev, hx_drv_dmac_get_dev and the vendor's
DMA_send / set_DMA_config / waitWIP / setWriteEnable helpers, none of which can
be removed without losing the read path, and those are enough to assemble WREN
plus an arbitrary opcode without naming one symbol this file looks at.  Direct
MMIO is beyond any of it.  What is checked here is that the ONE path the vendor
library offers goes through port/sdk_seam/nor_seam.c.  Read "the list passed" as
"this door is bolted", never as "there is no other door" (issue #87).

WHY THE LINKER'S MAP AND NOT THE FINISHED ELF
---------------------------------------------
The finished ELF cannot answer the question.  Once --gc-sections has run there
is no record of which input section a surviving instruction came from, nor of
which references were dropped -- and the vendor's own outer forwarders
(hx_lib_spi_eeprom_write and friends, in spi_eeprom_comm.o) each hold a
relocation to the inner name this seam wraps.  That object is ALREADY a link
input, because open() / read_ID() / enable_XIP() live in it.  So:

  - an object-level rule ("only the writer names the inner symbols") fails
    permanently, on a link that is correct;
  - and allowing that object would reopen the hole, because another translation
    unit only has to keep the OUTER forwarder alive to reach the inner name
    through it.

The unit that separates those two cases is the INPUT SECTION, and whether an
input section survived is a fact only the linker holds.  --print-gc-sections is
a diagnostic stream, not an artifact.  The map is the artifact: its "Discarded
input sections" block and its output-section listing between them classify every
input section the linker considered.

WHAT IS ASSERTED

  N1  the map describes THIS ELF.  Live input sections named .text.<symbol> must
      sit at the address <symbol> has in the ELF.  An old map is otherwise a
      silent fail-open: every rule below would be evaluated against a link that
      is not the one being checked.  (board.cmake also deletes the map PRE_LINK,
      so a map can only exist because this link wrote it.  Belt and braces --
      the fixture for a stale map is what says this half bites.)
  N2  every linker input is accounted.  Each LOAD in the map is either in the
      manifest board.cmake generated or under the toolchain root; an input from
      neither is refused rather than skipped.
  N3  the toolchain's own inputs do not name the write path.  Cheap, and it is
      what lets N4..N9 restrict themselves to the manifest.
  N4  no LTO, and nothing unreadable.  A manifest entry that is not an ELF
      relocatable (or an archive of them), or that carries LTO IR, is refused:
      the relocation audit below can see neither.
  N5  every live reference to an inner vendor name comes from an authorised
      caller.  With no --allow-caller given -- the state until issue #88's
      writer lands -- that means there must be none at all.
  N6  every live reference to __real_ comes from the seam object.
  N7  every live reference to __wrap_ comes from an authorised caller.  (The
      probe's forced references are -u flags, not relocations, so they do not
      appear here.)
  N8  the two entry points with nothing to bound them -- chip erase, and the
      word-at-a-time programmer -- are referenced by nobody, under any of their
      three names, and their vendor implementations are absent from the ELF.
      That is what keeps check_placement_budget.py's absence rule covering them.
  N9  every reference to a name of interest is a direct call or jump.  Taking
      the ADDRESS of one of these is not a call this audit can follow, and
      &__real_sym in particular hands out a pointer that --wrap cannot see
      through.
  N10 the vendor object that DEFINES the inner names does not call them itself.
      --wrap rewrites undefined references only, so a self-call inside
      qspi_eeprom_interface.o would go straight past the seam.  There are none
      today; this is that fact turned into a check.
  N11 the vendor's outer forwarders are dead and their symbols absent.
  N16 and nothing calls one.  N16 is separate from N11 on purpose: bringing a
      forwarder back to life trips both, so a negative test that only checked
      "the gate refused" would stay green with this edge rule deleted.
  N12 every section this audit classified was classified exactly once.  A
      section the map places in neither list, or in both, is a gate error --
      never an assumption.
  N13 the wrappers exist, and the archive is really in the link.
  N14 (--require-live-wrappers) the seam is LIVE and reaches the vendor: all
      four wrappers defined, and the two permitted vendor entry points present.
      Without this the gate is satisfied by a link that has no write path at
      all, which is true of the firmware until the writer lands and is exactly
      why board.cmake forces the references in seam_probe.
  N15 the interval the seam was compiled to enforce is the layout board.cmake
      declared.  Read out of the nor_seam_limits record in .rodata, because at
      -Os the bounds do not survive as instruction literals (the compiler turns
      `lo <= a && a < hi` into `a - lo <u hi - lo`).

Stdlib-only; POST_BUILD.
"""

import argparse
import os
import re
import subprocess
import sys

INNER_PERMITTED = [
    "hx_lib_qspi_eeprom_erase_sector",
    "hx_lib_qspi_eeprom_write",
]
# [!] The two with no address to bound them.  erase_all is a chip erase; the
# word-at-a-time programmer would be a second write path with its own rules.
# nor_seam.c refuses both without naming __real_, which is what lets the linker
# drop the vendor implementations and check_placement_budget.py go on barring
# them by absence.
INNER_REFUSED = [
    "hx_lib_qspi_eeprom_erase_all",
    "hx_lib_qspi_eeprom_word_write",
]
INNER = INNER_PERMITTED + INNER_REFUSED
# The vendor's outer forwarders: they pick a bus by id and tail into the inner
# form.  Wrapping the inner names covers them, and they must stay dead.
OUTER = ["hx_lib_spi_eeprom_" + n[len("hx_lib_qspi_eeprom_"):] for n in INNER]

REAL = ["__real_" + n for n in INNER]
WRAP = ["__wrap_" + n for n in INNER]
OF_INTEREST = set(INNER) | set(OUTER) | set(REAL) | set(WRAP)

# The object that defines the inner names.  Named because N10 is about IT.
VENDOR_INNER_MEMBER = "qspi_eeprom_interface.o"
VENDOR_OUTER_MEMBER = "spi_eeprom_comm.o"

# Direct, followable control transfer.  Everything else against a name of
# interest -- absolute, PC-relative data, movw/movt halves, GOT forms -- is an
# address being taken, and an address is not an edge this audit can follow.
CALL_RELOCS = {
    "R_ARM_CALL", "R_ARM_JUMP24", "R_ARM_PLT32",
    "R_ARM_THM_CALL", "R_ARM_THM_JUMP24", "R_ARM_THM_JUMP19",
    "R_ARM_THM_PC22",
}

# Sections that carry LTO IR.  GCC emits .gnu.lto_* / .gnu_lto_*; a bitcode file
# is not an ELF at all and fails the format check first.
LTO_SECTION = re.compile(r"^\.gnu[._]lto")


def run(cmd, check=True):
    r = subprocess.run(cmd, capture_output=True, text=True)
    if check and r.returncode != 0:
        raise SystemExit("check_nor_seam: %s failed:\n%s"
                         % (" ".join(cmd[:2]), r.stderr[-2000:]))
    return r.stdout


# --- the map -----------------------------------------------------------------

# ` .text.foo   0x1002c6a0   0x1c  path` -- one leading space marks an INPUT
# section; output sections start in column 0.
_ONE_LINE = re.compile(r"^ (\.\S+)\s+0x([0-9a-fA-F]+)\s+0x([0-9a-fA-F]+)\s+(\S.*)$")
# A long name goes on its own line, with the numbers and the file on the next.
_NAME_ONLY = re.compile(r"^ (\.\S+)\s*$")
_NUMS_ONLY = re.compile(r"^\s+0x([0-9a-fA-F]+)\s+0x([0-9a-fA-F]+)\s+(\S.*)$")
_ARCHIVE_MEMBER = re.compile(r"^(.*\.a)\((.*\.o(?:bj)?)\)$")


def _file_key(path, base=""):
    """(realpath, member) -- an archive member is not the archive.

    [!] REALPATH, ALWAYS.  The map names objects relative to the build directory
    and archives absolutely, while objdump reports whatever path it was handed.
    Keying on the string as printed gives the same object two identities, and
    then every lookup against the map misses -- which surfaces as "the map says
    nothing about this section", a gate error rather than a false pass, but a
    gate error on a link that is correct.
    """
    path = path.strip()
    m = _ARCHIVE_MEMBER.match(path)
    if m:
        return (os.path.realpath(os.path.join(base, m.group(1))), m.group(2))
    return (os.path.realpath(os.path.join(base, path)), None)


def _collect_sections(lines, base):
    """Every (file, member, section, addr, size) an input-section line names."""
    out, pending = [], None
    for line in lines:
        m = _ONE_LINE.match(line)
        if m:
            f, mem = _file_key(m.group(4), base)
            out.append((f, mem, m.group(1), int(m.group(2), 16),
                        int(m.group(3), 16)))
            pending = None
            continue
        m = _NAME_ONLY.match(line)
        if m:
            pending = m.group(1)
            continue
        if pending is not None:
            m = _NUMS_ONLY.match(line)
            if m:
                f, mem = _file_key(m.group(3), base)
                out.append((f, mem, pending, int(m.group(1), 16),
                            int(m.group(2), 16)))
            pending = None
    return out


class LinkMap:
    def __init__(self, path, link_dir):
        if not os.path.exists(path):
            raise SystemExit(
                "check_nor_seam: %s does not exist.  board.cmake deletes the "
                "map before every link and names it a BYPRODUCT so ninja "
                "rebuilds it; a missing one means this gate was run by hand "
                "against a link that has not happened." % path)
        # [!] THE LINKER'S WORKING DIRECTORY, NOT THE MAP'S.  A map names
        # objects relative to where the link RAN.  Those coincide for the real
        # build and do not for a fixture, whose map is written into a
        # subdirectory -- and resolving against the wrong one turns every object
        # into a path that exists nowhere, which surfaces as "this input is in
        # no manifest" for the whole link.
        self.base = os.path.abspath(link_dir)
        with open(path, errors="replace") as f:
            lines = f.read().splitlines()

        def find(title):
            for i, l in enumerate(lines):
                if l.strip() == title:
                    return i
            return None

        i_disc = find("Discarded input sections")
        i_mem = find("Memory Configuration")
        i_lsm = find("Linker script and memory map")
        i_xref = find("Cross Reference Table")
        if i_disc is None or i_mem is None or i_lsm is None:
            raise SystemExit("check_nor_seam: %s is not a GNU ld map this gate "
                             "can read (missing section headers) -- refusing "
                             "rather than guessing" % path)
        if i_xref is None:
            i_xref = len(lines)

        self.discarded = {}
        for f, mem, sec, _a, _s in _collect_sections(lines[i_disc:i_mem],
                                                     self.base):
            self.discarded.setdefault((f, mem, sec), 0)
            self.discarded[(f, mem, sec)] += 1

        self.live = {}
        for f, mem, sec, addr, size in _collect_sections(lines[i_lsm:i_xref],
                                                         self.base):
            key = (f, mem, sec)
            self.live.setdefault(key, [])
            self.live[key].append((addr, size))

        self.loads = [l[len("LOAD "):].strip()
                      for l in lines[i_lsm:i_xref] if l.startswith("LOAD ")]

    def classify(self, f, mem, sec):
        """'live', 'discarded', or None when the map does not say once."""
        in_live = (f, mem, sec) in self.live
        in_dead = (f, mem, sec) in self.discarded
        if in_live and in_dead:
            return None
        if in_live:
            return "live"
        if in_dead:
            return "discarded"
        return None


# --- the ELF -----------------------------------------------------------------

def elf_symbols(nm, elf):
    """name -> {address, ...}, for defined symbols.

    [!] A SET, NOT ONE ADDRESS.  Several translation units here have a static
    helper called fail(), and -Os clones each into its own `fail.isra.0`.  The
    names collide, the addresses do not, and a name->address map keeps whichever
    nm printed last -- which made the map/ELF cross-check below report three
    mismatches on a link that was correct.
    """
    syms = {}
    for line in run([nm, "--defined-only", elf]).splitlines():
        parts = line.split()
        if len(parts) == 3:
            syms.setdefault(parts[2], set()).add(int(parts[0], 16))
    return syms


def read_words(objdump, elf, addr, count):
    """`count` little-endian 32-bit words at `addr`, out of the image."""
    out = run([objdump, "-s", "--start-address=0x%x" % addr,
               "--stop-address=0x%x" % (addr + 4 * count), elf])
    data = bytearray()
    for line in out.splitlines():
        m = re.match(r"^\s*([0-9a-f]{4,16})\s((?:[0-9a-f]{2,8}\s+)+)", line)
        if not m:
            continue
        # [!] AT MOST FOUR GROUPS.  objdump -s prints up to four words and then
        # the same bytes as ASCII; content that happens to spell hex digits
        # would otherwise be read back as more data.
        for group in m.group(2).split()[:4]:
            try:
                data += bytes.fromhex(group)
            except ValueError:
                pass
    if len(data) < 4 * count:
        return None
    return [int.from_bytes(data[4 * i:4 * i + 4], "little")
            for i in range(count)]


# --- the inputs --------------------------------------------------------------

def read_manifest(path):
    entries = []
    with open(path) as f:
        for line in f:
            line = line.strip()
            if line and not line.startswith("#"):
                entries.append(line)
    return entries


def alloc_sections(objdump, files):
    """{(file, member): {section, ...}} for ALLOC sections only."""
    out = {}
    if not files:
        return out
    cur_file, cur_mem, pending = None, None, None
    for line in run([objdump, "-h"] + files).splitlines():
        m = re.match(r"^In archive (\S.*):$", line)
        if m:
            cur_file, cur_mem = os.path.realpath(m.group(1)), None
            continue
        m = re.match(r"^(\S.*?):\s+file format", line)
        if m:
            name = m.group(1)
            if cur_file is not None and cur_file.endswith(".a") and \
               not os.path.exists(name):
                cur_mem = name
            else:
                cur_file, cur_mem = os.path.realpath(name), None
            continue
        m = re.match(r"^\s*\d+\s+(\S+)\s+[0-9a-f]+", line)
        if m:
            pending = m.group(1)
            continue
        if pending is not None and "ALLOC" in line:
            out.setdefault((cur_file, cur_mem), set()).add(pending)
            pending = None
    return out


def scan_symbols(nm, files, flag):
    """{(file, member): {symbol, ...}} from one nm pass over many files."""
    out = {}
    if not files:
        return out
    cur_file, cur_mem = os.path.realpath(files[0]), None
    for line in run([nm, flag] + files).splitlines():
        line = line.rstrip()
        if not line:
            continue
        m = re.match(r"^(\S.*):$", line)
        if m:
            name = m.group(1)
            if name.endswith(".a"):
                cur_file, cur_mem = os.path.realpath(name), None
            elif cur_file.endswith(".a") and not os.path.exists(name):
                cur_mem = name
            else:
                cur_file, cur_mem = os.path.realpath(name), None
            continue
        parts = line.split()
        if not parts:
            continue
        sym = parts[-1]
        out.setdefault((cur_file, cur_mem), set()).add(sym)
    return out


def scan_relocations(objdump, files):
    """[(file, member, section, reloc_type, symbol), ...]."""
    recs = []
    if not files:
        return recs
    cur_file, cur_mem, cur_sec = os.path.realpath(files[0]), None, None
    for line in run([objdump, "-r"] + files).splitlines():
        m = re.match(r"^In archive (\S.*):$", line)
        if m:
            cur_file, cur_mem = os.path.realpath(m.group(1)), None
            cur_sec = None
            continue
        m = re.match(r"^(\S.*?):\s+file format", line)
        if m:
            name = m.group(1)
            if cur_file.endswith(".a") and not os.path.exists(name):
                cur_mem = name
            else:
                cur_file, cur_mem = os.path.realpath(name), None
            cur_sec = None
            continue
        m = re.match(r"^RELOCATION RECORDS FOR \[(.*)\]:$", line)
        if m:
            cur_sec = m.group(1)
            continue
        m = re.match(r"^[0-9a-f]+\s+(\S+)\s+(\S+)", line)
        if m and cur_sec is not None:
            sym = re.split(r"[+\-]", m.group(2))[0]
            recs.append((cur_file, cur_mem, cur_sec, m.group(1), sym))
    return recs


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--nm", required=True)
    ap.add_argument("--objdump", required=True)
    ap.add_argument("--link-dir", required=True,
                    help="the directory the link RAN in; the map's relative "
                         "input paths are resolved against it")
    ap.add_argument("--map", required=True,
                    help="the ld map for THIS link (board.cmake deletes it "
                         "PRE_LINK, so its presence means this link wrote it)")
    ap.add_argument("--manifest", required=True,
                    help="every non-toolchain linker input, one path per line")
    ap.add_argument("--toolchain-root", required=True,
                    help="prefix of the compiler's own inputs (crt*, libc, ...)")
    ap.add_argument("--seam-object", required=True,
                    help="path suffix of the object that owns __real_*")
    ap.add_argument("--allow-caller", action="append", default=[],
                    help="path suffix of an object allowed to call the wrapped "
                         "names; repeatable, empty until the writer lands")
    ap.add_argument("--writable-lo", required=True)
    ap.add_argument("--writable-hi", required=True)
    ap.add_argument("--erase-unit", required=True)
    ap.add_argument("--require-live-wrappers", action="store_true",
                    help="fail unless the seam is actually reachable in this "
                         "link (used on seam_probe)")
    ap.add_argument("elf")
    args = ap.parse_args()

    errors = []

    def fail(ident, msg):
        errors.append("[%s] %s" % (ident, msg))

    def is_seam(f):
        return f.endswith(args.seam_object)

    def is_caller(f):
        return any(f.endswith(s) for s in args.allow_caller)

    def show(f, mem, sec):
        base = os.path.basename(f)
        return "%s(%s)[%s]" % (base, mem, sec) if mem else "%s[%s]" % (base, sec)

    lm = LinkMap(args.map, args.link_dir)
    syms = elf_symbols(args.nm, args.elf)

    # N1. the map describes THIS ELF.
    checked, mismatched = 0, []
    for (f, mem, sec), places in lm.live.items():
        if not sec.startswith(".text."):
            continue
        name = sec[len(".text."):]
        if name not in syms:
            continue
        if not any(a in syms[name] for a, _s in places):
            mismatched.append("%s: map 0x%x, ELF %s"
                              % (show(f, mem, sec), places[0][0],
                                 ", ".join("0x%x" % a
                                           for a in sorted(syms[name]))))
        checked += 1
    if mismatched:
        fail("N1", "the map does not describe this ELF (%d of %d live text "
                   "sections are at a different address; e.g. %s) -- every "
                   "rule below would be checked against a different link"
                   % (len(mismatched), checked, mismatched[0]))
    elif checked < 50:
        fail("N1", "only %d live .text.<symbol> sections could be matched "
                   "against the ELF; that is too few to establish the map "
                   "belongs to this link" % checked)

    # N2. every linker input is accounted.
    manifest = read_manifest(args.manifest)
    manifest_real = {}
    for entry in manifest:
        manifest_real[os.path.realpath(entry)] = entry
    tc_root = os.path.realpath(args.toolchain_root)
    unaccounted, toolchain_inputs = [], set()
    for path in lm.loads:
        real = os.path.realpath(os.path.join(args.link_dir, path))
        if real in manifest_real:
            continue
        if real.startswith(tc_root + os.sep) or real == tc_root:
            toolchain_inputs.add(real)
            continue
        # ld names its own internal pseudo-input here.  Matched exactly: a
        # prefix test would also excuse a real object called linker_something.o.
        if path == "linker stubs":
            continue
        unaccounted.append(path)
    for u in sorted(set(unaccounted)):
        fail("N2", "linker input %s is in neither the manifest nor the "
                   "toolchain; this audit cannot see what it references" % u)

    # N3. the toolchain's own inputs do not name the write path.
    tc_files = sorted(toolchain_inputs)
    tc_undef = scan_symbols(args.nm, tc_files, "--undefined-only")
    for (f, mem), names in tc_undef.items():
        for bad in sorted(names & OF_INTEREST):
            fail("N3", "%s references %s -- a compiler-supplied input is "
                       "reaching the NOR write path" % (show(f, mem, "-"), bad))

    # N4. the manifest is readable, and free of LTO.
    missing = [e for e in manifest if not os.path.exists(e)]
    for m in missing:
        fail("N4", "manifest entry %s does not exist" % m)
    present = [e for e in manifest if os.path.exists(e)]
    if present:
        r = subprocess.run([args.objdump, "-h"] + present,
                           capture_output=True, text=True)
        if r.returncode != 0:
            fail("N4", "a manifest entry is not an ELF object or archive this "
                       "gate can read:\n%s" % r.stderr.strip()[-800:])
        for line in r.stdout.splitlines():
            m = re.match(r"^\s*\d+\s+(\S+)\s+[0-9a-f]+", line)
            if m and LTO_SECTION.match(m.group(1)):
                fail("N4", "a linker input carries LTO IR (%s); relocations "
                           "against the vendor names do not exist yet at this "
                           "stage and nothing here could see them" % m.group(1))
                break

    # The audit set: everything that mentions a name of interest, either as an
    # undefined reference or as a definition (N10 is about a DEFINING object).
    undef = scan_symbols(args.nm, present, "--undefined-only")
    defined = scan_symbols(args.nm, present, "--defined-only")
    candidates = sorted({f for (f, _m), names in undef.items()
                         if names & OF_INTEREST}
                        | {f for (f, _m), names in defined.items()
                           if names & OF_INTEREST})
    allocs = alloc_sections(args.objdump, candidates)
    recs = [r for r in scan_relocations(args.objdump, candidates)
            if r[4] in OF_INTEREST]

    for f, mem, sec, rtype, sym in recs:
        # Only ALLOCATABLE sections carry code that can run.  DWARF references
        # a function symbol too, and a debug reference is not an edge.
        if sec not in allocs.get((f, mem), set()):
            continue
        state = lm.classify(f, mem, sec)

        # N12. classification is exact, or this is a gate error.
        if state is None:
            fail("N12", "the map classifies %s as neither live nor discarded "
                        "(or as both); it references %s and this gate will not "
                        "guess" % (show(f, mem, sec), sym))
            continue

        # N9. only direct control transfer, whatever the verdict below.
        if rtype not in CALL_RELOCS:
            fail("N9", "%s takes the ADDRESS of %s (%s); that is not an edge "
                       "this audit can follow, and a pointer to __real_ is one "
                       "--wrap cannot see through"
                       % (show(f, mem, sec), sym, rtype))

        # N10. the object that DEFINES the inner names must not call them.
        if mem == VENDOR_INNER_MEMBER and sym in INNER:
            fail("N10", "%s calls %s, which it defines -- --wrap rewrites "
                        "undefined references only, so that call would go "
                        "straight past the seam" % (show(f, mem, sec), sym))

        # N8. the two with nothing to bound them are referenced by nobody.
        if sym in INNER_REFUSED or sym in ["__real_" + n for n in INNER_REFUSED] \
           or sym in ["__wrap_" + n for n in INNER_REFUSED]:
            if state == "live":
                fail("N8", "%s references %s; this port refuses chip erase and "
                           "the word-at-a-time programmer unconditionally, and "
                           "a live reference means something can reach one"
                           % (show(f, mem, sec), sym))
            continue

        if state != "live":
            continue

        # N5/N6/N7. the edge policy, on live sections only.
        if sym in INNER:
            if not is_caller(f):
                fail("N5", "%s calls %s directly; the only translation units "
                           "allowed to are %s"
                     % (show(f, mem, sec), sym,
                        ", ".join(args.allow_caller) or "(none yet)"))
        elif sym in REAL:
            if not is_seam(f):
                fail("N6", "%s references %s; only the seam (%s) may call "
                           "through to the vendor implementation"
                     % (show(f, mem, sec), sym, args.seam_object))
        elif sym in WRAP:
            if not is_caller(f) and not is_seam(f):
                fail("N7", "%s references %s by its wrapper name, bypassing "
                           "the caller policy" % (show(f, mem, sec), sym))
        elif sym in OUTER:
            # [!] ITS OWN ID, deliberately.  Keeping the forwarder alive also
            # makes its symbol survive, which N11 catches on its own -- so if
            # this edge rule were dead, a fixture asserting only "the link was
            # refused" would still be green.  The two diagnostics are separate
            # so the negative test can require that BOTH fired.
            fail("N16", "%s calls the vendor's outer forwarder %s, reaching the "
                        "inner entry point through vendor code instead of "
                        "through the seam" % (show(f, mem, sec), sym))

    # N11. the outer forwarders are dead, and their symbols are gone.
    for name in OUTER:
        if name in syms:
            fail("N11", "%s survived into the image; the vendor's outer "
                        "forwarder reaches the write path without passing the "
                        "caller policy" % name)
    for name in OUTER:
        want_sec = ".text." + name
        found = [(f, mem, sec) for (f, mem, sec) in
                 list(lm.live) + list(lm.discarded)
                 if mem == VENDOR_OUTER_MEMBER and sec == want_sec]
        if not found:
            fail("N12", "the map says nothing about %s(%s); it is the section "
                        "the outer-forwarder rule is about"
                 % (VENDOR_OUTER_MEMBER, want_sec))
            continue
        f, mem, sec = found[0]
        if lm.classify(f, mem, sec) != "discarded":
            fail("N11", "%s is LIVE; something is keeping the vendor's outer "
                        "forwarder alive" % show(f, mem, sec))

    # N8 (second half): the refused pair must not be in the image at all.
    for name in INNER_REFUSED:
        if name in syms:
            fail("N8", "%s is in the image; its wrapper is supposed to refuse "
                       "without naming __real_, which is what lets the linker "
                       "drop it and check_placement_budget.py go on barring it"
                 % name)

    # N13. the wrappers exist as definitions, and the archive is in the link.
    seam_defs = set()
    for (f, mem), names in defined.items():
        if is_seam(f):
            seam_defs |= names
    for name in WRAP:
        if name not in seam_defs:
            fail("N13", "%s is not defined in %s; a --wrap with no wrapper is "
                        "a link error waiting to happen, or a redirect that "
                        "silently went nowhere" % (name, args.seam_object))
    if not any(os.path.basename(e) == "lib_spi_eeprom.a" for e in manifest):
        fail("N13", "lib_spi_eeprom.a is not a linker input; every rule here "
                    "would pass vacuously on a link without it")

    # N14. the seam is really reachable in this link.
    if args.require_live_wrappers:
        for name in WRAP:
            if name not in syms:
                fail("N14", "%s is not in the image; with the references forced "
                            "this link cannot be missing a wrapper" % name)
        for name in INNER_PERMITTED:
            if name not in syms:
                fail("N14", "%s is not in the image; the seam's __real_ call "
                            "is not reaching the vendor implementation, so the "
                            "rules above have nothing to be true about" % name)

    # N15. the interval the seam enforces is the one board.cmake declared.
    want = (int(args.writable_lo, 0), int(args.writable_hi, 0),
            int(args.erase_unit, 0))
    if "nor_seam_limits" not in syms:
        fail("N15", "nor_seam_limits is not in the image; the interval the "
                    "seam enforces cannot be read back, and the wrappers were "
                    "compiled against something this gate cannot see")
    elif len(syms["nor_seam_limits"]) != 1:
        fail("N15", "nor_seam_limits is defined at %d addresses; the record "
                    "the seam enforces is not identifiable"
             % len(syms["nor_seam_limits"]))
    else:
        addr = next(iter(syms["nor_seam_limits"]))
        got = read_words(args.objdump, args.elf, addr, 3)
        if got is None:
            fail("N15", "could not read nor_seam_limits out of the image")
        elif tuple(got) != want:
            fail("N15", "the seam enforces 0x%x..0x%x unit 0x%x, but "
                        "board.cmake declared 0x%x..0x%x unit 0x%x"
                 % (got[0], got[1], got[2], want[0], want[1], want[2]))

    if errors:
        print("check_nor_seam: FAIL", file=sys.stderr)
        for e in errors:
            print("  - " + e, file=sys.stderr)
        return 1

    live_edges = sum(1 for f, mem, sec, _t, sym in recs
                     if sym in REAL and lm.classify(f, mem, sec) == "live")
    print("check_nor_seam: OK (%d inputs audited, %d live __real_ edge(s), "
          "writable 0x%x..0x%x unit 0x%x)"
          % (len(candidates), live_edges, want[0], want[1], want[2]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
