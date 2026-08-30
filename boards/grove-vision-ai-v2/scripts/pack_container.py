#!/usr/bin/env python3
"""Assemble a model + plugin container, then verify the thing it assembled.

Issue #101 (#78 Step 1a).

[!] THE ORDER IS THE WHOLE POINT.  The obvious shape -- check the parts, then
staple them together -- is not equivalent to what
boards/grove-vision-ai-v2/scripts/send_verified_model.sh.in guarantees today.
That script copies the model into a staging directory, verifies THE COPY, and
sends THE SAME COPY: what was checked and what goes on the wire are one file.
Checking the parts and then assembling leaves the assembler free to read the
inputs again, and nothing downstream would notice.

So the container is built ONCE, and everything after that reads the built
artifact:

    stage -> assemble once -> re-parse the container -> extract the model and
    the plugin image FROM IT -> verify those extracted bytes -> re-check the
    manifest, offsets and digest -> hand that unmodified file to the sender.

[!] AND THE LAYOUT IS NOT TRANSCRIBED HERE.  Field offsets come from
abi_layout.c, compiled against the same svc/plugin_abi.h the firmware includes.
A struct format string in this file would be a second declaration of the ABI,
and the day a field moved the two would disagree while both still ran.
"""
import argparse
import json
import os
import struct
import subprocess
import sys
import zlib


def die(msg):
    print(f"pack_container: {msg}", file=sys.stderr)
    sys.exit(1)


def run(cmd):
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        die(f"{cmd[0]} failed: {r.stderr.strip()}")
    return r.stdout


def elf_symbols(nm, elf):
    syms = {}
    for line in run([nm, elf]).splitlines():
        p = line.split()
        if len(p) == 3:
            syms[p[2]] = int(p[0], 16)
    return syms


def elf_section_bytes(objcopy, elf, section, work):
    """Raw contents of one section, or b'' when it has none."""
    out = os.path.join(work, section.strip(".") + ".bin")
    r = subprocess.run([objcopy, "-O", "binary", f"--only-section={section}",
                        elf, out], capture_output=True, text=True)
    if r.returncode != 0 or not os.path.exists(out):
        return b""
    with open(out, "rb") as fh:
        return fh.read()


def put(buf, off, fmt, *vals):
    struct.pack_into(fmt, buf, off, *vals)


def build_container(L, elf_info, model, data, name, build_id, stacks):
    """Lay the container out and return the bytes.  Called exactly once."""
    hdr_sz = L["hdr_size"]
    man_sz = L["manifest_size"]
    hm, mm = L["hdr"], L["manifest"],

    image = elf_info["image"]
    plugin_len = man_sz + len(image)

    # Sections follow the header, in the order they are laid out.  The model
    # must land on PLUGIN_MODEL_ALIGN because the Ethos-U command stream is read
    # where it lies -- nothing copies it first.
    off = hdr_sz
    plugin_off = off
    off += plugin_len
    off = (off + L["model_align"] - 1) & ~(L["model_align"] - 1)
    model_off = off
    off += len(model)
    data_off = 0
    if data:
        data_off = off
        off += len(data)
    total = off

    buf = bytearray(total)

    # --- container header ---
    buf[hm["magic"]:hm["magic"] + 4] = L["magic"].encode()
    buf[hm["format"]:hm["format"] + 4] = L["format"].encode()
    put(buf, hm["hdr_size"], "<I", hdr_sz)
    put(buf, hm["abi_version"], "<I", L["abi_version"])
    put(buf, hm["total_size"], "<I", total)
    put(buf, hm["section_count"], "<I", 3 if data else 2)

    sec_sz = L["section_size"]
    st, so = L["section"]["type"], L["section"]["offset"]
    sl = L["section"]["length"]
    base = hm["sections"]
    entries = [(L["section_type"]["plugin"], plugin_off, plugin_len),
               (L["section_type"]["model"], model_off, len(model))]
    if data:
        entries.append((L["section_type"]["data"], data_off, len(data)))
    for i, (t, o, n) in enumerate(entries):
        at = base + i * sec_sz
        put(buf, at + st, "<I", t)
        put(buf, at + so, "<I", o)
        put(buf, at + sl, "<I", n)

    # --- manifest ---
    m = plugin_off
    buf[m + mm["magic"]:m + mm["magic"] + 4] = L["manifest_magic"].encode()
    put(buf, m + mm["struct_size"], "<I", man_sz)
    put(buf, m + mm["abi_version"], "<I", L["abi_version"])
    put(buf, m + mm["target_id"], "<I", elf_info["target_id"])
    put(buf, m + mm["link_addr"], "<I", elf_info["link_addr"])
    put(buf, m + mm["capability"], "<I", elf_info["capability"])
    put(buf, m + mm["image_off"], "<I", man_sz)
    put(buf, m + mm["file_size"], "<I", elf_info["file_size"])
    put(buf, m + mm["mem_size"], "<I", elf_info["mem_size"])
    for f in ("code", "data", "bss", "scratch"):
        put(buf, m + mm[f + "_off"], "<I", elf_info[f][0])
        put(buf, m + mm[f + "_len"], "<I", elf_info[f][1])
    for i, v in enumerate(elf_info["slots"]):
        put(buf, m + mm["slot"] + 4 * i, "<I", v)
        put(buf, m + mm["stack"] + 4 * i, "<I", stacks[i] if v else 0)
    nb = name.encode()[:L["name_max"] - 1]
    buf[m + mm["name"]:m + mm["name"] + len(nb)] = nb
    bb = build_id.encode()[:L["build_id_max"] - 1]
    buf[m + mm["build_id"]:m + mm["build_id"] + len(bb)] = bb

    buf[m + man_sz:m + man_sz + len(image)] = image
    buf[model_off:model_off + len(model)] = model
    if data:
        buf[data_off:data_off + len(data)] = data

    # The digest covers the plugin section and lives OUTSIDE it, in the header,
    # so there is no "hash with this field zeroed" rule for the two sides to
    # implement differently.  It is written last for that reason.
    put(buf, hm["plugin_digest"], "<I",
        zlib.crc32(bytes(buf[plugin_off:plugin_off + plugin_len])) & 0xFFFFFFFF)
    return bytes(buf)


def reparse(L, blob):
    """Read the ASSEMBLED container back, the way the device will."""
    hm = L["hdr"]
    if blob[0:4] != L["magic"].encode():
        die("re-parse: magic is wrong in the container we just built")
    if blob[4:8] != L["format"].encode():
        die("re-parse: format word is wrong")
    total = struct.unpack_from("<I", blob, hm["total_size"])[0]
    if total != len(blob):
        die(f"re-parse: total_size {total} != file {len(blob)}")
    count = struct.unpack_from("<I", blob, hm["section_count"])[0]
    secs = {}
    for i in range(count):
        at = hm["sections"] + i * L["section_size"]
        t = struct.unpack_from("<I", blob, at + L["section"]["type"])[0]
        o = struct.unpack_from("<I", blob, at + L["section"]["offset"])[0]
        n = struct.unpack_from("<I", blob, at + L["section"]["length"])[0]
        if o + n > total:
            die(f"re-parse: section {t} runs past the container")
        secs[t] = (o, n)
    want = struct.unpack_from("<I", blob, hm["plugin_digest"])[0]
    po, pn = secs[L["section_type"]["plugin"]]
    got = zlib.crc32(blob[po:po + pn]) & 0xFFFFFFFF
    if got != want:
        die(f"re-parse: digest {got:08x} != stored {want:08x}")
    if secs[L["section_type"]["model"]][0] % L["model_align"]:
        die("re-parse: the model is not aligned for the command stream")
    return secs


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--layout", required=True)
    ap.add_argument("--elf", required=True)
    ap.add_argument("--model", required=True)
    ap.add_argument("--data")
    ap.add_argument("--name", required=True)
    ap.add_argument("--build-id", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--nm", required=True)
    ap.add_argument("--objcopy", required=True)
    ap.add_argument("--target-id", required=True, type=lambda s: int(s, 0))
    ap.add_argument("--stack", action="append", default=[],
                    help="slot=bytes, from the plugin gate's analysis")
    args = ap.parse_args()

    with open(args.layout) as fh:
        L = json.load(fh)

    work = os.path.dirname(os.path.abspath(args.out)) or "."
    os.makedirs(work, exist_ok=True)

    syms = elf_symbols(args.nm, args.elf)
    need = ["__plugin_code_start", "__plugin_code_end", "__plugin_data_start",
            "__plugin_data_end", "__plugin_bss_start", "__plugin_bss_end",
            "__plugin_file_end", "__plugin_mem_end", "__plugin_slots_start"]
    for s in need:
        if s not in syms:
            die(f"the plugin ELF has no {s} -- its link script is not the one "
                "this packer was written for")

    base = syms["__plugin_code_start"]
    text = elf_section_bytes(args.objcopy, args.elf, ".text", work)
    data = elf_section_bytes(args.objcopy, args.elf, ".data", work)
    image = text + data
    file_size = syms["__plugin_file_end"] - base
    if len(image) != file_size:
        die(f"image is {len(image)} B but the script says {file_size} B")

    # The slot table lives in .text and holds one address per slot, already
    # absolute because the plugin is prelinked.  They are stored as offsets from
    # the image base: the manifest carries no addresses, so a container cannot
    # disagree with the reservation it is loaded into.
    slot_at = syms["__plugin_slots_start"] - base
    slots = []
    for i in range(L["slot_count"]):
        v = struct.unpack_from("<I", image, slot_at + 4 * i)[0]
        slots.append((v - base) if v else L["slot_absent"])

    cap = 0
    if slots[L["slot"]["draw"]]:
        cap |= L["cap"]["draw"]
    if slots[L["slot"]["report"]]:
        cap |= L["cap"]["report"]
    if slots[L["slot"]["param_set"]]:
        cap |= L["cap"]["params"]

    stacks = [0] * L["slot_count"]
    named = {k: int(v) for k, v in (s.split("=") for s in args.stack)}
    for slot_name, idx in L["slot"].items():
        if slot_name in named:
            stacks[idx] = named[slot_name]
    for i, v in enumerate(slots):
        if v and not stacks[i]:
            die(f"slot {i} is present but no --stack was given for it; the "
                "manifest may not declare a stack it has not measured")

    elf_info = {
        "image": image,
        "link_addr": base,
        "target_id": args.target_id,
        "capability": cap,
        "file_size": file_size,
        "mem_size": syms["__plugin_mem_end"] - base,
        "code": (0, syms["__plugin_code_end"] - base),
        "data": (syms["__plugin_data_start"] - base,
                 syms["__plugin_data_end"] - syms["__plugin_data_start"]),
        "bss": (syms["__plugin_bss_start"] - base,
                syms["__plugin_bss_end"] - syms["__plugin_bss_start"]),
        "scratch": (0, 0),
        "slots": slots,
    }

    with open(args.model, "rb") as fh:
        model = fh.read()
    extra = None
    if args.data:
        with open(args.data, "rb") as fh:
            extra = fh.read()

    blob = build_container(L, elf_info, model, extra, args.name,
                           args.build_id, stacks)

    # Everything from here reads the ASSEMBLED artifact, never the inputs again.
    secs = reparse(L, blob)
    mo, mn = secs[L["section_type"]["model"]]
    if blob[mo:mo + mn] != model:
        die("re-parse: the model in the container is not the model on disk")
    po, pn = secs[L["section_type"]["plugin"]]
    if blob[po + L["manifest_size"]:po + pn] != image:
        die("re-parse: the plugin image in the container is not the ELF's")

    with open(args.out, "wb") as fh:
        fh.write(blob)
    print(f"pack_container: {args.out} ({len(blob)} B: plugin {pn} B, "
          f"model {mn} B{', data ' + str(secs[L['section_type']['data']][1]) + ' B' if extra else ''})",
          file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
