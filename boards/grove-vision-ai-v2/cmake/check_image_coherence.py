#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 ThreadX Shell Project
"""Gate 1: the generated .img must carry exactly what the linker placed.

The Himax image generator (we2_local_image_gen) walks the ELF and extracts
allocatable sections into the flash image individually, recording each one --
including the intermediate per-section payload file it packed -- in
output_case1_sec_wlcsp/DEBUG_APP_PREPROCESS.json.  This gate cross-checks that
record against the ELF at three depths:

  1. every ELF section with ALLOC + CONTENTS + size > 0 appears in the JSON
     with a matching VMA and length (a skipped section would boot as
     silently-missing bytes -- e.g. an empty shell command registry);
  2. every JSON-listed payload file's BYTES equal the section content
     extracted from the current ELF (metadata alone would still accept a
     packer that recorded a section and then wrote garbage or stale data --
     the build also rm -rf's the output dir before the generator runs, so
     none of these files can predate this build);
  3. the shell command registry span (__cli_root_cmds_start/_end) is
     non-empty, 4-aligned at BOTH ends, a whole multiple of
     sizeof(struct cli_cmd) (read from the cli_cmd_sizeof_probe symbol the
     firmware exports, so it cannot drift from the struct), and lies wholly
     inside the ELF .rodata section.  A stray or truncated registry entry
     would otherwise be walked as a struct array of function pointers.

Also asserts the final shell.img exists and is non-empty.  Stdlib-only;
POST_BUILD, ordered after the image generation.
"""

import argparse
import json
import os
import re
import subprocess
import sys
import tempfile


def run(cmd):
    return subprocess.run(cmd, check=True, capture_output=True,
                          text=True).stdout


def elf_sections(objdump, elf):
    """{name: (vma, size, flags)} from `objdump -h`."""
    out = run([objdump, "-h", elf])
    secs = {}
    lines = out.splitlines()
    for i, line in enumerate(lines):
        m = re.match(r"^\s*\d+\s+(\S+)\s+([0-9a-f]{8})\s+([0-9a-f]{8})\s+"
                     r"([0-9a-f]{8})\s+([0-9a-f]{8})", line)
        if not m or i + 1 >= len(lines):
            continue
        name = m.group(1)
        size = int(m.group(2), 16)
        vma = int(m.group(3), 16)
        flags = {f.strip() for f in lines[i + 1].split(",")}
        secs[name] = (vma, size, flags)
    return secs


def nm_symbols(nm, elf):
    out = run([nm, elf])
    syms = {}
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 3:
            syms[parts[2]] = int(parts[0], 16)
    return syms


def dump_section(objcopy, elf, name, tmpdir):
    """Section content bytes extracted from the ELF."""
    out = os.path.join(tmpdir, name.replace("/", "_") + ".bin")
    subprocess.run([objcopy, "--dump-section", f"{name}={out}", elf,
                    os.path.join(tmpdir, "scratch.elf")],
                   check=True, capture_output=True)
    with open(out, "rb") as f:
        return f.read()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--objdump", required=True)
    ap.add_argument("--nm", required=True)
    ap.add_argument("--objcopy", required=True)
    ap.add_argument("--preprocess-json", required=True)
    ap.add_argument("--image-gen-dir", required=True)
    ap.add_argument("--img", required=True)
    ap.add_argument("elf")
    args = ap.parse_args()

    errors = []

    # -- what the image generator recorded --------------------------------
    with open(args.preprocess_json, encoding="utf-8") as f:
        pre = json.load(f)
    # Single top-level app key; entries are the IDXn_* dicts.
    img = {}
    for app in pre.values():
        if not isinstance(app, dict):
            continue
        for entry in app.values():
            if isinstance(entry, dict) and "sec_name" in entry:
                img[entry["sec_name"]] = (int(entry["vma"], 16),
                                          int(entry["length"], 16),
                                          entry.get("output"))

    if not img:
        print("check_image_coherence: no sections found in "
              f"{args.preprocess_json}", file=sys.stderr)
        return 1

    # -- what the linker actually placed ----------------------------------
    secs = elf_sections(args.objdump, args.elf)
    carried = {n: s for n, s in secs.items()
               if "ALLOC" in s[2] and "CONTENTS" in s[2] and s[1] > 0}
    if not carried:
        print("check_image_coherence: no allocatable content sections in the "
              "ELF?", file=sys.stderr)
        return 1

    with tempfile.TemporaryDirectory() as tmpdir:
        for name, (vma, size, _f) in sorted(carried.items()):
            if name not in img:
                errors.append(f"section {name} (vma 0x{vma:08x}, {size} B) "
                              "is MISSING from the generated image")
                continue
            ivma, ilen, ipath = img[name]
            if ivma != vma or ilen != size:
                errors.append(f"section {name}: ELF vma/size "
                              f"0x{vma:08x}/{size} vs image "
                              f"0x{ivma:08x}/{ilen}")
                continue
            # Depth 2: the payload the tool packed == the current ELF bytes.
            if not ipath:
                errors.append(f"section {name}: no payload path recorded")
                continue
            payload_file = os.path.join(args.image_gen_dir, ipath)
            if not os.path.isfile(payload_file):
                errors.append(f"section {name}: payload {ipath} missing")
                continue
            with open(payload_file, "rb") as f:
                payload = f.read()
            elf_bytes = dump_section(args.objcopy, args.elf, name, tmpdir)
            if len(elf_bytes) != size:
                errors.append(f"section {name}: objcopy dump {len(elf_bytes)}"
                              f" B != section size {size} B")
                continue
            if len(payload) < size or payload[:size] != elf_bytes:
                errors.append(f"section {name}: payload bytes differ from "
                              "the current ELF (stale or corrupted packing)")

        # -- shell command registry ---------------------------------------
        syms = nm_symbols(args.nm, args.elf)
        start = syms.get("__cli_root_cmds_start")
        end = syms.get("__cli_root_cmds_end")
        probe = syms.get("cli_cmd_sizeof_probe")
        if start is None or end is None:
            errors.append("__cli_root_cmds_start/_end not found in the ELF")
        else:
            if end <= start:
                errors.append("shell command registry is EMPTY "
                              f"(start 0x{start:08x} == end 0x{end:08x})")
            if start % 4 != 0 or end % 4 != 0:
                errors.append(f"registry bounds 0x{start:08x}/0x{end:08x} "
                              "not 4-aligned")
            ro = carried.get(".rodata")
            if ro is None:
                errors.append(".rodata not among the carried sections")
            elif not (ro[0] <= start and end <= ro[0] + ro[1]):
                errors.append(f"registry [0x{start:08x},0x{end:08x}) not "
                              f"inside .rodata "
                              f"[0x{ro[0]:08x},0x{ro[0] + ro[1]:08x})")
            # Stride: read sizeof(struct cli_cmd) out of the firmware itself.
            if probe is None:
                errors.append("cli_cmd_sizeof_probe not found in the ELF")
            elif ro is not None and ro[0] <= probe <= ro[0] + ro[1] - 4:
                ro_bytes = dump_section(args.objcopy, args.elf, ".rodata",
                                        tmpdir)
                stride = int.from_bytes(ro_bytes[probe - ro[0]:
                                                 probe - ro[0] + 4], "little")
                if not (8 <= stride <= 256):
                    errors.append(f"cli_cmd_sizeof_probe value {stride} "
                                  "implausible")
                elif (end - start) % stride != 0:
                    errors.append(f"registry span {end - start} B is not a "
                                  f"multiple of sizeof(struct cli_cmd) = "
                                  f"{stride} B (stray/truncated entry)")
            else:
                errors.append("cli_cmd_sizeof_probe not inside .rodata")

    # -- the flashable artifact itself ------------------------------------
    if not os.path.isfile(args.img) or os.path.getsize(args.img) == 0:
        errors.append(f"{args.img} missing or empty")

    if errors:
        print("check_image_coherence: FAIL", file=sys.stderr)
        for e in errors:
            print(f"  - {e}", file=sys.stderr)
        return 1

    n_cmds = (end - start)
    print(f"check_image_coherence: OK ({len(carried)} sections byte-verified, "
          f"registry {n_cmds} B in .rodata)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
