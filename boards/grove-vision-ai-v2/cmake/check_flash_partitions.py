#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 ThreadX Shell Project
"""Refuse a flash that would land on top of something else (issue #45).

WHY THIS EXISTS

Until now "the model does not overwrite the firmware" was a habit: one model,
one hardcoded offset, and a comment asking two files to agree.  A second model
turns that into a layout, and a layout nobody checks is a layout that is wrong
the first time somebody moves something.  The cost of being wrong is specific:
the external NOR is rewritten in place with no read-back, the console and the
flash channel are the same serial port, and recovery from a clobbered
bootloader means the BOOT_OPT strap and a factory image.

THE LAYOUT IS RESERVATIONS, NOT FILES

Each partition declares where it starts and how much flash it may consume.  The
reservations are checked against each other with no files present at all -- a
layout is a property of addresses, not of what happens to be built today.  Only
the artifact actually being written has to exist.

[!] That separation is the whole design, and getting it wrong the other way is
easy: an earlier version of this script required EVERY declared file, which made
plain firmware flashing refuse on any tree where the detection model -- which
cannot be committed, being model-zoo licensed -- had not been built by hand.  A
gate that blocks the operation it is not protecting gets deleted, and then
nothing is protected.

WHAT IT COMPARES: DESTROYED FOOTPRINTS, NOT FILE EXTENTS

Two things make the bytes destroyed by a write larger than the file:

  * xmodem sends whole packets.  The last one is padded, so the receiver is
    handed ceil(size / packet) * packet bytes and writes all of them.

  * flash is erased in blocks.  The receiver is the Himax bootloader resident
    in the NOR itself -- closed, and not disassembled to a conclusion here --
    so the block size it uses is not known.  The SDK's own SPI flash library
    offers 4 KB sectors, 32 KB blocks and 64 KB blocks, and this check rounds
    to the LARGEST of them.  That is a conservative bound and is stated as one:
    whatever the bootloader actually does, its footprint is contained in what
    is checked here.  (A bound is the right shape for this: erring large costs
    address space, of which there is spare; erring small costs a board.)

The one thing it cannot bound is a receiver that erases the whole chip before
writing.  Nothing static could; the evidence is empirical, from the hardware
check that reads the OTHER model back after flashing one.

Exit 0 and print the layout when the reservations are disjoint, every supplied
artifact fits its own reservation, and the artifact being written exists.
"""

import argparse
import os
import sys


def parse_int(text):
    """Accept 0x... or decimal, and nothing else -- a silently-zero address is
    exactly the mistake this file exists to catch."""
    return int(text, 0)


def block_span(start, length, granularity):
    """[first, last) of flash a write of @length at @start can disturb."""
    g = granularity
    first = (start // g) * g
    last = ((start + length + g - 1) // g) * g
    return first, last


class Partition:
    def __init__(self, spec, packet, granularity):
        parts = spec.split(":")
        if len(parts) != 3:
            raise ValueError(
                f"--partition wants NAME:START:RESERVED, got {spec!r}")
        self.name = parts[0]
        self.start = parse_int(parts[1])
        self.reserved = parse_int(parts[2])
        self.packet = packet
        self.granularity = granularity
        self.path = None          # set by --image
        self.writing = False      # set by --writing

        if not self.name:
            raise ValueError("a partition needs a name")
        if self.start < 0:
            raise ValueError(f"{self.name}: negative start address")
        if self.reserved <= 0:
            raise ValueError(f"{self.name}: reservation must be positive")

    @property
    def reservation(self):
        """The blocks this partition owns.  Rounded the same way a write is, so
        that a start which is not block-aligned still owns the block it sits
        in -- otherwise a neighbour could be given blocks this partition's own
        erase would take."""
        return block_span(self.start, self.reserved, self.granularity)

    def transferred(self, size):
        """Bytes the receiver is handed: whole xmodem packets."""
        packets = (size + self.packet - 1) // self.packet
        return packets * self.packet


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--flash-size", type=parse_int, required=True,
                    help="total flash bytes (W25Q128JW: 0x1000000)")
    ap.add_argument("--packet", type=parse_int, default=128,
                    help="xmodem packet size the sender uses")
    ap.add_argument("--erase-granularity", type=parse_int, default=0x10000,
                    help="largest erase block the part offers (see the header)")
    ap.add_argument("--partition", action="append", default=[],
                    metavar="NAME:START:RESERVED",
                    help="one declared region; repeat for each")
    ap.add_argument("--image", action="append", default=[], metavar="NAME:FILE",
                    help="an artifact to measure against its reservation")
    ap.add_argument("--writing", action="append", default=[], metavar="NAME",
                    help="the partition(s) this flash writes; their images must "
                         "exist")
    args = ap.parse_args()

    if not args.partition:
        print("check_flash_partitions: no partitions declared", file=sys.stderr)
        return 1

    try:
        parts = [Partition(s, args.packet, args.erase_granularity)
                 for s in args.partition]
    except ValueError as exc:
        print(f"check_flash_partitions: {exc}", file=sys.stderr)
        return 1

    by_name = {}
    for p in parts:
        if p.name in by_name:
            print(f"check_flash_partitions: {p.name} declared twice",
                  file=sys.stderr)
            return 1
        by_name[p.name] = p

    for spec in args.image:
        name, _, path = spec.partition(":")
        if name not in by_name or not path:
            print(f"check_flash_partitions: --image {spec!r} names no declared "
                  f"partition", file=sys.stderr)
            return 1
        by_name[name].path = path

    for name in args.writing:
        if name not in by_name:
            print(f"check_flash_partitions: --writing {name!r} names no "
                  f"declared partition", file=sys.stderr)
            return 1
        if by_name[name].path is None:
            print(f"check_flash_partitions: --writing {name!r} without a "
                  f"matching --image", file=sys.stderr)
            return 1
        by_name[name].writing = True

    fails = []

    print(f"flash    : {args.flash_size} B, "
          f"erase granularity {args.erase_granularity} B (conservative bound), "
          f"xmodem packet {args.packet} B")
    print(f"{'partition':<12} {'reserved blocks':>25} {'file':>10} {'sent':>10} "
          f"  state")

    for p in parts:
        r0, r1 = p.reservation
        size = sent = None
        if p.path is not None and os.path.isfile(p.path):
            size = os.path.getsize(p.path)
            sent = p.transferred(size)
        state = "WRITING" if p.writing else ("present" if size is not None
                                             else "not built")
        print(f"{p.name:<12}   0x{r0:08x}..0x{r1:08x} "
              f"{'' if size is None else size:>10} "
              f"{'' if sent is None else sent:>10}   {state}")

    # 1. The reservations themselves.  No file is consulted, so this is checked
    #    on every flash of every kind -- including one that writes a partition
    #    whose neighbours were never built.
    for p in parts:
        r0, r1 = p.reservation
        if p.start > args.flash_size or p.reserved > args.flash_size - p.start:
            fails.append(
                f"{p.name}: reserves {p.reserved} B at 0x{p.start:08x}, past "
                f"the end of a {args.flash_size} B flash")
        elif r1 > args.flash_size:
            fails.append(
                f"{p.name}: its reservation rounds up to 0x{r1:08x}, past the "
                f"end of a {args.flash_size} B flash")

    for i in range(len(parts)):
        for j in range(i + 1, len(parts)):
            a, b = parts[i], parts[j]
            a0, a1 = a.reservation
            b0, b1 = b.reservation
            if a0 < b1 and b0 < a1:
                fails.append(
                    f"{a.name} (0x{a0:08x}..0x{a1:08x}) and "
                    f"{b.name} (0x{b0:08x}..0x{b1:08x}) reserve the same flash "
                    f"blocks.\n"
                    f"          Flashing one would destroy part of the other.")

    # 2. Whatever artifacts we were given: each must fit the blocks its own
    #    partition owns.  Checking a model that is not being written is free
    #    coverage -- it catches a model that outgrew its slot before the flash
    #    that would have proved it the destructive way.
    for p in parts:
        if p.path is None:
            continue
        if not os.path.isfile(p.path):
            if p.writing:
                fails.append(
                    f"{p.name}: no file at {p.path}\n"
                    f"          This is the artifact being written.  Build it\n"
                    f"          first -- see the board README.")
            continue
        size = os.path.getsize(p.path)
        sent = p.transferred(size)
        w0, w1 = block_span(p.start, sent, args.erase_granularity)
        r0, r1 = p.reservation
        if w0 < r0 or w1 > r1:
            fails.append(
                f"{p.name}: writing {size} B ({sent} B sent) at "
                f"0x{p.start:08x} disturbs 0x{w0:08x}..0x{w1:08x}, outside its "
                f"reservation 0x{r0:08x}..0x{r1:08x}.\n"
                f"          Either the artifact outgrew its slot or the slot "
                f"is declared wrong.")

    for f in fails:
        print(f"  FAIL   {f}")
    if fails:
        print("RESULT   : REFUSE")
        return 1

    print("RESULT   : OK (reservations are disjoint; nothing written strays "
          "outside its own)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
