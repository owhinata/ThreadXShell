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

[!] AND SO ARE THE TRANSFER AND THE COMPARISON (issue #159).  This file used to
print "in picocom: C-a C-s" and "`blob list` must show crc32" for every board;
f746 has neither -- its file goes to the card through a PC card reader, and the
CRC to compare is the one its load prints.  So each arrives as a label and its
lines (--transfer-label/--transfer, --verify-label/--verify, templates over
{name}, {slot}, {file} and {crc}).  Grove and wio pass the text this printed
before, so their receipts are byte-identical.

The destination is exactly one: a slot (the positional argument) or --file.
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
ap.add_argument("--file", default="",
                help="the file name on the board's card (a file destination)")
ap.add_argument("--transfer-label", required=True)
ap.add_argument("--transfer", action="append", default=[], required=True,
                help="how the file reaches the board, same template")
ap.add_argument("--verify-label", required=True)
ap.add_argument("--verify", action="append", default=[], required=True,
                help="how the stored bytes are compared with {crc}")
args = ap.parse_args()
receipt, slot = args.receipt, args.slot
if bool(slot) == bool(args.file):
    sys.exit("asset_receipt: exactly one destination -- a slot or --file -- "
             "is required")
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
dest_txt = ("slot %s" % slot) if slot else ("file %s" % args.file)
fill = dict(name=r["name"], slot=slot or "<slot>", file=args.file or "<file>",
            crc=r["crc32"])


def block(label, lines):
    # The label column is 15 wide after two spaces, as the receipt always was.
    for i, line in enumerate(lines):
        print("%s%s" % ("  %-15s" % label if i == 0 else " " * 17,
                        line.format(**fill)))


print("")
print("  asset %-12s %s" % (r["name"], r["path"]))
print("  %-18s %d B   crc32 %s" % (dest_txt, r["bytes"], r["crc32"]))
print("")
block("on the board:", args.step)
block(args.transfer_label, args.transfer)
block(args.verify_label, args.verify)
block("then:", args.then)
print("")
