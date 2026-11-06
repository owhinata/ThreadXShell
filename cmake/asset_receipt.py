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

[!] THE COMMANDS ARE THE SHARED `blob` COMMAND'S, SO THEY LIVE HERE (issue
#195).  Until then each board passed its own lines (--step, --then, --transfer,
--verify), from when each board's store spoke differently; since #194 all three
run shell/cmds/cmd_blob.c, and the three copies had drifted (Grove lacked the
`then:` line and the note on `blob write`).  So the text below is the one
receipt, and a board passes only its slot -- the same value add_asset() gave
the fit check.  The removed options are left to argparse, which refuses each
by name: what is left takes exactly two positional values and nothing that
could absorb a stray word.

[!] AND IT FOLLOWS THE COMMAND, NOT THE OTHER WAY ROUND.  `blob write <name>
<slot>` erases the slot and receives into it when the slot is empty or holds
this same name (it is rewritten); it refuses, before any erase, a slot that
holds another blob, and a name already stored in another slot, and tells the
operator to `blob erase` on purpose.  The receipt used to start with `blob
erase <slot>`, which cleared exactly that protection every time it was
followed to the letter.
"""
import argparse
import json
import os
import sys
import zlib

ap = argparse.ArgumentParser()
ap.add_argument("receipt")
ap.add_argument("slot")
args = ap.parse_args()
receipt, slot = args.receipt, args.slot
if not slot.isascii() or not slot.isdigit():
    sys.exit("asset_receipt: the slot must be a slot number, got %r" % slot)
r = json.load(open(receipt))

# [!] RECOMPUTED FROM THE FILE, NOT TRUSTED FROM THE RECEIPT.  The receipt and
# the .nnc are two files published one after the other, so an interrupted build
# could in principle pair a new receipt with an older container -- and a CRC that
# describes neither the file nor what the board stored is worse than none, since
# the whole point is that an operator acts on this number.  Recomputing costs
# milliseconds and makes the pairing unfalsifiable.
if not os.path.isfile(r["path"]):
    sys.exit("asset_receipt: %s is missing.  Rebuild the asset." % r["path"])
_actual = zlib.crc32(open(r["path"], "rb").read()) & 0xFFFFFFFF
if "%08X" % _actual != r["crc32"]:
    sys.exit("asset_receipt: %s does not match its receipt (file %08X, receipt "
             "%s).  Rebuild the asset -- do not send this file."
             % (r["path"], _actual, r["crc32"]))
name, crc = r["name"], r["crc32"]

# The one receipt.  The lines are fixed text with the name, slot and CRC put
# in, so a longer name or a two-digit slot only lengthens a line; the label
# column (two spaces, then 15 wide) never moves.
RECEIPT = """
  asset {name:<12} {path}
  {dest:<18} {bytes} B   crc32 {crc}

  on the board:  blob write {name} {slot}   (erases the slot, then waits for the file)
                 refused if slot {slot} holds another name, or {name} is
                 already in another slot: `blob list` shows which, and
                 `blob erase <that slot>` clears it on purpose
  in picocom:    C-a C-s, then the path above
  afterwards:    `blob list` must show crc32 {crc} -- that is how you know
                 the bytes that were built are the bytes that were stored.
  then:          nn model load {name}
"""
sys.stdout.write(RECEIPT.format(name=name, path=r["path"], slot=slot,
                                dest="slot %s" % slot, bytes=r["bytes"],
                                crc=crc) + "\n")
