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
      -> stack_limit[slot] == --stack-limit slot=bytes, for EVERY slot (#126)

[!] THE STACK ALLOWANCES ARE THE SAME CLASS OF NUMBER (issue #126).  They reach
the policy as board-named -D's from plugin_stack_table(), and a later -D wins
over them in the same way.  The table is the one the plugin image gate and the
host container verifier were given, so a policy that holds anything else --
lower or higher, by an override or by the firmware mapping a slot to another
allowance -- is a device that admits by a limit nothing on the host checked.
What this cannot see: two allowances of EQUAL value swapped between slots (the
device enforces the same numbers either way), and whether the table itself is
right (that is the firmware's assert against each thread's stack, and the
measured depths in the board README).

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
SHF_WRITE = 0x1
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
    ap.add_argument("--stack-limit", action="append", default=[],
                    metavar="SLOT=BYTES",
                    help="the board's plugin_stack_table(), one per slot; "
                         "every slot of the ABI is required")
    args = ap.parse_args()
    want_acct = check_plugin_image.ABI["PLUGIN_STACK_ACCOUNTING"]

    def fail(msg):
        print(f"check_policy_probe: FAIL -- {msg}", file=sys.stderr)
        return 1

    # The table, by slot NAME.  The indices are the ABI's (check_plugin_image
    # carries them, pinned to svc/plugin_abi.h by run_plugin_gate_tests.py), so
    # neither the caller nor this file states an order.  Every slot is required:
    # a missing row would leave one number of the policy unchecked.
    abi = check_plugin_image.ABI
    count = abi["PLUGIN_SLOT_COUNT"]
    slots = {k[len("PLUGIN_SLOT_"):].lower(): v for k, v in abi.items()
             if k.startswith("PLUGIN_SLOT_") and k != "PLUGIN_SLOT_COUNT"}
    if sorted(slots.values()) != list(range(count)):
        return fail(f"the ABI table names slots {sorted(slots.values())}, "
                    f"not 0..{count - 1}")
    want = {}
    for e in args.stack_limit:
        name, eq, val = e.partition("=")
        if not eq or name not in slots or not val.isdigit() or int(val) <= 0:
            return fail(f"--stack-limit {e!r}: expected <slot>=<bytes> with a "
                        f"positive byte count and one of {sorted(slots)}")
        if name in want:
            return fail(f"--stack-limit names slot {name} twice")
        want[name] = int(val)
    missing = sorted(set(slots) - set(want))
    if missing:
        return fail(f"--stack-limit has no row for {', '.join(missing)}; the "
                    "board's plugin_stack_table() declares every slot")

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
    # [!] THE POLICY MUST BE DECLARED READ-ONLY (issue #126 review).  What is
    # read here is the object's INITIAL bytes; a policy in a writable section
    # (a non-const object in .data) can hold something else by the time
    # plugin_parse() reads it.  This sees only the section's flags: Grove's
    # .rodata is SRAM, writable by the CPU, so it catches a policy DECLARED
    # writable, not one written through a cast.  Nor does anything here show
    # that this object is the one the board passes to plugin_parse() -- the
    # same root as parity P13.
    home = [s for s in elf.sections
            if s.flags & SHF_ALLOC and s.type != SHT_NOBITS and s.size
            and s.addr <= ptr and ptr + psize <= s.addr + s.size]
    if len(home) != 1:
        return fail(f"the policy at 0x{ptr:08x} ({psize} B) is not inside one "
                    "loadable section")
    if home[0].flags & SHF_WRITE:
        return fail(f"the policy at 0x{ptr:08x} is in {home[0].name}, a "
                    "writable section: what the image holds there is only its "
                    "initial value.  Declare the board's policy const.")
    if off_lim + 4 * count > psize:
        return fail(f"stack_limit[{count}] at offset {off_lim} lies outside a "
                    f"{psize} B policy")
    cost = loaded_u32(elf, ptr + off_cost)
    acct = loaded_u32(elf, ptr + off_acct)
    lims = [loaded_u32(elf, ptr + off_lim + 4 * i) for i in range(count)]
    if cost is None or acct is None or None in lims:
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
    off = [f"slot {name} holds a stack limit of {lims[idx]} B where the "
           f"table declares {want[name]} B"
           for name, idx in sorted(slots.items(), key=lambda kv: kv[1])
           if lims[idx] != want[name]]
    if off:
        bad.append("; ".join(off) + " -- the board's plugin_stack_table() is "
                   "not what the policy holds: a later -D or #define of an "
                   "allowance overrode the table's, or the firmware maps a "
                   "slot to another allowance, and the device would admit by "
                   "a limit the plugin gate and the host verifier did not use")
    if bad:
        return fail(f"the plugin policy at 0x{ptr:08x} in {args.elf}: "
                    + "; ".join(bad))
    table = ", ".join(f"{n} {lims[i]}" for n, i in
                      sorted(slots.items(), key=lambda kv: kv[1]))
    print(f"check_policy_probe: OK -- the policy at 0x{ptr:08x} charges "
          f"c = {cost} B (= DECLARED) under stack accounting {acct}, and its "
          f"stack limits are the board's table ({table} B)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
