#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 ThreadX Shell Project
"""Refuse detector vocabulary in the firmware's output strings (issue #105).

WHY.  Issue #105 let a classifier plugin hold the panel, which makes every
sentence the firmware prints about "faces" and "boxes" wrong whenever the loaded
container decodes something else.  The offending lines were enumerated three
times and the enumeration was wrong twice -- one line, then three, then five, and
the fifth (the camera counters' `anchors -> boxes`) shows up on an ORDINARY classifier
stream.  Nothing in this project gates what a command PRINTS, so a fourth careful
reading would have the same shape of error and nothing would mark the sixth.

[!] IT READS THE LINKED FIRMWARE'S LOADABLE SECTIONS, NOT THE SOURCE.  The
first version scanned C string literals with a regex, and the adversarial review
of issue #105 took it apart: it covered a hand-picked set of directories while the image
links many more; `"fa" "ce"` and a line continuation and a stringifying macro all
walked past it; and a literal on a line that happened to begin with `/*` was
skipped outright.  Reading the section the compiler and linker actually produced
answers all of those at once, because by then the string IS one string and every
translation unit that reached the image has contributed.  Which sections is
derived from the ELF's flags, not listed (issue #170; see loaded_sections()).

[!] AND IT STILL DOES NOT PROVE THE FIRMWARE CANNOT SAY "face".  A string
assembled at run time out of char literals is in no section and nothing here
would see it.  That is a deliberate act, not a slip, and this check is aimed at
the slip: the ordinary edit that reintroduces the vocabulary because the person
making it was thinking about a detector.  Do not read a pass as a proof.

[!] PLUGINS ARE OUT OF SCOPE BY CONSTRUCTION, and that is the right scope.  A
container carries the code that interprets its model and the vocabulary comes
with it -- asset/plugins/blazeface saying "faces" is the entire point of issue #78, not
a defect.  Plugin images are a separate artifact and are never linked into this
ELF, so no exclusion rule is needed and none can be got wrong.
"""
import argparse
import os
import re
import struct
import subprocess
import sys
import tempfile

# Words the firmware may not use about a result it does not interpret.
#
# [!] A LIST, SO THE RUN THRESHOLD CAN BE DERIVED FROM IT.  Written as a bare
# regex with a hand-picked minimum run length, the check had a hole exactly the
# width of its shortest word: MIN_RUN was 4, so a three-character "box" was never
# extracted from the section at all and `cli_print(sh, "box")` sailed through.
# The threshold now comes from the words themselves and cannot drift behind them.
FORBIDDEN_WORDS = (
    "box", "boxes", "face", "faces", "anchor", "anchors", "bbox",
    "blazeface", "detector", "detection", "detections",
)

FORBIDDEN = re.compile(
    r"\b(" + "|".join(FORBIDDEN_WORDS) + r")\b" + r"|--name\s+det\b",
    re.IGNORECASE,
)

# Strings that are allowed, EXACTLY, each with the reason it is honest.
#
# [!] ONE ENTRY PER STRING, and matched whole.  The first version allowed a
# SUBSTRING and covered two different output lines with one entry, so either
# could vanish and the exemption would live on -- and widening the entry to a
# bare word would have exempted everything containing it.
ALLOW = [
    (
        "  face[%d]  outside the frame\r\n",
        "the caller-boxes path prints a struct bf_det, so this branch is "
        "BlazeFace BY TYPE and no classifier can reach it",
    ),
    (
        "  face[%d]  x %ld%% y %ld%% w %ld%% h %ld%%  score %ld\r\n",
        "the same branch, printing the box it just mapped",
    ),
]

MIN_RUN = min(len(w) for w in FORBIDDEN_WORDS)

# [!] WHICH SECTIONS ARE READ IS DERIVED FROM THE ELF, NOT LISTED (issue #170).
# Until then this was a fixed (".rodata", ".data", ".text"): .rodata is where
# string constants normally land, .data catches a non-const
# `static char s[] = "faces"`, .text a constant inlined into a literal pool.  But
# a fixed list is blind to the next section someone adds -- issue #170 moves cold
# code into a new SRAM section, and its literal pools would have walked past.
# The rule is now every section with SHF_ALLOC whose type is not SHT_NOBITS and
# whose size is non-zero: everything that has bytes in the loaded image, chosen
# by FLAGS, never by name.  Debug sections, .comment, .ARM.attributes (no
# SHF_ALLOC), .bss and the NOLOAD buffers (SHT_NOBITS) and the non-ALLOC
# .heap/.stack placeholders drop out by the same rule.  On the firmware as of
# issue #170 that adds .table, .ARM.exidx, .copy.table and .zero.table to the
# three, none of which holds a forbidden string.
SHF_ALLOC = 0x2
SHT_NOBITS = 8


def loaded_sections(elf):
    """(name, size) of the sections with bytes in the loaded image, in order.

    Read straight from the ELF32 section header table (stdlib only, so the gate
    needs no second tool argument).  Anything that is not a little-endian ELF32
    is refused rather than guessed at.
    """
    with open(elf, "rb") as fh:
        data = fh.read()
    if data[:4] != b"\x7fELF" or data[4] != 1 or data[5] != 1:
        sys.exit(f"check_output_vocabulary: {elf} is not a little-endian ELF32")
    shoff, = struct.unpack_from("<I", data, 0x20)
    shentsize, shnum, shstrndx = struct.unpack_from("<HHH", data, 0x2E)
    if shoff == 0 or shnum == 0 or shentsize < 40 or shstrndx >= shnum:
        sys.exit(f"check_output_vocabulary: {elf} has no usable section table")

    def header(i):
        # name, type, flags, addr, offset, size
        return struct.unpack_from("<IIIIII", data, shoff + i * shentsize)

    strtab_off = header(shstrndx)[4]

    def name_of(off):
        start = strtab_off + off
        return data[start:data.index(b"\x00", start)].decode("ascii")

    out = []
    for i in range(1, shnum):
        name, typ, flags, _addr, _off, size = header(i)
        if flags & SHF_ALLOC and typ != SHT_NOBITS and size > 0:
            out.append((name_of(name), size))
    return out


def section_strings(objcopy, elf, section, size):
    """Every printable run of MIN_RUN bytes or more in one section.

    [!] The bytes objcopy hands back must be exactly the section header's
    size.  A name objcopy does not find yields an empty (or no) file, and
    reading that as "no strings" would pass the section unread.
    """
    with tempfile.TemporaryDirectory() as work:
        raw = os.path.join(work, "section.bin")
        rc = subprocess.run(
            [objcopy, "-O", "binary", f"--only-section={section}", elf, raw],
            capture_output=True,
        )
        if rc.returncode != 0:
            sys.exit(f"check_output_vocabulary: objcopy failed: "
                     f"{rc.stderr.decode(errors='replace').strip()}")
        blob = b""
        if os.path.exists(raw):
            with open(raw, "rb") as fh:
                blob = fh.read()
    if len(blob) != size:
        sys.exit(f"check_output_vocabulary: {section}: objcopy returned "
                 f"{len(blob)} B, the section header says {size} B")

    # [!] Runs are split on NUL only, so escapes a C string carries -- \r, \n,
    # \t -- stay INSIDE the run.  Splitting on them as well would cut
    # "...frame\r\n" into pieces and an allowlist entry written the way the
    # source writes it would never match.
    out = []
    for chunk in blob.split(b"\x00"):
        run = bytearray()
        for byte in chunk:
            if 0x20 <= byte <= 0x7E or byte in (0x09, 0x0A, 0x0D):
                run.append(byte)
            else:
                if len(run) >= MIN_RUN:
                    out.append(run.decode("ascii"))
                run = bytearray()
        if len(run) >= MIN_RUN:
            out.append(run.decode("ascii"))
    return out


def show(s):
    return s.replace("\r", "\\r").replace("\n", "\\n").replace("\t", "\\t")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("elf")
    ap.add_argument("--objcopy", required=True)
    args = ap.parse_args()

    sections = loaded_sections(args.elf)
    if not sections:
        # Fail closed: an empty scan set would pass any firmware.
        sys.exit("check_output_vocabulary: no loaded section in "
                 f"{args.elf}; nothing to scan")
    found = []
    for section, size in sections:
        found.extend(section_strings(args.objcopy, args.elf, section, size))
    allowed = {text for text, _why in ALLOW}

    bad = [s for s in found if FORBIDDEN.search(s) and s not in allowed]

    # [!] An allowlist entry that matches nothing is a stale exemption, and a
    # stale exemption is a hole waiting for a coincidence.  Because the entries
    # are whole strings from the image, this really does mean "that line is
    # gone" rather than "the word appears somewhere in that file".
    stale = [text for text in allowed if text not in found]

    if bad or stale:
        print("check_output_vocabulary: FAIL", file=sys.stderr)
        for s in bad:
            print(f'  detector vocabulary in firmware output: "{show(s)}"',
                  file=sys.stderr)
        for s in stale:
            print(f'  stale allowlist entry, nothing matches: "{show(s)}"',
                  file=sys.stderr)
        if bad:
            print(
                "\n  The firmware does not know what a container's decoder "
                "produced -- that is issue #78.  Say `result` or `item` and let "
                "the decoder describe it (`nn dets` reaches the plugin's own "
                "report).  If a path really is BlazeFace BY TYPE, add the whole "
                "string to ALLOW with the reason.",
                file=sys.stderr,
            )
        return 1

    print(f"check_output_vocabulary: OK ({len(found)} string(s) in "
          f"{', '.join(n for n, _s in sections)}; {len(ALLOW)} allowed)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
