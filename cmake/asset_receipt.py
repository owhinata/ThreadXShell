#!/usr/bin/env python3
"""Print one asset's receipt (issue #107; shared in #108).

Separate from the build so it can run from an always-out-of-date target: the
command that PRODUCES the .nnc does not rerun once its output is current, so the
number an operator needs would be printed exactly once and never again.

[!] THE CRC IS THE POINT.  With picocom started as `sb -k`, nothing on the PC
checks what is actually sent; the device stores a CRC over the payload as it
arrived, and svc/crc32.h says in so many words that the number exists so it can
be compared against the file that was sent.  This is the operator's only
end-to-end "built bytes = stored bytes" check, so it is printed every time
alongside the path to paste.

[!] THE COMMANDS TYPED ON THE BOARD ARE THE BOARD'S, AND ARRIVE AS --step.  The
two boards spell the store differently -- Grove names a blob and a slot, wio
only a slot -- and a receipt that printed one board's syntax on the other would
send an operator to a command that does not exist, at the one moment they are
following it to the letter.  Each --step is a template over {name} and {slot};
the first is printed as "on the board", the rest under it in order.  What is
NOT a board's -- the CRC and the instruction to compare it -- stays here.
A --then is printed after the CRC comparison, for what to type once the bytes
are known to be the right ones (wio names its model load there).
"""
import argparse
import json
import sys
import zlib

ap = argparse.ArgumentParser()
ap.add_argument("receipt")
ap.add_argument("slot", nargs="?", default="")
ap.add_argument("--step", action="append", default=[], required=True,
                help="a board command, as a template over {name} and {slot}")
ap.add_argument("--then", action="append", default=[],
                help="a board command for after the CRC check, same template")
args = ap.parse_args()
receipt, slot = args.receipt, args.slot
r = json.load(open(receipt))

# [!] RECOMPUTED FROM THE FILE, NOT TRUSTED FROM THE RECEIPT.  The receipt and
# the .nnc are two files published one after the other, so an interrupted build
# could in principle pair a new receipt with an older container -- and a CRC that
# describes neither the file nor what the board stored is worse than none, since
# the whole point is that an operator acts on this number.  Recomputing costs
# milliseconds and makes the pairing unfalsifiable.
_actual = zlib.crc32(open(r["path"], "rb").read()) & 0xFFFFFFFF
if "%08X" % _actual != r["crc32"]:
    sys.exit("asset_receipt: %s does not match its receipt (file %08X, receipt "
             "%s).  Rebuild the asset -- do not send this file."
             % (r["path"], _actual, r["crc32"]))
slot_txt = ("slot %s" % slot) if slot else "slot: pick one that fits"
print("")
print("  asset %-12s %s" % (r["name"], r["path"]))
print("  %-18s %d B   crc32 %s" % (slot_txt, r["bytes"], r["crc32"]))
print("")
for i, step in enumerate(args.step):
    print("%s%s" % ("  on the board:  " if i == 0 else " " * 17,
                    step.format(name=r["name"], slot=slot or "<slot>")))
print("  in picocom:    C-a C-s, then the path above")
print("  afterwards:    `blob list` must show crc32 %s -- that is how you know" % r["crc32"])
print("                 the bytes that were built are the bytes that were stored.")
for i, step in enumerate(args.then):
    print("%s%s" % ("  then:          " if i == 0 else " " * 17,
                    step.format(name=r["name"], slot=slot or "<slot>")))
print("")
