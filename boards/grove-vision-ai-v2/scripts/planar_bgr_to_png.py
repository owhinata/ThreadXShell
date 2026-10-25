#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 ThreadX Shell Project
"""Convert a planar B/G/R frame sent by `camera send` (Grove Vision AI V2) to PNG.

The firmware sends the last `camera capture` exactly as the datapath wrote it:
three 8-bit planes, one after the other -- the whole B plane, then G, then R
(WDMA3's order, the same as `camera capture`'s B(plane 0) / G(plane 1) /
R(plane 2) lines) -- 320x240 by default, 230400 bytes.  This interleaves them
into RGB.  Nothing is scaled: the values are the planes' own.

Usage:
    python3 boards/grove-vision-ai-v2/scripts/planar_bgr_to_png.py frame.raw out.png
    python3 boards/grove-vision-ai-v2/scripts/planar_bgr_to_png.py \\
        --width 320 --height 240 frame.raw out.png

Requires Pillow:  pip install Pillow
"""

import argparse
import sys

from PIL import Image


def planar_bgr_to_rgb888(data: bytes, width: int, height: int) -> bytes:
    n = width * height
    b, g, r = data[0:n], data[n:2 * n], data[2 * n:3 * n]
    out = bytearray(n * 3)
    out[0::3] = r
    out[1::3] = g
    out[2::3] = b
    return bytes(out)


def main() -> int:
    ap = argparse.ArgumentParser(
        description="planar B/G/R (camera send) -> PNG")
    ap.add_argument("input", help="raw planar file (e.g. from `camera send`)")
    ap.add_argument("output", help="PNG file to write")
    ap.add_argument("--width", type=int, default=320)
    ap.add_argument("--height", type=int, default=240)
    args = ap.parse_args()

    with open(args.input, "rb") as f:
        data = f.read()

    expected = args.width * args.height * 3
    if len(data) != expected:
        print(f"error: {args.input} is {len(data)} bytes, expected "
              f"{expected} ({args.width}x{args.height} planar B/G/R)",
              file=sys.stderr)
        return 1

    rgb = planar_bgr_to_rgb888(data, args.width, args.height)
    Image.frombytes("RGB", (args.width, args.height), rgb).save(args.output)
    print(f"wrote {args.output} ({args.width}x{args.height})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
