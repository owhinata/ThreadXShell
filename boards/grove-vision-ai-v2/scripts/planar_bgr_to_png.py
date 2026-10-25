#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 ThreadX Shell Project
"""Convert a planar B/G/R frame sent by `camera send` (Grove Vision AI V2) to PNG.

The firmware sends the last `camera capture` exactly as the datapath wrote it:
three 8-bit planes, one after the other -- the whole B plane, then G, then R
(WDMA3's order, the same as `camera capture`'s B(plane 0) / G(plane 1) /
R(plane 2) lines) -- 320x240 by default, 230400 bytes.  By default this
interleaves them into RGB and nothing else: the values are the planes' own.

The panel does NOT show those values.  The firmware's packer
(port/camera/cam_convert.c) subtracts a black level, applies the white-balance
gains, spreads the colours away from the luma (saturation), sRGB-encodes, and
truncates to RGB565 before the frame reaches the LCD -- so the raw PNG is
darker, flatter and paler than the panel (issue #151).  Give any of --black /
--wb / --sat / --gamma and this script runs the same integer pipeline, in the
same order, and writes what the panel shows: the RGB565 result, expanded back
to 8 bits per channel by bit replication (r5 -> r5<<3 | r5>>2, g6 -> g6<<2 |
g6>>4).  The values to pass are the ones `camera info` prints on its `wb` line,
which change under `camera wb / black / sat / gamma` and the automatic white
balance; with the firmware's defaults (unity gains, before `camera auto` moves
r and b) it reads

    wb       : r 256  g 256  b 256   black 16   sat 600   gamma sRGB

    --wb 256,256,256 --black 16 --sat 600 --gamma srgb

An option left out takes its NEUTRAL value (black 0, wb 256,256,256, sat 256,
gamma none), not the firmware's default, so copy all four from `camera info`.
As in the firmware, sat 0 and sat 256 both leave the colours alone.

Usage:
    python3 boards/grove-vision-ai-v2/scripts/planar_bgr_to_png.py frame.raw out.png
    python3 boards/grove-vision-ai-v2/scripts/planar_bgr_to_png.py \\
        --width 320 --height 240 frame.raw out.png
    python3 boards/grove-vision-ai-v2/scripts/planar_bgr_to_png.py \\
        --wb 256,256,256 --black 16 --sat 600 --gamma srgb frame.raw out.png

Requires Pillow:  pip install Pillow
"""

import argparse
import sys

# sRGB encode, linear 8-bit in, display-encoded 8-bit out.  [!] DATA COPIED
# VERBATIM from cam_srgb_encode[256] in
# boards/grove-vision-ai-v2/port/camera/cam_convert.c -- not regenerated from
# the formula, so that rounding cannot make the two disagree.  The host test
# (boards/grove-vision-ai-v2/test/test_planar_bgr_to_png.py) compares it with
# the firmware's table byte for byte.
SRGB_ENCODE = (
      0,  13,  22,  28,  34,  38,  42,  46,  50,  53,  56,  59,
     61,  64,  66,  69,  71,  73,  75,  77,  79,  81,  83,  85,
     86,  88,  90,  92,  93,  95,  96,  98,  99, 101, 102, 104,
    105, 106, 108, 109, 110, 112, 113, 114, 115, 117, 118, 119,
    120, 121, 122, 124, 125, 126, 127, 128, 129, 130, 131, 132,
    133, 134, 135, 136, 137, 138, 139, 140, 141, 142, 143, 144,
    145, 146, 147, 148, 148, 149, 150, 151, 152, 153, 154, 155,
    155, 156, 157, 158, 159, 159, 160, 161, 162, 163, 163, 164,
    165, 166, 167, 167, 168, 169, 170, 170, 171, 172, 173, 173,
    174, 175, 175, 176, 177, 178, 178, 179, 180, 180, 181, 182,
    182, 183, 184, 185, 185, 186, 187, 187, 188, 189, 189, 190,
    190, 191, 192, 192, 193, 194, 194, 195, 196, 196, 197, 197,
    198, 199, 199, 200, 200, 201, 202, 202, 203, 203, 204, 205,
    205, 206, 206, 207, 208, 208, 209, 209, 210, 210, 211, 212,
    212, 213, 213, 214, 214, 215, 215, 216, 216, 217, 218, 218,
    219, 219, 220, 220, 221, 221, 222, 222, 223, 223, 224, 224,
    225, 226, 226, 227, 227, 228, 228, 229, 229, 230, 230, 231,
    231, 232, 232, 233, 233, 234, 234, 235, 235, 236, 236, 237,
    237, 238, 238, 238, 239, 239, 240, 240, 241, 241, 242, 242,
    243, 243, 244, 244, 245, 245, 246, 246, 246, 247, 247, 248,
    248, 249, 249, 250, 250, 251, 251, 251, 252, 252, 253, 253,
    254, 254, 255, 255,
)
assert len(SRGB_ENCODE) == 256

# The firmware's constants (cam_convert.c / cam_convert.h).
WB_UNITY = 256
SAT_UNITY = 256
SPREAD_LIMIT = 256          # CAM_SPREAD_LIMIT
FIELD_DROP = (3, 2, 3)      # B, G, R: bits dropped to 5/6/5
FIELD_SHIFT = (0, 5, 11)    # B, G, R: position in the RGB565 word


def planar_bgr_to_rgb888(data: bytes, width: int, height: int) -> bytes:
    n = width * height
    b, g, r = data[0:n], data[n:2 * n], data[2 * n:3 * n]
    out = bytearray(n * 3)
    out[0::3] = r
    out[1::3] = g
    out[2::3] = b
    return bytes(out)


def gain8(v: int, gain: int, black: int) -> int:
    """cam_gain8(): pedestal off, then (v * gain) >> 8, saturating at 255."""
    v = v - black if v > black else 0
    return min((v * gain) >> 8, 255)


def spread(t: int, sat: int) -> int:
    """One cam_tone.spread entry: (t * sat) / 256 in C (truncates toward
    zero, which Python's // does not), clamped to +-SPREAD_LIMIT."""
    q = abs(t * sat) // 256
    d = q if t * sat >= 0 else -q
    return max(-SPREAD_LIMIT, min(SPREAD_LIMIT, d))


def encode_field(c: int, s: int, gamma: bool) -> int:
    """One cam_tone.encode entry for channel c (0/1/2 = B/G/R): clamp to
    0..255, sRGB-encode if on, truncate and shift into the RGB565 word."""
    s = max(0, min(255, s))
    e = SRGB_ENCODE[s] if gamma else s
    return (e >> FIELD_DROP[c]) << FIELD_SHIFT[c]


def planar_bgr_to_rgb565(data: bytes, pixels: int, black: int, wb, sat: int,
                         gamma: bool) -> list:
    """cam_bgr_planar_to_rgb565_wb() on the host.  wb is (r, g, b) in 1/256ths,
    the order `camera info` prints.  Returns one RGB565 int per pixel."""
    b, g, r = data[0:pixels], data[pixels:2 * pixels], data[2 * pixels:]
    gains = (wb[2], wb[1], wb[0])          # B, G, R, like cam_tone_build()
    lo = -SPREAD_LIMIT
    enc = [[encode_field(c, s, gamma) for s in range(lo, 256 + SPREAD_LIMIT)]
           for c in range(3)]
    lin = [[gain8(v, gains[c], black) for v in range(256)] for c in range(3)]

    if sat in (SAT_UNITY, 0):
        # The firmware's folded path: no spread at all, sat 0 included.
        d = [[enc[c][lin[c][v] - lo] for v in range(256)] for c in range(3)]
        db, dg, dr = d
        return [dr[rv] | dg[gv] | db[bv] for bv, gv, rv in zip(b, g, r)]

    sp = [spread(t, sat) for t in range(-255, 256)]   # index t + 255
    lb, lg, lr = lin
    eb, eg, er = enc
    out = []
    for bv, gv, rv in zip(b, g, r):
        bv, gv, rv = lb[bv], lg[gv], lr[rv]
        y = (rv + 2 * gv + bv) // 4                    # non-negative: // == /
        out.append(er[y + sp[rv - y + 255] - lo] |
                   eg[y + sp[gv - y + 255] - lo] |
                   eb[y + sp[bv - y + 255] - lo])
    return out


def rgb565_to_rgb888(px: list) -> bytes:
    """Expand RGB565 to 8 bits per channel by bit replication, the usual way a
    5/6-bit panel value maps onto 0..255 (0 -> 0, full scale -> 255)."""
    out = bytearray(len(px) * 3)
    for i, p in enumerate(px):
        r5, g6, b5 = (p >> 11) & 0x1F, (p >> 5) & 0x3F, p & 0x1F
        out[3 * i] = (r5 << 3) | (r5 >> 2)
        out[3 * i + 1] = (g6 << 2) | (g6 >> 4)
        out[3 * i + 2] = (b5 << 3) | (b5 >> 2)
    return bytes(out)


def _ranged(lo: int, hi: int):
    def parse(s: str) -> int:
        v = int(s, 0)
        if not lo <= v <= hi:
            raise argparse.ArgumentTypeError(f"{s} is not in {lo}..{hi}")
        return v
    return parse


def _wb(s: str):
    parts = s.split(",")
    if len(parts) != 3:
        raise argparse.ArgumentTypeError("--wb takes R,G,B (e.g. 256,256,256)")
    return tuple(_ranged(0, 0xFFFF)(p) for p in parts)


def main() -> int:
    ap = argparse.ArgumentParser(
        description="planar B/G/R (camera send) -> PNG; with any tuning "
                    "option, what the panel shows")
    ap.add_argument("input", help="raw planar file (e.g. from `camera send`)")
    ap.add_argument("output", help="PNG file to write")
    ap.add_argument("--width", type=int, default=320)
    ap.add_argument("--height", type=int, default=240)
    ap.add_argument("--black", type=_ranged(0, 255),
                    help="black level subtracted first (camera info: black)")
    ap.add_argument("--wb", type=_wb, metavar="R,G,B",
                    help="white-balance gains, 256 = unity (camera info: "
                         "wb r/g/b)")
    ap.add_argument("--sat", type=_ranged(0, 0xFFFF),
                    help="saturation, 256 = unchanged; 0 also leaves the "
                         "colours alone (camera info: sat)")
    ap.add_argument("--gamma", choices=("srgb", "none"),
                    help="sRGB encode (camera info: gamma sRGB / off)")
    args = ap.parse_args()

    with open(args.input, "rb") as f:
        data = f.read()

    expected = args.width * args.height * 3
    if len(data) != expected:
        print(f"error: {args.input} is {len(data)} bytes, expected "
              f"{expected} ({args.width}x{args.height} planar B/G/R)",
              file=sys.stderr)
        return 1

    from PIL import Image   # here, so the test can import this without Pillow

    tuned = any(v is not None
                for v in (args.black, args.wb, args.sat, args.gamma))
    if tuned:
        px = planar_bgr_to_rgb565(
            data, args.width * args.height,
            args.black if args.black is not None else 0,
            args.wb if args.wb is not None else (WB_UNITY,) * 3,
            args.sat if args.sat is not None else SAT_UNITY,
            args.gamma == "srgb")
        rgb = rgb565_to_rgb888(px)
    else:
        rgb = planar_bgr_to_rgb888(data, args.width, args.height)
    Image.frombytes("RGB", (args.width, args.height), rgb).save(args.output)
    print(f"wrote {args.output} ({args.width}x{args.height}"
          f"{', as the panel shows it' if tuned else ''})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
