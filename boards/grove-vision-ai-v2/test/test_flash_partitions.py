#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 ThreadX Shell Project
"""Host tests for cmake/check_flash_partitions.py (issue #45).

The gate refuses a flash that would land on top of something else.  Its cases
cannot be produced on hardware without deliberately destroying a partition, so
they are produced here instead -- with files of the exact sizes that put a
boundary where it matters.

Two cases earn this file:

  OVERLAP THROUGH ROUNDING.  Two regions whose byte extents do not touch but
  whose ERASE BLOCKS do.  A checker comparing file extents passes it, and the
  board comes back with one of the two gone.

  A GATE THAT BLOCKS THE WRONG OPERATION.  The first version required every
  declared artifact to exist, so flashing the FIRMWARE refused on any tree
  where the detection model -- which cannot be committed -- had not been built
  by hand.  A gate that stops work it is not protecting gets removed, and then
  nothing is protected.  The reservation/artifact split is what fixed it, and
  the tests below pin both halves.
"""

import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
CHECKER = os.path.join(HERE, "..", "cmake", "check_flash_partitions.py")

FLASH = 0x1000000
G = 0x10000          # the checker's default erase granularity
PACKET = 128

fails = 0


def run(partitions, images=(), writing=(), flash_size=FLASH, image_max=()):
    argv = [sys.executable, CHECKER, "--flash-size", hex(flash_size)]
    for name, start, reserved in partitions:
        argv += ["--partition", f"{name}:{hex(start)}:{hex(reserved)}"]
    for name, path in images:
        argv += ["--image", f"{name}:{path}"]
    for name, limit in image_max:
        argv += ["--image-max", f"{name}:{hex(limit)}"]
    for name in writing:
        argv += ["--writing", name]
    return subprocess.run(argv, capture_output=True, text=True)


def check(name, want_ok, partitions, images=(), writing=(), expect_text=None,
          flash_size=FLASH, image_max=()):
    global fails
    r = run(partitions, images, writing, flash_size, image_max)
    got_ok = (r.returncode == 0)
    out = r.stdout + r.stderr

    if got_ok != want_ok:
        print(f"  FAIL {name}: wanted {'OK' if want_ok else 'REFUSE'}, "
              f"got rc={r.returncode}\n{out}")
        fails += 1
        return
    if expect_text is not None and expect_text not in out:
        print(f"  FAIL {name}: output does not mention {expect_text!r}\n{out}")
        fails += 1
        return
    print(f"  ok   {name}")


def make(tmp, name, size):
    path = os.path.join(tmp, name)
    with open(path, "wb") as f:
        f.write(b"\x5a" * size)
    return path


def main():
    global fails

    # The real layout, so the tests exercise the shape actually shipped.
    REAL = [
        ("firmware",  0x000000, 0x200000),
        ("blob",      0x200000, 0x970000),
        ("model-cls", 0xB7B000, 0x1A5000),
        ("model-det", 0xD20000, 0x030000),
        ("blob-tail", 0xD50000, 0x2A0000),
        ("slot-header", 0xFF0000, 0x010000),
    ]
    # firmware reserves BOTH A/B slots because a flash lands in whichever is
    # inactive; one image still has to fit ONE of them (issue #85).
    REAL_MAX = [("firmware", 0x100000)]

    with tempfile.TemporaryDirectory() as tmp:
        small = make(tmp, "small.bin", 1024)
        one_block = make(tmp, "one_block.bin", G)
        three_blocks = make(tmp, "three_blocks.bin", 3 * G)
        missing = os.path.join(tmp, "never-built.tflite")

        # --- the layout alone, with no artifacts at all -------------------
        # This is what makes the gate independent of what happens to be built.
        check("the shipped layout checks out with no files present", True, REAL,
              image_max=REAL_MAX, expect_text="reservations are disjoint")

        # [!] The layout claims the WHOLE part, with no unnamed gap (issue #85).
        # The checker cannot assert this -- a gap is legal, and on most layouts
        # it is what you want -- but on this board it is a property worth
        # keeping: an unclaimed run is not spare capacity, it is capacity
        # nothing stops the next partition from being placed into.  Checked
        # here, where the shipped numbers live.
        # [!] Compare BLOCK SPANS, not byte extents -- the same rounding the
        # checker does.  model-cls starts at 0xB7B000 but owns the block from
        # 0xB70000, so a byte-extent comparison reports a gap that does not
        # exist.  Getting this wrong here is the file's own headline mistake.
        def span(start, reserved):
            return ((start // G) * G, ((start + reserved + G - 1) // G) * G)

        covered = sorted(span(st, n) for _, st, n in REAL)
        cursor = 0
        for start, end in covered:
            if start != cursor:
                print(f"  FAIL the shipped layout leaves 0x{cursor:06x}"
                      f"..0x{start:06x} unclaimed")
                fails += 1
            cursor = max(cursor, end)
        if cursor != FLASH:
            print(f"  FAIL the shipped layout stops at 0x{cursor:06x}, not the "
                  f"end of a 0x{FLASH:x} B flash")
            fails += 1
        else:
            print("  ok   the shipped layout claims every byte of the part")

        check("reservations that share a block are refused", False, [
            ("a", 0x100000, 0x010000),
            ("b", 0x108000, 0x010000),
        ], expect_text="reserve the same flash blocks")

        # [!] Byte extents 0x1000 apart, erase blocks identical.
        check("reservations overlap through rounding, not through bytes", False, [
            ("a", 0x100000, 0x000800),
            ("b", 0x100800, 0x000800),
        ], expect_text="reserve the same flash blocks")

        check("a reservation inside another is refused", False, [
            ("a", 0x000000, 0x100000),
            ("b", 0x010000, 0x010000),
        ], expect_text="reserve the same flash blocks")

        check("declaration order does not matter", False, [
            ("b", 0x010000, 0x010000),
            ("a", 0x000000, 0x100000),
        ], expect_text="reserve the same flash blocks")

        check("a reservation past the end of flash is refused", False, [
            ("a", FLASH - G, 2 * G),
        ], expect_text="past the end")

        check("the last block of flash is reservable", True, [
            ("a", FLASH - G, G),
        ])

        # --- the regression that motivated the split ----------------------
        check("flashing the firmware does not need the models", True, REAL,
              images=[("firmware", three_blocks),
                      ("model-cls", missing),
                      ("model-det", missing)],
              writing=["firmware"])

        check("flashing one model does not need the other", True, REAL,
              images=[("firmware", missing),
                      ("model-det", one_block)],
              writing=["model-det"])

        # ...but the thing actually being written must exist.
        check("the artifact being written must exist", False, REAL,
              images=[("model-det", missing)], writing=["model-det"],
              expect_text="This is the artifact being written")

        # --- artifacts against their own reservations ---------------------
        check("an artifact larger than its reservation is refused", False, [
            ("a", 0x100000, 0x010000),
            ("b", 0x200000, 0x010000),
        ], images=[("a", three_blocks)], writing=["a"],
           expect_text="outside its reservation")

        # Free coverage: a model that outgrew its slot is caught even when the
        # flash being run writes something else entirely.
        check("an oversized neighbour is caught while flashing the firmware",
              False, [
            ("firmware", 0x000000, 0x100000),
            ("model", 0x100000, 0x010000),
        ], images=[("firmware", small), ("model", three_blocks)],
           writing=["firmware"], expect_text="outside its reservation")

        # xmodem pads the last packet, so a file one byte over a block boundary
        # is handed to the receiver as a whole extra packet.
        exact = make(tmp, "exact.bin", G)
        over = make(tmp, "over.bin", G + 1)
        check("a file that exactly fills its reservation is accepted", True, [
            ("a", 0x100000, G),
        ], images=[("a", exact)], writing=["a"])
        check("one byte over the reservation is refused", False, [
            ("a", 0x100000, G),
        ], images=[("a", over)], writing=["a"],
           expect_text="outside its reservation")

        # --- the per-artifact ceiling (issue #85) ------------------------
        # firmware reserves two 1 MB slots but one image must fit one slot.
        SLOT = 0x100000
        TWO_SLOTS = [("firmware", 0x000000, 2 * SLOT)]
        one_slot = make(tmp, "one_slot.bin", SLOT)
        over_slot = make(tmp, "over_slot.bin", SLOT + 1)

        check("an artifact exactly at the ceiling is accepted", True,
              TWO_SLOTS, images=[("firmware", one_slot)], writing=["firmware"],
              image_max=[("firmware", SLOT)],
              expect_text="reservations are disjoint")

        # [!] The one this exists for: it fits the RESERVATION (2 MB) and would
        # have passed before, then been refused by the bootloader on hardware
        # with the serial port open and a reset already pressed.
        check("an artifact over the ceiling but inside the reservation is "
              "refused", False,
              TWO_SLOTS, images=[("firmware", over_slot)], writing=["firmware"],
              image_max=[("firmware", SLOT)],
              expect_text="ceiling on ONE artifact")

        check("without a ceiling the same artifact is accepted", True,
              TWO_SLOTS, images=[("firmware", over_slot)], writing=["firmware"],
              expect_text="reservations are disjoint")

        # --- malformed input ----------------------------------------------
        for argv, why in (
            (["--partition", "no-reservation:0x0"], "a malformed --partition"),
            (["--partition", "a:0x0:0"], "a zero reservation"),
            ([], "no partitions at all"),
            (["--partition", "a:0x0:0x1000", "--partition", "a:0x2000:0x1000"],
             "a duplicate partition name"),
            (["--partition", "a:0x0:0x1000", "--image", "b:/tmp/x"],
             "an --image for an undeclared partition"),
            (["--partition", "a:0x0:0x1000", "--writing", "a"],
             "--writing without --image"),
            (["--partition", "a:0x0:0x1000", "--image-max", "b:0x100"],
             "an --image-max for an undeclared partition"),
            (["--partition", "a:0x0:0x1000", "--image-max", "a:nonsense"],
             "an --image-max with an unreadable size"),
            (["--partition", "a:0x0:0x1000", "--image-max", "a:0x0"],
             "a zero --image-max"),
            # [!] A ceiling ABOVE the reservation can never fire -- the
            # reservation check refuses first -- so it would read as protection
            # while doing nothing.  Refuse the declaration instead (issue #85).
            (["--partition", "a:0x0:0x1000", "--image-max", "a:0x2000"],
             "an --image-max larger than the reservation"),
        ):
            r = subprocess.run(
                [sys.executable, CHECKER, "--flash-size", hex(FLASH)] + argv,
                capture_output=True, text=True)
            if r.returncode == 0:
                print(f"  FAIL {why} is accepted")
                fails += 1
            else:
                print(f"  ok   {why} refuses")

    if fails:
        print(f"test_flash_partitions: {fails} failure(s)")
        return 1
    print("test_flash_partitions: all cases pass")
    return 0


if __name__ == "__main__":
    sys.exit(main())
