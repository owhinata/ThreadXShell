#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 ThreadX Shell Project
"""Read the plugin policy's veneer cost back out of the shipped firmware (#111).

cmake/veneer_cost_gate.cmake checks DECLARED against the stack below each veneer
and hands the same number to the firmware as -DPLUGIN_VENEER_BASE_COST.  A -D is
not a guarantee: a later -D on the command line (CMAKE_C_FLAGS,
add_compile_definitions) or a #define in a header overrides it with only a
warning, and the loader would then add a SMALLER c than the one checked -- every
container it admitted would be under-charged.  So this reads what the policy
actually HOLDS, from the linked image:

    symbol plugin_policy_probe (svc/plugin_load.h, PLUGIN_POLICY_PROBE)
      -> magic, sizeof(struct plugin_policy), the field offsets, and a pointer
      -> the policy object the board passes to plugin_parse()
      -> veneer_cost == --declared, stack_accounting == the ABI's

[!] SYMBOL VALUES AND IMAGE BYTES, NOT SOURCE TEXT.  A regex over the C would
see the macro's name, not what the compiler made of it.  And the probe carries
the offsets, so no struct layout is transcribed here.

[!] FAIL CLOSED.  No probe, two probes, a pointer into no loadable bytes, a
wrong magic or size: each is a refusal, and the stamp is not written.
"""
import argparse
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import check_plugin_image  # noqa: E402 -- the ABI's accounting, pinned there
from check_veneer_base_cost import Elf, InputError  # noqa: E402

SHT_NOBITS = 8
SHF_ALLOC = 0x2
PROBE = "plugin_policy_probe"
MAGIC = 0x4C4F5050          # svc/plugin_load.h PLUGIN_POLICY_PROBE_MAGIC
PROBE_WORDS = 6


def loaded_u32(elf, addr):
    """The word at @addr as the image loads it, or None."""
    for s in elf.sections:
        if (s.flags & SHF_ALLOC and s.type != SHT_NOBITS and s.size
                and s.addr <= addr and addr + 4 <= s.addr + s.size):
            return struct.unpack_from("<I", elf.raw,
                                      s.offset + addr - s.addr)[0]
    return None


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("elf")
    ap.add_argument("--declared", required=True, type=int,
                    help="the DECLARED veneer_cost_gate() checked")
    ap.add_argument("--show-limits", action="store_true",
                    help="also print the stack_limit[] the policy holds")
    args = ap.parse_args()
    want_acct = check_plugin_image.ABI["PLUGIN_STACK_ACCOUNTING"]

    def fail(msg):
        print(f"check_policy_probe: FAIL -- {msg}", file=sys.stderr)
        return 1

    try:
        elf = Elf(args.elf)
    except InputError as exc:
        return fail(str(exc))
    hits = [s for s in elf.symbols if s[0] == PROBE]
    if len(hits) != 1:
        return fail(f"{len(hits)} symbols named {PROBE} in {args.elf}; the "
                    "board must export its plugin policy exactly once "
                    "(PLUGIN_POLICY_PROBE in svc/plugin_load.h)")
    _, addr, size, _, _ = hits[0]
    if size != 4 * PROBE_WORDS:
        return fail(f"{PROBE} is {size} B, not {4 * PROBE_WORDS}")
    w = [loaded_u32(elf, addr + 4 * i) for i in range(PROBE_WORDS)]
    if None in w:
        return fail(f"{PROBE} at 0x{addr:08x} is not in loadable bytes")
    magic, psize, off_cost, off_acct, off_lim, ptr = w
    if magic != MAGIC:
        return fail(f"{PROBE} magic 0x{magic:08x}, not 0x{MAGIC:08x}")
    for off in (off_cost, off_acct):
        if off + 4 > psize:
            return fail(f"offset {off} lies outside a {psize} B policy")
    cost = loaded_u32(elf, ptr + off_cost)
    acct = loaded_u32(elf, ptr + off_acct)
    if cost is None or acct is None:
        return fail(f"the policy at 0x{ptr:08x} is not in loadable bytes")
    bad = []
    if cost != args.declared:
        bad.append(f"its veneer_cost is {cost} B, but the build checked the "
                   f"firmware against DECLARED {args.declared} B -- a later -D "
                   "or #define of PLUGIN_VENEER_BASE_COST overrode the one "
                   "veneer_cost_gate() passed, and the loader would charge "
                   "that instead")
    if acct != want_acct:
        bad.append(f"its stack_accounting is {acct}, the ABI's is {want_acct}")
    if bad:
        return fail(f"the plugin policy at 0x{ptr:08x} in {args.elf}: "
                    + "; ".join(bad))
    msg = (f"check_policy_probe: OK -- the policy at 0x{ptr:08x} charges "
           f"c = {cost} B (= DECLARED) under stack accounting {acct}")
    if args.show_limits:
        lims = [loaded_u32(elf, ptr + off_lim + 4 * i) for i in range(7)]
        msg += f"; stack_limit {lims}"
    print(msg)
    return 0


if __name__ == "__main__":
    sys.exit(main())
