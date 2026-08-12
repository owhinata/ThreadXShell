#!/usr/bin/env python3
"""Sector-0 safety gate for the Wio Lite AI DFU bootloader (boards/wio-lite-ai/boot/).

WHY THIS EXISTS
---------------
The bootloader owns internal flash sector 0 (0x08000000, 128 KB).  Writing that
sector is the one operation that can brick the board, exactly one board is left
(#1 is a permanent paperweight), and the bootloader is also the thing that
recovers a bad app image -- so it is both the most dangerous and the least
replaceable code in the repository.

The donor repository had no build-time verification at all: its boot/README.md
asked a human to run four objdump commands by hand.  That works exactly as long
as somebody remembers.  This script is those checks, plus the ones the hand
procedure could not express, run on every build.

WHAT IT IS AND IS NOT
---------------------
The load-bearing guarantees are, in order:

  C6a  the bootloader SOURCES are byte-for-byte the ones that were reviewed, and
       no file has been added to or removed from the tree;
  C6b  the linked IMAGE is byte-for-byte the golden one;
  C4   the flash-writing call graph in the image is the expected one.

C3 (no option-byte / RDP / DBGMCU constants or symbols) is defence in depth, not
proof: an unlock key can in principle be computed at run time or fetched from
data, so "the key is not in the image" does not mean "the option bytes cannot be
written".  It is a cheap layer on top of the three above, and it is worth having
because the failure it screens for is unrecoverable.

Deliberately NOT checked: whether the bootloader reconfigures the SWD pins
(PA13/PA14).  HAL_GPIO_Init() takes a struct built at run time, so no static
check can answer it, and a pattern match would fail open at the first codegen
change.  A gate that passes silently is worse than no gate; this gap is recorded
here and left to source immutability (C6a) and the golden image (C6b).

MODES
-----
  precheck   Runs BEFORE the link, reads no build artefact, and touches a stamp
             file on success.  boot_image lists that stamp in LINK_DEPENDS, so a
             failure here stops the build before anything is produced, and a
             successful run is what makes boot_image relink -- which is in turn
             what guarantees the POST_BUILD half below always runs.  Checks: C6a
             and the compile-command audit (no LTO on any bootloader object).

  postlink   Runs as boot_image's POST_BUILD, on the ELF, the raw .bin and the
             objects they were built from.  Checks: C1, C2, the ELF<->bin join,
             C3, C4, C5, no LTO IR in any object, C6b.

EXIT STATUS
-----------
  0  pass
  1  a policy violation was proven
  2  the check could not be performed -- NOT a pass.  "No caller found" and "no
     caller exists" are indistinguishable byte-for-byte, so anything that makes
     the analysis unreliable stops the build and asks for a human.

Every failure prints a stable diagnostic ID (BOOT-Cx-...).  The negative tests in
cmake/fixtures/ match on those IDs, not merely on "non-zero", so a refactor that
starts failing for a different reason than intended is itself a test failure.
"""

import argparse
import hashlib
import json
import os
import re
import shlex
import stat
import struct
import subprocess
import sys

CANNOT_CHECK = 2       # exit code for "the check did not run", never a pass

TOOL = "check_boot_safety"

# --- The memory map this bootloader is required to live in -------------------
# RM0468 sec 4.3.4 / Table 15: the H725AE's internal flash is four 128 KB
# sectors from 0x08000000.  Sector 0 is the bootloader; 1-3 are the app.
FLASH_BASE = 0x08000000
SECTOR0_SIZE = 128 * 1024
APP_BASE = 0x08020000

# Windows the initial MSP may point into: the internal RAMs, per the ROM linker
# script's MEMORY block (RM0468 Table 7).  The measured value is 0x24050000 --
# the TOP of AXI-SRAM, because the Cortex-M7 stack is full-descending.
RAM_WINDOWS = (
    ("AXI-SRAM", 0x24000000, 320 * 1024),
    ("DTCM", 0x20000000, 128 * 1024),
)

# --- C3: symbols ------------------------------------------------------------
# An ALLOWLIST, not a denylist.  A denylist is written against today's spelling
# of the HAL, and fails OPEN on the day upstream renames or adds a function --
# which is the day it matters.  Everything the image defines whose name looks
# like a flash driver must be one of these seven.
FLASH_SYMBOL_RE = re.compile(r"^(HAL_)?FLASH")
FLASH_SYMBOL_ALLOWLIST = frozenset(
    (
        "HAL_FLASH_Unlock",
        "HAL_FLASH_Lock",
        "HAL_FLASH_GetError",
        "HAL_FLASH_Program",
        "HAL_FLASHEx_Erase",
        "FLASH_WaitForLastOperation",
        # Allowed ONLY under the three conditions in check_flash_irq_handler().
        # The name alone would fail open: a future strong implementation that
        # banged FLASH registers directly would pass a name-based allowlist.
        "FLASH_IRQHandler",
    )
)

# Anything that can drop the debug connection or rewrite the option bytes.
# AGENTS.md invariant 6 forbids both outright.
DBGMCU_SYMBOL_RE = re.compile(r"DBGMCU")
LOWPOWER_SYMBOL_RE = re.compile(r"^HAL_PWR(Ex)?_Enter")

# --- C3: constants ----------------------------------------------------------
# NOT the absolute register addresses the donor README grepped for.  The HAL
# writes through base+offset (WRITE_REG(FLASH->OPTKEYR, ...)), so 0x52002008 and
# friends never appear as literals -- measured: zero occurrences, while the bank
# base 0x52002000 legitimately appears seven times.  That grep passed because it
# could not fail.
#
# What CANNOT be avoided is the option-byte unlock key: the hardware will not
# open OPTCR without exactly these two words (RM0468 sec 4.5.1).  Anything that
# means to rewrite the option bytes has to produce them.
FORBIDDEN_CONSTANTS = (
    ("BOOT-C3-OPTKEY", "FLASH_OPT_KEY1", 0x08192A3B),
    ("BOOT-C3-OPTKEY", "FLASH_OPT_KEY2", 0x4C5D6E7F),
    ("BOOT-C3-DBGMCU", "DBGMCU base", 0x5C001000),
)
# Deliberately NOT scanned: 0x52002000 (FLASH bank base, 7 legitimate hits) and
# 0x45670123 / 0xCDEF89AB (FLASH_KEY1/2 -- the ordinary program/erase unlock the
# bootloader is supposed to have, 1 hit each).

# --- C4: the flash-writing call graph ---------------------------------------
FLASH_WRITE_APIS = ("HAL_FLASHEx_Erase", "HAL_FLASH_Program", "HAL_FLASH_Unlock")
ALLOWED_FLASH_CALLERS = frozenset(("iflash_erase_sector", "iflash_program"))
REQUIRED_EDGES = (
    ("iflash_erase_sector", "HAL_FLASHEx_Erase"),
    ("iflash_program", "HAL_FLASH_Program"),
)

# --- C5 ---------------------------------------------------------------------
# If the include order regresses and TinyUSB picks up the APP's tusb_config.h
# (CFG_TUD_DFU=0), the bootloader compiles AND links -- and comes out with no DFU
# class at all.  Only a flashed board would show that, which is the one way we
# have decided never to find out.
REQUIRED_DFU_SYMBOL = "tud_dfu_finish_flashing"

# The one file in boot/ that is allowed to diverge from the donor: the README was
# translated to English (this repository writes documentation in English).  It is
# therefore not in the hash manifest -- but it must still be a real file, or a
# symlink could satisfy the name-set check below.
BOOT_DOC_FILE = "README.md"

# GCC clone/localisation suffixes, same list as the other checkers in this repo.
CLONE_SUFFIX_RE = re.compile(
    r"\.(?:isra|constprop|part|lto_priv|cold|localalias)(?:\.\d+)?$"
)
VENEER_RE = re.compile(r"^__(.+)_veneer$")


def strip_clone_suffixes(name):
    """`foo.lto_priv.0` -> `foo`.  The suffixes stack, so this repeats."""
    while True:
        stripped = CLONE_SUFFIX_RE.sub("", name)
        if stripped == name:
            return name
        name = stripped


def canonical_symbol(name):
    """Clone suffixes stripped and long-branch veneers seen through."""
    name = strip_clone_suffixes(name)
    veneer = VENEER_RE.match(name)
    return veneer.group(1) if veneer else name


def die(ident, message):
    """Exit CANNOT_CHECK.  The gate did not run; that is not the same as passing."""
    print(f"{TOOL}: {ident}: {message}", file=sys.stderr)
    sys.exit(CANNOT_CHECK)


def run(cmd, ident="BOOT-TOOL"):
    try:
        return subprocess.run(cmd, capture_output=True, text=True, check=True).stdout
    except (OSError, subprocess.CalledProcessError) as exc:
        die(ident, f"cannot run {cmd[0]}: {exc}")


class Gate:
    """Collects proven violations so one run reports all of them, not just the first."""

    def __init__(self):
        self.failures = []
        self.notes = []

    def fail(self, ident, message):
        self.failures.append((ident, message))

    def note(self, message):
        self.notes.append(message)

    def finish(self, banner):
        for note in self.notes:
            print(f"{TOOL}: {note}")
        if not self.failures:
            print(f"{TOOL}: OK -- {banner}")
            return 0
        for ident, message in self.failures:
            print(f"{TOOL}: {ident}: {message}", file=sys.stderr)
        print(
            f"{TOOL}: FAILED -- {len(self.failures)} violation(s); "
            "the bootloader tree is frozen (AGENTS.md invariant 6)",
            file=sys.stderr,
        )
        return 1


# ===========================================================================
#  C6a -- the bootloader sources are the reviewed ones, and there are no others
# ===========================================================================
def sha256_file(path):
    digest = hashlib.sha256()
    try:
        with open(path, "rb") as handle:
            for chunk in iter(lambda: handle.read(1 << 16), b""):
                digest.update(chunk)
    except OSError as exc:
        die("BOOT-C6A-READ", f"cannot read {path}: {exc}")
    return digest.hexdigest()


def read_manifest(path):
    """`<sha256>  <path relative to the board directory>` per line, # comments allowed."""
    entries = []
    try:
        with open(path, "r", encoding="utf-8") as handle:
            lines = handle.read().splitlines()
    except OSError as exc:
        die("BOOT-C6A-MANIFEST", f"cannot read manifest {path}: {exc}")
    for lineno, line in enumerate(lines, 1):
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        parts = line.split(maxsplit=1)
        if len(parts) != 2 or not re.fullmatch(r"[0-9a-f]{64}", parts[0]):
            die(
                "BOOT-C6A-MANIFEST",
                f"{path}:{lineno}: expected '<sha256>  <relative path>', got {line!r}",
            )
        entries.append((parts[0], parts[1].strip()))
    if not entries:
        die("BOOT-C6A-MANIFEST", f"{path} lists no files -- nothing would be checked")
    return entries


def list_tree(root):
    """Every entry under @root recursively, relative to it.

    Directories and symlinks are reported as entries in their own right: an added
    subdirectory, or a `.inc` nobody thought to look for, is exactly what a check
    that only walked a known list of names would wave through.
    """
    found = []
    stack = [""]
    while stack:
        rel = stack.pop()
        directory = os.path.join(root, rel) if rel else root
        try:
            with os.scandir(directory) as it:
                for entry in it:
                    child = os.path.join(rel, entry.name) if rel else entry.name
                    found.append(child)
                    if entry.is_dir(follow_symlinks=False):
                        stack.append(child)
        except OSError as exc:
            die("BOOT-C6A-READ", f"cannot list {directory}: {exc}")
    return found


def require_regular_file(gate, ident, path):
    """lstat, not stat: a symlink to an identical file outside the tree is not the file."""
    try:
        st = os.lstat(path)
    except OSError as exc:
        gate.fail(ident, f"{path}: {exc}")
        return False
    if not stat.S_ISREG(st.st_mode):
        gate.fail(
            ident,
            f"{path} is not a regular file (mode 0o{stat.S_IFMT(st.st_mode):o}) -- "
            "symlinks, directories and special files are all rejected",
        )
        return False
    return True


def check_source_manifest(gate, board_dir, boot_dir, manifest_path):
    entries = read_manifest(manifest_path)

    # The boot root itself: a directory, and not a symlink to one.
    try:
        st = os.lstat(boot_dir)
    except OSError as exc:
        die("BOOT-C6A-ROOT", f"cannot lstat {boot_dir}: {exc}")
    if not stat.S_ISDIR(st.st_mode):
        gate.fail("BOOT-C6A-ROOT", f"{boot_dir} is not a directory (symlinked away?)")
        return

    boot_rel_prefix = os.path.relpath(boot_dir, board_dir) + os.sep
    manifest_boot_leaves = {
        rel[len(boot_rel_prefix):]
        for _, rel in entries
        if rel.startswith(boot_rel_prefix)
    }
    if not manifest_boot_leaves:
        die(
            "BOOT-C6A-MANIFEST",
            f"{manifest_path} names no file under {boot_rel_prefix} -- "
            "the source-set check would be vacuous",
        )
    expected = manifest_boot_leaves | {BOOT_DOC_FILE}

    actual = set(list_tree(boot_dir))
    unexpected = sorted(actual - expected)
    missing = sorted(expected - actual)
    if unexpected:
        gate.fail(
            "BOOT-C6A-FILESET",
            f"{boot_dir} has {len(unexpected)} entry/entries the manifest does not "
            f"account for: {', '.join(unexpected)} -- the boot tree is frozen, and a "
            "hash list alone cannot notice an ADDED file",
        )
    if missing:
        gate.fail(
            "BOOT-C6A-FILESET",
            f"{boot_dir} is missing {', '.join(missing)}",
        )

    # Every expected leaf must be a real file, README.md included -- checking only
    # the hashed sources would let a README.md symlink through the name-set test.
    for leaf in sorted(expected):
        require_regular_file(gate, "BOOT-C6A-FILETYPE", os.path.join(boot_dir, leaf))

    for want, rel in entries:
        path = os.path.join(board_dir, rel)
        if not require_regular_file(gate, "BOOT-C6A-FILETYPE", path):
            continue
        got = sha256_file(path)
        if got != want:
            gate.fail(
                "BOOT-C6A-HASH",
                f"{rel} has changed: expected sha256 {want}, got {got}.  "
                "The bootloader tree and the ROM linker script are immutable; if the "
                "change is genuinely intended it needs a reviewed exception and a new "
                "manifest AND a new golden image hash.",
            )
    gate.note(f"C6a: {len(entries)} manifest file(s) unchanged, boot/ file set exact")


# ===========================================================================
#  The compile-command audit -- no LTO IR in any bootloader object
# ===========================================================================
# WHY THE REAL COMMAND LINE AND NOT THE TARGET PROPERTY: source-level
# COMPILE_OPTIONS live on a different property than the target's, the target's
# own COMPILE_OPTIONS does not include usage requirements inherited from linked
# libraries, the legacy COMPILE_FLAGS path bypasses both, generator expressions
# are unresolved at configure time, and anything appended after the guard ran is
# invisible to it.  Only the command line decides, so only the command line is
# audited.
#
# This is the PRE-LINK fast path, and its claim is narrow: no bootloader compile
# command carries a literal -flto, and none of them can hide options from this
# scan behind a response file or a spec file.  It is NOT the guarantee -- a
# compiler launcher, or an object left over from a build that did use LTO, would
# both pass it.  check_objects_free_of_lto() looks at the produced objects for
# the IR itself, and that is what the no-LTO property actually rests on.
# Neither says anything about GIMPLE inside a toolchain archive, and neither is a
# statement about the whole image -- that is C6b's job.
def split_command(entry, index):
    """The entry's argv.  `command` is shell-quoted (JSON Compilation Database)."""
    if isinstance(entry.get("arguments"), list):
        return [str(a) for a in entry["arguments"]]
    command = entry.get("command")
    if not isinstance(command, str):
        die(
            "BOOT-CC-ENTRY",
            f"compile_commands.json entry {index} has neither 'command' nor 'arguments'",
        )
    try:
        return shlex.split(command)
    except ValueError as exc:
        die("BOOT-CC-ARGV", f"compile_commands.json entry {index}: cannot split: {exc}")


def extract_outputs(argv):
    """Every -o in @argv.  Both `-o path` and the joined `-opath` are GCC-legal."""
    outs = []
    i = 0
    while i < len(argv):
        arg = argv[i]
        if arg == "-o":
            if i + 1 >= len(argv):
                return None
            outs.append(argv[i + 1])
            i += 2
            continue
        if arg.startswith("-o") and len(arg) > 2:
            outs.append(arg[2:])
        i += 1
    return outs


def normalise(directory, path):
    """Absolute and lexically normalised, per-entry `directory` as the base.

    normpath and not realpath: resolving symlinks would make the comparison
    depend on how the build tree happens to be reached.
    """
    return os.path.normpath(os.path.join(directory, path))


def has_path_elements(path, needle):
    """True if @needle's components appear consecutively in @path's.

    A substring test would accept `.../not_boot_image.dir_backup/...`.
    """
    parts = os.path.normpath(path).split(os.sep)
    want = list(needle)
    span = len(want)
    return any(parts[i : i + span] == want for i in range(len(parts) - span + 1))


def read_expected_translation_units(path):
    """The source list CMake generated from the very list it fed add_executable().

    Being generated is the point: it cannot drift from the target.  What it buys is
    NOT "nobody removed a source" (the golden image covers that) but "the database
    really does describe this target" -- if EXPORT_COMPILE_COMMANDS silently stops
    working, or the object directory is renamed, the audit would otherwise inspect
    nothing at all and pass.
    """
    try:
        with open(path, "r", encoding="utf-8") as handle:
            units = [line.strip() for line in handle if line.strip()]
    except OSError as exc:
        die("BOOT-CC-TULIST", f"cannot read {path}: {exc}")
    if not units:
        die("BOOT-CC-TULIST", f"{path} is empty -- the audit would inspect nothing")
    return units


def elf_section_names(path):
    """The section names of an ELF file, or None if it is not one.

    Parsed here rather than shelled out to objdump: this runs over every object in
    the target and a subprocess apiece would be the slowest thing in the build.
    """
    try:
        with open(path, "rb") as handle:
            data = handle.read()
    except OSError as exc:
        die("BOOT-OBJ-READ", f"cannot read {path}: {exc}")
    if len(data) < 52 or data[:4] != b"\x7fELF":
        return None
    wide = data[4] == 2
    end = "<" if data[5] == 1 else ">"
    try:
        if wide:
            shoff = struct.unpack_from(end + "Q", data, 0x28)[0]
            shentsize, shnum, shstrndx = struct.unpack_from(end + "HHH", data, 0x3A)
            off_field, size_field = 0x18, 0x20
            off_fmt = end + "Q"
        else:
            shoff = struct.unpack_from(end + "I", data, 0x20)[0]
            shentsize, shnum, shstrndx = struct.unpack_from(end + "HHH", data, 0x2E)
            off_field, size_field = 0x10, 0x14
            off_fmt = end + "I"
        if shnum == 0 or shstrndx >= shnum:
            return []
        strtab = shoff + shstrndx * shentsize
        str_off = struct.unpack_from(off_fmt, data, strtab + off_field)[0]
        str_size = struct.unpack_from(off_fmt, data, strtab + size_field)[0]
        blob = data[str_off : str_off + str_size]
        names = []
        for index in range(shnum):
            header = shoff + index * shentsize
            name_off = struct.unpack_from(end + "I", data, header)[0]
            terminator = blob.find(b"\0", name_off)
            names.append(
                blob[name_off : terminator if terminator != -1 else None].decode(
                    "utf-8", "replace"
                )
            )
        return names
    except struct.error as exc:
        die("BOOT-OBJ-READ", f"{path} is a malformed ELF: {exc}")


# GCC puts its LTO intermediate representation in sections with these prefixes.
# Their presence is the property itself, not a proxy for it.
LTO_SECTION_PREFIXES = (".gnu.lto_", ".gnu.debuglto_")


def check_objects_free_of_lto(gate, object_dir, expected_count):
    """No object boot_image links carries LTO intermediate representation.

    WHY THIS AND NOT ONLY THE COMMAND LINE: scanning compile_commands.json for
    `-flto` proves something narrower than it looks.  A spec file
    (-specs=<file>) can hand -flto to cc1 without the token appearing in the
    command, a compiler launcher can rewrite the invocation, and an object can
    simply be stale from a build that did use LTO -- which is not hypothetical:
    an object rebuilt outside Ninja with the same command line stays "up to date"
    forever.  So the argv audit is kept as the pre-link fast path, and this is the
    guarantee: the produced objects are examined for the IR itself.

    It matters because C4 reads the call graph out of the linked image, and
    cross-translation-unit inlining of the HAL flash writers into iflash.c would
    delete the very edges it checks.
    """
    if not os.path.isdir(object_dir):
        die("BOOT-OBJ-DIR", f"{object_dir} is not a directory -- cannot audit objects")
    objects = []
    for root, _dirs, files in os.walk(object_dir):
        for name in files:
            if name.endswith((".obj", ".o")):
                objects.append(os.path.join(root, name))
    # A floor, not an equality: Ninja does not delete the object of a source that
    # was removed, and an orphan is harmless.  What the floor rules out is the
    # audit inspecting an empty or truncated set and reporting a pass.
    if len(objects) < expected_count:
        die(
            "BOOT-OBJ-COUNT",
            f"{object_dir} holds {len(objects)} object(s) but boot_image has "
            f"{expected_count} translation unit(s) -- the LTO audit would be "
            "inspecting a subset",
        )
    for obj in sorted(objects):
        names = elf_section_names(obj)
        if names is None:
            gate.fail(
                "BOOT-OBJ-LTO",
                f"{obj} is not an ELF object.  GCC emits LTO objects that are not "
                "ordinary ELF at all, so this cannot be waved through.",
            )
            continue
        lto = [n for n in names if n.startswith(LTO_SECTION_PREFIXES)]
        if lto:
            gate.fail(
                "BOOT-OBJ-LTO",
                f"{obj} carries LTO intermediate representation ({lto[0]}).  Link-time "
                "optimisation may inline the HAL flash writers into iflash.c across "
                "translation units, which deletes the call-graph edges C4 checks.",
            )
    gate.note(f"objects: {len(objects)} bootloader object(s), no LTO IR")


def audit_compile_commands(gate, cc_path, object_dir, tu_list_path):
    try:
        with open(cc_path, "r", encoding="utf-8") as handle:
            database = json.load(handle)
    except OSError as exc:
        die(
            "BOOT-CC-MISSING",
            f"cannot read {cc_path}: {exc}.  boot_image sets EXPORT_COMPILE_COMMANDS, "
            "so this file is expected to exist; without it the LTO audit cannot run.",
        )
    except ValueError as exc:
        die("BOOT-CC-MISSING", f"{cc_path} is not valid JSON: {exc}")
    if not isinstance(database, list):
        die("BOOT-CC-MISSING", f"{cc_path} is not a JSON array")

    object_marker = ("CMakeFiles", object_dir)
    # A bootloader entry CANNOT avoid mentioning the object directory -- that is
    # where its -o points -- so selecting candidates on the raw string first and
    # confirming on path elements afterwards cannot miss one.  Entries belonging
    # to other targets are not this gate's business and are left alone.
    seen_sources = {}
    for index, entry in enumerate(database):
        if not isinstance(entry, dict):
            continue
        raw = entry.get("command") or " ".join(entry.get("arguments") or [])
        if object_dir not in raw and object_dir not in str(entry.get("output", "")):
            continue

        directory = entry.get("directory")
        if not isinstance(directory, str):
            die("BOOT-CC-ENTRY", f"entry {index} has no 'directory'")

        argv = split_command(entry, index)
        for arg in argv:
            if arg.startswith("@"):
                die(
                    "BOOT-CC-RESPONSE-FILE",
                    f"entry {index} passes a response file ({arg}).  GCC expands @file "
                    "recursively into real options, so scanning this command line would "
                    "no longer see what the compiler sees.",
                )
            if arg.startswith("-specs=") or arg == "-specs":
                die(
                    "BOOT-CC-SPECS",
                    f"entry {index} passes {arg}.  A spec file rewrites the options that "
                    "reach cc1, so this command line stops being the thing that decides. "
                    "(The bootloader's compile commands carry none today; -specs belongs "
                    "on the LINK line.)",
                )

        outputs = extract_outputs(argv)
        if outputs is None:
            die("BOOT-CC-OUTPUT", f"entry {index} ends with a dangling -o")
        if len(outputs) != 1:
            die(
                "BOOT-CC-OUTPUT",
                f"entry {index} specifies {len(outputs)} outputs; exactly one is "
                "required to identify the object",
            )
        out_path = normalise(directory, outputs[0])

        # The `output` key is a CMake 3.26 addition and this repository requires
        # 3.20, so -o is the primary source of truth.  When the key IS there it
        # must agree -- a disagreement means the parse is wrong, not that one of
        # them wins.
        declared = entry.get("output")
        if isinstance(declared, str) and normalise(directory, declared) != out_path:
            die(
                "BOOT-CC-OUTPUT-MISMATCH",
                f"entry {index}: -o says {out_path}, 'output' says "
                f"{normalise(directory, declared)}",
            )

        if not has_path_elements(out_path, object_marker):
            die(
                "BOOT-CC-OUTPUT",
                f"entry {index} mentions {object_dir} but its output {out_path} is not "
                f"under CMakeFiles/{object_dir} -- cannot tell whose object this is",
            )

        source = entry.get("file")
        if not isinstance(source, str):
            die("BOOT-CC-ENTRY", f"entry {index} has no 'file'")
        source = normalise(directory, source)
        if source in seen_sources:
            die(
                "BOOT-CC-TU-SET",
                f"{source} is compiled twice into {object_dir} -- ambiguous audit",
            )
        seen_sources[source] = argv

        for arg in argv:
            if arg == "-flto" or arg.startswith("-flto="):
                gate.fail(
                    "BOOT-CC-LTO",
                    f"{source} is compiled with {arg}.  Link-time optimisation may "
                    "inline the HAL flash writers into iflash.c across translation "
                    "units, which deletes the very call-graph edges C4 checks.",
                )

    expected = {
        os.path.normpath(unit) for unit in read_expected_translation_units(tu_list_path)
    }
    got = set(seen_sources)
    missing = sorted(expected - got)
    extra = sorted(got - expected)
    if missing:
        die(
            "BOOT-CC-TU-SET",
            f"{len(missing)} bootloader translation unit(s) have no entry in {cc_path} "
            f"(first: {missing[0]}) -- the audit would be inspecting a subset and "
            "reporting a pass",
        )
    if extra:
        die(
            "BOOT-CC-TU-SET",
            f"{cc_path} has {len(extra)} entry/entries under {object_dir} that are not "
            f"bootloader sources (first: {extra[0]})",
        )
    gate.note(f"compile commands: {len(got)} bootloader TU(s) audited, no LTO")


# ===========================================================================
#  C1 / C2 -- where the image starts and how big it is
# ===========================================================================
def read_sections(objdump, elf):
    """[(name, size, vma, lma)] from `objdump -h`."""
    sections = []
    # The first thing this gate touches, so a file that is not an ELF at all is
    # reported as bad INPUT rather than as a broken toolchain.
    text = run([objdump, "-h", elf], ident="BOOT-INPUT-NOT-ELF")
    for match in re.finditer(
        r"^\s*\d+\s+(\S+)\s+([0-9a-f]+)\s+([0-9a-f]+)\s+([0-9a-f]+)\s+",
        text,
        re.MULTILINE,
    ):
        sections.append(
            (
                match.group(1),
                int(match.group(2), 16),
                int(match.group(3), 16),
                int(match.group(4), 16),
            )
        )
    if not sections:
        die("BOOT-INPUT-NOT-ELF", f"{objdump} reported no sections for {elf}")
    return sections


def read_load_segments(objdump, elf):
    """[(vaddr, paddr, filesz, memsz)] for the PT_LOAD program headers.

    `objdump -p` prints each one over two lines:
        LOAD off 0x... vaddr 0x... paddr 0x... align 2**12
             filesz 0x... memsz 0x... flags rwx
    """
    segments = []
    text = run([objdump, "-p", elf])
    for match in re.finditer(
        r"^\s*LOAD\s+off\s+0x([0-9a-f]+)\s+vaddr\s+0x([0-9a-f]+)\s+paddr\s+0x([0-9a-f]+)"
        r"[^\n]*\n\s*filesz\s+0x([0-9a-f]+)\s+memsz\s+0x([0-9a-f]+)",
        text,
        re.MULTILINE,
    ):
        segments.append(
            (
                int(match.group(2), 16),
                int(match.group(3), 16),
                int(match.group(4), 16),
                int(match.group(5), 16),
            )
        )
    if not segments:
        die("BOOT-INPUT-NOT-ELF", f"{objdump} -p reported no LOAD segments for {elf}")
    return segments


def read_symbols(nm, elf):
    """[(address, type, name)] for every symbol nm reports with an address."""
    syms = []
    for line in run([nm, elf]).splitlines():
        parts = line.split(maxsplit=2)
        if len(parts) != 3 or not re.fullmatch(r"[0-9a-fA-F]+", parts[0]):
            continue
        syms.append((int(parts[0], 16), parts[1], parts[2]))
    if not syms:
        die("BOOT-INPUT-NOT-ELF", f"{nm} reported no symbols for {elf} -- stripped?")
    return syms


def read_vectors(objdump, elf):
    """.isr_vector as 32-bit little-endian words.

    objdump -s prints `<addr> <w0> <w1> <w2> <w3>  <ascii>` with TWO spaces before
    the ASCII gutter.  Splitting there first keeps an ASCII column that happens to
    read as hex out of the vector table.  (Same reader as check_f746_layout.py.)
    """
    text = run([objdump, "-s", "-j", ".isr_vector", elf])
    words = []
    for line in text.splitlines():
        match = re.match(r"\s*([0-9a-fA-F]+)\s(.*)$", line)
        if not match:
            continue
        groups = match.group(2).split("  ", 1)[0].split()
        if not groups or not all(re.fullmatch(r"[0-9a-fA-F]{2,8}", g) for g in groups):
            continue
        for group in groups:
            if len(group) == 8:
                words.append(int.from_bytes(bytes.fromhex(group), "little"))
    if not words:
        die("BOOT-C1-VECTOR", f"{objdump} found no .isr_vector in {elf}")
    return words


def unique_address(syms, name, bindings=None):
    """The address of @name, or None.  Ambiguity is a failure, not a coin toss."""
    hits = {
        addr
        for addr, typ, sym in syms
        if strip_clone_suffixes(sym) == name and (bindings is None or typ in bindings)
    }
    if len(hits) > 1:
        die("BOOT-SYMBOL", f"{name} resolves to {len(hits)} addresses -- cannot check")
    return hits.pop() if hits else None


def check_vectors_and_placement(gate, objdump, nm, elf, bin_size):
    sections = read_sections(objdump, elf)
    by_name = {name: (size, vma, lma) for name, size, vma, lma in sections}

    if ".isr_vector" not in by_name:
        gate.fail("BOOT-C1-VECTOR-VMA", f"{elf} has no .isr_vector section")
        return
    _, vma, lma = by_name[".isr_vector"]
    if vma != FLASH_BASE:
        gate.fail(
            "BOOT-C1-VECTOR-VMA",
            f".isr_vector VMA is 0x{vma:08x}, must be 0x{FLASH_BASE:08x} "
            "(this is the bootloader, not an app image)",
        )
    if lma != FLASH_BASE:
        gate.fail(
            "BOOT-C1-VECTOR-LMA",
            f".isr_vector LMA is 0x{lma:08x}, must be 0x{FLASH_BASE:08x}",
        )

    segments = read_load_segments(objdump, elf)
    lowest = min(paddr for _, paddr, _, _ in segments)
    if lowest != FLASH_BASE:
        gate.fail(
            "BOOT-C1-LOAD-BASE",
            f"the lowest LOAD segment loads at 0x{lowest:08x}, not 0x{FLASH_BASE:08x} "
            "-- something is placed in front of the vector table",
        )

    # C2.  Not redundant with the linker script: a section whose LMA landed in RAM
    # keeps the link happy and makes `objcopy -O binary` emit a file hundreds of
    # megabytes long, padded from 0x08000000 to the RAM address.
    window_end = FLASH_BASE + SECTOR0_SIZE
    for vaddr, paddr, filesz, _ in segments:
        if filesz == 0:
            continue        # nothing is written for a .bss-only segment
        if paddr < FLASH_BASE or paddr + filesz > window_end:
            gate.fail(
                "BOOT-C2-LOAD-WINDOW",
                f"LOAD segment (vaddr 0x{vaddr:08x}) occupies "
                f"[0x{paddr:08x}, 0x{paddr + filesz:08x}) which leaves sector 0 "
                f"[0x{FLASH_BASE:08x}, 0x{window_end:08x})",
            )
    if bin_size > SECTOR0_SIZE:
        gate.fail(
            "BOOT-C2-SIZE",
            f"boot.bin is {bin_size} B, over the {SECTOR0_SIZE} B sector-0 budget",
        )
    else:
        gate.note(
            f"C2: {bin_size} B of {SECTOR0_SIZE} B sector-0 budget "
            f"({100.0 * bin_size / SECTOR0_SIZE:.1f}%), "
            f"{SECTOR0_SIZE - bin_size} B spare"
        )

    vectors = read_vectors(objdump, elf)
    if len(vectors) < 2:
        die("BOOT-C1-VECTOR", f"{elf} has a {len(vectors)}-word .isr_vector")
    syms = read_symbols(nm, elf)

    # vector[0] is the initial MSP: the Cortex-M7 loads it from the first word at
    # reset, before a single instruction runs (PM0253 sec 2.4.4).
    msp = vectors[0]
    if msp % 8 != 0:
        gate.fail(
            "BOOT-C1-MSP-ALIGN",
            f"initial MSP 0x{msp:08x} is not 8-byte aligned (AAPCS stack alignment)",
        )
    # start < msp <= end.  The top is INCLUSIVE: a full-descending stack starts one
    # word above its highest slot, and 0x24050000 -- the measured value -- is
    # exactly the top of AXI-SRAM.  The bottom is EXCLUSIVE for the mirror-image
    # reason: the M7 decrements SP and then writes, so an SP at the base of a RAM
    # leaves it on the very first push (PM0253 sec 2.1.2).
    in_ram = any(
        base < msp <= base + length for _, base, length in RAM_WINDOWS
    )
    if not in_ram:
        windows = ", ".join(
            f"{name} (0x{base:08x}, 0x{base + length:08x}]"
            for name, base, length in RAM_WINDOWS
        )
        gate.fail(
            "BOOT-C1-MSP-RANGE",
            f"initial MSP 0x{msp:08x} is not inside any internal RAM: {windows}",
        )

    reset = vectors[1]
    if not FLASH_BASE <= (reset & ~1) < APP_BASE:
        gate.fail(
            "BOOT-C1-RESET-RANGE",
            f"reset vector 0x{reset:08x} is outside sector 0 "
            f"[0x{FLASH_BASE:08x}, 0x{APP_BASE:08x})",
        )
    if not reset & 1:
        gate.fail(
            "BOOT-C1-RESET-THUMB",
            f"reset vector 0x{reset:08x} has bit 0 clear; the M7 would fault on an "
            "ARM-state entry",
        )
    reset_sym = unique_address(syms, "Reset_Handler")
    if reset_sym is None:
        gate.fail("BOOT-C1-RESET-SYM", "the image defines no Reset_Handler")
    elif (reset & ~1) != (reset_sym & ~1):
        # Masked on BOTH sides: nm reports the even function address (0x08000f10)
        # while the vector slot carries the Thumb bit (0x08000f11).  Comparing raw
        # would reject every correct image.
        gate.fail(
            "BOOT-C1-RESET-SYM",
            f"reset vector 0x{reset:08x} does not point at Reset_Handler "
            f"(0x{reset_sym:08x})",
        )
    gate.note(f"C1: MSP 0x{msp:08x}, reset 0x{reset:08x} -> Reset_Handler")
    return vectors, syms


# ===========================================================================
#  The ELF <-> bin join
# ===========================================================================
def check_elf_bin_pair(gate, objcopy, elf, bin_path):
    """Regenerate the .bin from the .elf and compare, byte for byte.

    Without this, C1-C5 (which read the ELF) and C6b (which reads the .bin) never
    meet: with -DBOOT_ALLOW_IMAGE_DRIFT=ON one could keep a healthy ELF, swap in a
    different .bin, and pass everything -- C6b being the only check the override
    excuses.  Runs under the override too, for exactly that reason.

    The regeneration uses the same objcopy AND the same argv (-O binary -S) as the
    build step; a different argv would compare two different transformations.  The
    temporary lands next to the real artefact, because a build must not write
    outside build/.
    """
    scratch = f"{bin_path}.regen.{os.getpid()}.tmp"
    try:
        try:
            subprocess.run(
                [objcopy, "-O", "binary", "-S", elf, scratch],
                capture_output=True,
                text=True,
                check=True,
            )
        except (OSError, subprocess.CalledProcessError) as exc:
            die("BOOT-BIN-OBJCOPY", f"cannot regenerate the binary from {elf}: {exc}")
        try:
            with open(scratch, "rb") as handle:
                regenerated = handle.read()
            with open(bin_path, "rb") as handle:
                delivered = handle.read()
        except OSError as exc:
            die("BOOT-BIN-OBJCOPY", f"cannot read back the regenerated binary: {exc}")
    finally:
        try:
            os.unlink(scratch)
        except OSError:
            pass

    if regenerated != delivered:
        first = next(
            (
                i
                for i in range(min(len(regenerated), len(delivered)))
                if regenerated[i] != delivered[i]
            ),
            min(len(regenerated), len(delivered)),
        )
        gate.fail(
            "BOOT-BIN-MISMATCH",
            f"{bin_path} ({len(delivered)} B) is not what {elf} produces "
            f"({len(regenerated)} B); first difference at offset 0x{first:x}.  The two "
            "halves of this gate would otherwise be checking different images.",
        )
        return delivered
    gate.note(f"ELF<->bin: {len(delivered)} B, regenerated from the ELF and identical")
    return delivered


# ===========================================================================
#  C3 -- no option-byte / RDP / DBGMCU / low-power path
# ===========================================================================
def check_flash_irq_handler(gate, vectors, syms):
    """FLASH_IRQHandler is allowed ONLY as the untouched weak default.

    Allowing it by name would fail open the day somebody adds a real handler that
    pokes the flash registers directly.  Three conditions, all required:
      1. it is WEAK,
      2. its address is Default_Handler's,
      3. the vector slot for FLASH_IRQn carries that same address.
    Condition 3 needs the SLOT, not just "an entry with this value": 144 slots hold
    Default_Handler, so finding the value proves nothing about this interrupt.
    """
    # RM0468 / lib/cmsis_device_h7/Include/stm32h725xx.h: FLASH_IRQn = 4, and the
    # M7 vector table puts external interrupt n at index 16 + n.
    flash_slot = 16 + 4

    weak = {
        addr
        for addr, typ, sym in syms
        if strip_clone_suffixes(sym) == "FLASH_IRQHandler" and typ in ("W", "w", "V", "v")
    }
    any_binding = {
        addr for addr, _, sym in syms if strip_clone_suffixes(sym) == "FLASH_IRQHandler"
    }
    if not any_binding:
        return          # not defined at all: nothing to allow
    if len(any_binding) > 1:
        die(
            "BOOT-C3-IRQ",
            f"FLASH_IRQHandler resolves to {len(any_binding)} different addresses "
            "-- picking one would decide the check by coin toss",
        )
    if not weak:
        gate.fail(
            "BOOT-C3-IRQ-WEAK",
            "FLASH_IRQHandler is defined with strong binding.  The only accepted form "
            "is the startup file's weak alias of Default_Handler.",
        )
    default = unique_address(syms, "Default_Handler")
    if default is None:
        die("BOOT-C3-IRQ", "the image defines no Default_Handler -- cannot check")
    handler = sorted(any_binding)[0]
    if (handler & ~1) != (default & ~1):
        gate.fail(
            "BOOT-C3-IRQ-ADDR",
            f"FLASH_IRQHandler (0x{handler:08x}) is not Default_Handler "
            f"(0x{default:08x}) -- a real flash interrupt handler has appeared",
        )
    if len(vectors) <= flash_slot:
        die("BOOT-C3-IRQ", f".isr_vector has no slot {flash_slot} (FLASH_IRQn)")
    slot = vectors[flash_slot]
    if (slot & ~1) != (default & ~1):
        gate.fail(
            "BOOT-C3-IRQ-SLOT",
            f".isr_vector[{flash_slot}] (FLASH_IRQn) is 0x{slot:08x}, not "
            f"Default_Handler 0x{default:08x}",
        )


def check_forbidden_symbols(gate, syms):
    defined = {
        strip_clone_suffixes(sym) for _, typ, sym in syms if typ not in ("U", "w", "v")
    }
    for name in sorted(defined):
        if FLASH_SYMBOL_RE.match(name) and name not in FLASH_SYMBOL_ALLOWLIST:
            gate.fail(
                "BOOT-C3-FLASH-SYM",
                f"{name} is not in the flash-driver allowlist.  The bootloader may use "
                f"only {', '.join(sorted(FLASH_SYMBOL_ALLOWLIST))}; anything else -- an "
                "option-byte programmer above all -- needs a reviewed exception.",
            )
    for _, _, sym in syms:
        name = strip_clone_suffixes(sym)
        if DBGMCU_SYMBOL_RE.search(name):
            gate.fail(
                "BOOT-C3-DBGMCU-SYM",
                f"{name} touches DBGMCU; the debug configuration is off limits "
                "(AGENTS.md invariant 6)",
            )
        if LOWPOWER_SYMBOL_RE.match(name):
            gate.fail(
                "BOOT-C3-LOWPOWER-SYM",
                f"{name} enters a low-power mode, which can drop the SWD connection "
                "to the one board that is left",
            )


def scan_raw_constant(image, value):
    """Every byte offset at which @value appears as a little-endian word.

    Every offset, not every 4-aligned one: a packed struct or an align-1 section
    can put the word anywhere, and at 128 KB the exhaustive scan costs nothing.
    """
    needle = struct.pack("<I", value)
    hits = []
    start = image.find(needle)
    while start != -1:
        hits.append(start)
        start = image.find(needle, start + 1)
    return hits


MOVW_RE = re.compile(r"\bmovw\s+(\w+),\s*#(\d+)")
MOVT_RE = re.compile(r"\bmovt\s+(\w+),\s*#(\d+)")


def scan_movw_movt(disassembly_functions):
    """{value: [(function, register)]} for every movw/movt pair that builds a word.

    A constant an instruction pair assembles into a register never appears in the
    image as a word, so the raw scan above cannot see it.  Pairing is per function
    and per register, which is what the compiler actually emits.
    """
    built = {}
    for name, lines in disassembly_functions.items():
        pending = {}
        for line in lines:
            movw = MOVW_RE.search(line)
            if movw:
                pending[movw.group(1)] = int(movw.group(2))
                continue
            movt = MOVT_RE.search(line)
            if movt:
                low = pending.get(movt.group(1))
                if low is not None:
                    value = (int(movt.group(2)) << 16) | low
                    built.setdefault(value, []).append((name, movt.group(1)))
    return built


def check_forbidden_constants(gate, image, built):
    """The two ways a forbidden constant can reach a register.

    Neither is exhaustive, and that is the honest position: a value can also be
    computed (a pool-loaded half plus a movt, an add, a table lookup), and no
    static scan can enumerate those.  This is the defence-in-depth layer named in
    the module docstring; the guarantees live in C6a, C6b and C4.  What it does
    buy is that the two OBVIOUS spellings -- the literal in a pool, and the
    movw/movt pair a compiler emits for a 32-bit immediate -- cannot pass.
    """
    for ident, label, value in FORBIDDEN_CONSTANTS:
        hits = scan_raw_constant(image, value)
        if hits:
            gate.fail(
                f"{ident}-RAW",
                f"{label} (0x{value:08x}) appears as a literal word at offset(s) "
                f"{', '.join(hex(h) for h in hits[:8])} -- this image can unlock what "
                "it must never write",
            )
        if value in built:
            where = ", ".join(f"{fn} ({reg})" for fn, reg in built[value][:8])
            gate.fail(
                f"{ident}-MOVW",
                f"{label} (0x{value:08x}) is assembled by a movw/movt pair in {where}",
            )


# ===========================================================================
#  C4 -- the flash-writing call graph
# ===========================================================================
FUNC_HEADER_RE = re.compile(r"^([0-9a-f]+) <([^>]+)>:$")
REFERENCE_RE = re.compile(r"<([^>+]+)(?:\+0x[0-9a-f]+)?>")


def read_disassembly(objdump, elf):
    """({function: [lines]}, {function: address}), comments already removed.

    Comments are stripped FIRST: objdump annotates a PC-relative load as
    `ldr r0, [pc, #12]  @ (8000390 <deregister_tm_clones+0x10>)`, and that <name>
    is the address of the literal pool word, not a callee.  Keeping it would
    invent edges that do not exist.
    """
    functions = {}
    addresses = {}
    current = None
    for line in run([objdump, "-d", elf]).splitlines():
        header = FUNC_HEADER_RE.match(line)
        if header:
            current = canonical_symbol(header.group(2))
            functions.setdefault(current, [])
            addresses.setdefault(current, int(header.group(1), 16))
            continue
        if current is None:
            continue
        functions[current].append(line.split(" @ ", 1)[0])
    if not functions:
        die("BOOT-C4-DISASM", f"{objdump} -d produced no functions for {elf}")
    return functions, addresses


def build_call_graph(functions):
    """{caller: {callee}} from every <name> left after comment stripping.

    No opcode allowlist.  `bl` is not the only way to reach a function: a tail call
    is `b.w`, an indirect call is `blx` on a register loaded from a pool, and the
    next compiler release is free to invent another.  A list of opcodes is a guard
    that fails open in whatever shape nobody thought of, so every surviving symbol
    reference counts as an edge.
    """
    graph = {}
    for caller, lines in functions.items():
        callees = set()
        for line in lines:
            for match in REFERENCE_RE.finditer(line):
                callee = canonical_symbol(match.group(1))
                if callee != caller:
                    callees.add(callee)
        graph[caller] = callees
    return graph


def check_call_graph(gate, image, functions, addresses, built):
    graph = build_call_graph(functions)

    for caller, callee in REQUIRED_EDGES:
        if callee not in graph.get(caller, ()):
            # Absence is NOT proof of safety.  "The call was inlined away" and "the
            # call is gone" look identical here, so this stops the build instead of
            # quietly reporting a pass on an image it can no longer analyse.
            die(
                "BOOT-C4-MISSING-EDGE",
                f"{caller} -> {callee} is not in the image.  Either the flash path "
                "changed or it was inlined across translation units (LTO); in both "
                "cases the call-graph check no longer means anything.",
            )

    for api in FLASH_WRITE_APIS:
        callers = sorted(c for c, callees in graph.items() if api in callees)
        for caller in callers:
            if caller not in ALLOWED_FLASH_CALLERS:
                gate.fail(
                    "BOOT-C4-EXTRA-CALLER",
                    f"{caller} calls {api}.  Only "
                    f"{', '.join(sorted(ALLOWED_FLASH_CALLERS))} may reach the flash "
                    "writers -- they are what keeps sector 0 out of range.",
                )
        if callers:
            gate.note(f"C4: {api} <- {', '.join(callers)}")

    # An indirect route to a flash writer: its ADDRESS materialised somewhere,
    # rather than a direct branch the graph above can see.  Finding one does NOT
    # identify a caller -- neither a data word nor a register says who will use
    # it -- so this cannot be attributed and cannot be reported as a proven
    # violation.  What it means is that the call graph is no longer the whole
    # story, which is a reason to stop and ask a human, not to pass.
    #
    # BOTH ways of materialising it have to be covered, and the second one is why
    # this is not just a data scan.  A rogue caller can do
    #
    #     movw r3, #:lower16:HAL_FLASH_Program
    #     movt r3, #:upper16:HAL_FLASH_Program
    #     bx   r3
    #
    # which leaves NO <symbol> in the disassembly for the edge extractor and NO
    # contiguous pointer word for the raw scan -- the address only ever exists
    # split across two instruction encodings.  Measured on the golden image: zero
    # hits either way.
    for api in FLASH_WRITE_APIS:
        address = addresses.get(api)
        if address is None:
            continue
        for candidate in (address & ~1, address | 1):
            hits = scan_raw_constant(image, candidate)
            if hits:
                die(
                    "BOOT-C4-FNPTR",
                    f"the address of {api} (0x{candidate:08x}) occurs as a data word at "
                    f"offset(s) {', '.join(hex(h) for h in hits[:8])}.  Whoever loads it "
                    "cannot be identified from the image, so the caller allowlist above "
                    "can no longer be trusted.",
                )
            if candidate in built:
                where = ", ".join(f"{fn} ({reg})" for fn, reg in built[candidate][:8])
                die(
                    "BOOT-C4-FNPTR",
                    f"the address of {api} (0x{candidate:08x}) is assembled by a "
                    f"movw/movt pair in {where}.  That is an indirect call site the "
                    "caller allowlist cannot see.",
                )


# ===========================================================================
#  C5 / C6b
# ===========================================================================
def check_dfu_class(gate, syms):
    if unique_address(syms, REQUIRED_DFU_SYMBOL) is None:
        gate.fail(
            "BOOT-C5-NO-DFU",
            f"{REQUIRED_DFU_SYMBOL} is not in the image: this bootloader has no DFU "
            "class.  The usual cause is TinyUSB resolving #include \"tusb_config.h\" to "
            "the APP's copy (CFG_TUD_DFU=0) because the -I order changed.  It compiles, "
            "it links, and it cannot be updated over USB.",
        )


def check_golden_image(gate, image, expected_sha, expected_size, allow_drift):
    got = hashlib.sha256(image).hexdigest()
    if got == expected_sha and len(image) == expected_size:
        gate.note(f"C6b: image matches the golden hash ({got[:16]}..., {len(image)} B)")
        return
    detail = (
        f"boot.bin is sha256 {got} ({len(image)} B); expected {expected_sha} "
        f"({expected_size} B)"
    )
    if allow_drift:
        print(
            f"{TOOL}: WARNING: BOOT-C6B-DRIFT: {detail}.\n"
            f"{TOOL}: WARNING: BOOT_ALLOW_IMAGE_DRIFT=ON -- this bootloader image has "
            "never run on hardware, and the setting is cached, so it will stay on until "
            "somebody turns it off.  Update the golden hash or clear the flag.",
            file=sys.stderr,
        )
        return
    gate.fail(
        "BOOT-C6B-HASH" if len(image) == expected_size else "BOOT-C6B-SIZE",
        f"{detail}.  This is the reproducibility baseline for the bootloader that is "
        "actually on the board; a HAL, TinyUSB or toolchain bump is the usual cause. "
        "Re-freeze it deliberately (and record why) or build with "
        "-DBOOT_ALLOW_IMAGE_DRIFT=ON.",
    )


# ===========================================================================
#  Entry points
# ===========================================================================
def mode_precheck(args):
    gate = Gate()
    board_dir = os.path.abspath(args.board_dir)
    boot_dir = os.path.abspath(args.boot_dir)
    check_source_manifest(gate, board_dir, boot_dir, args.manifest)
    audit_compile_commands(
        gate, args.compile_commands, args.object_dir, args.translation_units
    )
    if args.allow_image_drift:
        print(
            f"{TOOL}: WARNING: BOOT-C6B-DRIFT: BOOT_ALLOW_IMAGE_DRIFT=ON -- the golden "
            "image hash is not enforced in this build tree.",
            file=sys.stderr,
        )
    status = gate.finish("bootloader sources and compile commands unchanged")
    if status == 0:
        # The stamp is what boot_image's LINK_DEPENDS points at, so touching it
        # here is what forces a relink -- and therefore what guarantees POST_BUILD
        # runs.  It is deliberately NOT passed to the linker: adding it to the link
        # inputs would change the image.
        try:
            with open(args.stamp, "w", encoding="utf-8") as handle:
                handle.write(
                    "check_boot_safety precheck passed; boot_image relinks because "
                    "this file is newer than it.\n"
                )
        except OSError as exc:
            die("BOOT-STAMP", f"cannot write {args.stamp}: {exc}")
    return status


def mode_postlink(args):
    gate = Gate()
    if not args.bin:
        die("BOOT-INPUT-NO-BIN", "--bin is required in postlink mode")
    try:
        bin_size = os.path.getsize(args.bin)
    except OSError as exc:
        die("BOOT-INPUT-NO-BIN", f"cannot stat {args.bin}: {exc}")

    result = check_vectors_and_placement(gate, args.objdump, args.nm, args.elf, bin_size)
    # If C1/C2 already proved this is not a sector-0 image, say so and stop.  Going
    # on would report "the call graph could not be analysed" (exit 2, cannot check)
    # about an image whose problem is already proven (exit 1) -- and an app image
    # handed to this gate by mistake is exactly that case.
    if result is None or gate.failures:
        return gate.finish("")
    vectors, syms = result

    image = check_elf_bin_pair(gate, args.objcopy, args.elf, args.bin)

    functions, addresses = read_disassembly(args.objdump, args.elf)
    # Built once: C3 uses it for the forbidden constants, C4 for the addresses of
    # the flash writers.  They are the same mechanism -- a 32-bit value that only
    # ever exists inside two instruction encodings -- pointed at two questions.
    built = scan_movw_movt(functions)
    check_flash_irq_handler(gate, vectors, syms)
    check_forbidden_symbols(gate, syms)
    check_forbidden_constants(gate, image, built)
    check_call_graph(gate, image, functions, addresses, built)
    check_dfu_class(gate, syms)
    check_objects_free_of_lto(
        gate,
        args.object_dir,
        len(read_expected_translation_units(args.translation_units)),
    )
    check_golden_image(
        gate, image, args.golden_sha256, args.golden_size, args.allow_image_drift
    )
    return gate.finish("sector 0 is safe from this image")


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = parser.add_subparsers(dest="mode", required=True)

    pre = sub.add_parser("precheck", help="source manifest + compile-command audit")
    pre.add_argument("--board-dir", required=True)
    pre.add_argument("--boot-dir", required=True)
    pre.add_argument("--manifest", required=True)
    pre.add_argument("--compile-commands", required=True)
    pre.add_argument("--object-dir", required=True, help="e.g. boot_image.dir")
    pre.add_argument("--translation-units", required=True)
    pre.add_argument("--stamp", required=True)
    pre.add_argument("--allow-image-drift", action="store_true")
    pre.set_defaults(func=mode_precheck)

    post = sub.add_parser("postlink", help="linked-image checks")
    post.add_argument("--elf", required=True)
    post.add_argument("--bin", required=True)
    post.add_argument("--nm", required=True)
    post.add_argument("--objdump", required=True)
    post.add_argument("--objcopy", required=True)
    post.add_argument("--object-dir", required=True,
                      help="the target's object directory, e.g. "
                           "<build>/CMakeFiles/boot_image.dir")
    post.add_argument("--translation-units", required=True)
    post.add_argument("--golden-sha256", required=True)
    post.add_argument("--golden-size", type=int, required=True)
    post.add_argument("--allow-image-drift", action="store_true")
    post.set_defaults(func=mode_postlink)

    args = parser.parse_args()
    sys.exit(args.func(args))


if __name__ == "__main__":
    main()
