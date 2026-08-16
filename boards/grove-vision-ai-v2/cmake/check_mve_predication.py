#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 ThreadX Shell Project
"""Gate 3: no VPR-dependent MVE (Helium) instructions in the linked image.

[!] THE PREMISE THIS GATE WAS WRITTEN ON IS WRONG (issue #42).  It was that the
ThreadX Cortex-M55 port saves s16-s31 (aliasing q4-q7) across context switches
but NOT the VPR predication register, leaving predicated MVE code that spans a
context switch computing with the wrong predicate.  The Armv8-M ARM contradicts
it: PushStack/PopStack stack and unstack VPR under HaveMve(), and rule RZWQX
makes MVE execution set CONTROL.FPCA, so the hardware preserves VPR and the
port only owns the callee-saved half -- which it does save.

The gate stays until #42 decides, because it is fail-closed: it costs scalar
code, not correctness, and removing it has its own prerequisite (enforcing and
reading back FPCCR.ASPEN, since this app inherits that state from a
bootloader).  This board's own code avoids MVE in M-G1 and the prebuilt driver
archive was scanned clean at plan time; this gate keeps that true build over
build.

Flagged: the VPT/VPST/VCTP families, VPSEL/VPNOT, and any explicit VPR
operand (VMSR/VMRS to/from VPR).  Plain unpredicated MVE loads/stores are
fine (their vector registers ARE saved) and are not flagged.

The disassembler is the toolchain-pinned objdump (mapping symbols keep
literal pools out of the decode); mnemonics are matched case-insensitively.

Stdlib-only; POST_BUILD.
"""

import argparse
import re
import subprocess
import sys

# Mnemonic families that read or write VPR.
BAD_MNEMONIC = re.compile(r"^(vpt|vpst|vctp|vpsel|vpnot)", re.IGNORECASE)
BAD_OPERAND = re.compile(r"\bvpr\b", re.IGNORECASE)

# objdump disassembly line: "10000780:\t<hex>\t<mnemonic>\t<operands>"
INSN = re.compile(r"^\s*[0-9a-f]+:\s+(?:[0-9a-f]{4,8}\s+)+([a-zA-Z][\w.]*)"
                  r"(?:\s+(.*))?$")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--objdump", required=True)
    ap.add_argument("elf")
    args = ap.parse_args()

    out = subprocess.run([args.objdump, "-d", args.elf], check=True,
                         capture_output=True, text=True).stdout

    hits = []
    for line in out.splitlines():
        m = INSN.match(line)
        if not m:
            continue
        mnemonic = m.group(1)
        operands = m.group(2) or ""
        if BAD_MNEMONIC.match(mnemonic) or \
           (mnemonic.lower().startswith(("vmsr", "vmrs")) and
                BAD_OPERAND.search(operands)):
            hits.append(line.strip())

    if hits:
        print("check_mve_predication: FAIL -- VPR-dependent MVE present "
              "(the ThreadX M55 port does not save VPR):", file=sys.stderr)
        for h in hits[:20]:
            print(f"  {h}", file=sys.stderr)
        if len(hits) > 20:
            print(f"  ... and {len(hits) - 20} more", file=sys.stderr)
        return 1

    print("check_mve_predication: OK (no VPT/VPST/VCTP/VPSEL/VPNOT/VPR use)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
