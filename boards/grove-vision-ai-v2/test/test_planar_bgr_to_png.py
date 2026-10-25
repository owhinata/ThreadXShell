#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 ThreadX Shell Project
"""Host test: scripts/planar_bgr_to_png.py's panel pipeline IS the firmware's.

Issue #151.  The script re-implements cam_bgr_planar_to_rgb565_wb() so that a
`camera send` PNG can look like the panel.  A re-implementation drifts from the
original without anything noticing, so this runs the REAL cam_convert.c (via
cam_convert_dump.c, built by host_tests.sh) and the script over the same frames
and settings and compares:

  - the RGB565 frames, word for word;
  - the per-channel black/gain table, the saturation spread table and the sRGB
    table the firmware's packer used, entry for entry (the RGB565 output alone
    drops 2-3 bits per channel, under which a one-count error could hide).

Frames: every (B, G) pair at three R values (covers every value of every
channel, and every luma/spread combination the 2-D grid can reach), plus a
random 320x240 frame.  Settings: neutral, the firmware's defaults, both sat
short-circuits (0 and 256), the extremes of every knob, and random ones.

usage: test_planar_bgr_to_png.py --driver <cam_convert_dump> [--script <py>]
"""

import argparse
import importlib.util
import os
import random
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_SCRIPT = os.path.join(HERE, "..", "scripts", "planar_bgr_to_png.py")


def load(path):
    spec = importlib.util.spec_from_file_location("planar_bgr_to_png", path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def frames():
    grid_b, grid_g, grid_r = bytearray(), bytearray(), bytearray()
    for r in (0, 97, 255):
        for b in range(256):
            for g in range(256):
                grid_b.append(b)
                grid_g.append(g)
                grid_r.append(r)
    rng = random.Random(151)
    n = 320 * 240
    rand = bytes(rng.randrange(256) for _ in range(3 * n))
    return [("grid", bytes(grid_b + grid_g + grid_r)), ("random", rand)]


def settings():
    # (r, g, b, black, gamma, sat)
    s = [
        (256, 256, 256, 0, 0, 256),      # neutral: only the truncation
        (256, 256, 256, 16, 1, 600),     # the firmware's defaults (camera.c)
        (256, 256, 256, 16, 1, 0),       # sat 0 short-circuits like 256
        (300, 256, 410, 16, 0, 256),     # gains, linear, folded path
        (300, 256, 410, 16, 1, 1),       # sat 1: spread path, tiny spread
        (4096, 0, 1, 255, 1, 2048),      # extremes the commands accept
        (65535, 65535, 65535, 0, 1, 65535),  # the fields' own ceiling
        (128, 256, 512, 8, 1, 128),      # sat < 256 pulls toward the luma
    ]
    rng = random.Random(1510)
    for _ in range(4):
        s.append((rng.randrange(4097), rng.randrange(4097),
                  rng.randrange(4097), rng.randrange(256), rng.randrange(2),
                  rng.randrange(2049)))
    return s


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--driver", required=True)
    ap.add_argument("--script", default=DEFAULT_SCRIPT)
    args = ap.parse_args()
    mod = load(args.script)

    fails = 0
    checks = 0
    for fname, data in frames():
        pixels = len(data) // 3
        for (r, g, b, black, gamma, sat) in settings():
            tag = (f"{fname} r{r} g{g} b{b} black{black} "
                   f"gamma{gamma} sat{sat}")
            dump = subprocess.run(
                [args.driver, str(r), str(g), str(b), str(black), str(gamma),
                 str(sat)],
                input=data, stdout=subprocess.PIPE, check=True).stdout
            want_len = 2 * pixels + 768 + 1022 + 256
            if len(dump) != want_len:
                print(f"FAIL {tag}: driver wrote {len(dump)} bytes, "
                      f"expected {want_len}")
                fails += 1
                continue
            c_px = struct.unpack(f"<{pixels}H", dump[:2 * pixels])
            o = 2 * pixels
            c_lin = dump[o:o + 768]
            o += 768
            c_spread = struct.unpack("<511h", dump[o:o + 1022])
            o += 1022
            c_srgb = dump[o:o + 256]

            py_px = mod.planar_bgr_to_rgb565(data, pixels, black, (r, g, b),
                                             sat, bool(gamma))
            checks += 1
            if list(c_px) != py_px:
                i = next(i for i, (a, q) in enumerate(zip(c_px, py_px))
                         if a != q)
                print(f"FAIL {tag}: pixel {i} C 0x{c_px[i]:04x} "
                      f"py 0x{py_px[i]:04x}")
                fails += 1

            gains = (b, g, r)
            py_lin = bytes(mod.gain8(v, gains[c], black)
                           for c in range(3) for v in range(256))
            checks += 1
            if py_lin != c_lin:
                print(f"FAIL {tag}: black/gain table differs")
                fails += 1

            if sat not in (0, 256):
                # The firmware only consults spread off the folded path.
                py_spread = tuple(mod.spread(t, sat)
                                  for t in range(-255, 256))
                checks += 1
                if py_spread != c_spread:
                    print(f"FAIL {tag}: spread table differs")
                    fails += 1

            checks += 1
            if bytes(mod.SRGB_ENCODE) != c_srgb:
                bad = [i for i in range(256)
                       if mod.SRGB_ENCODE[i] != c_srgb[i]]
                print(f"FAIL {tag}: sRGB table differs at {bad[:8]}")
                fails += 1

    # The 8-bit expansion: bit replication, both ends exact.
    exp = mod.rgb565_to_rgb888([0x0000, 0xFFFF, 0x8410, 0xF800, 0x07E0,
                                0x001F])
    want = bytes([0, 0, 0, 255, 255, 255, 132, 130, 132, 255, 0, 0,
                  0, 255, 0, 0, 0, 255])
    checks += 1
    if exp != want:
        print(f"FAIL rgb565_to_rgb888: {exp.hex()} != {want.hex()}")
        fails += 1

    # No option -> the raw interleave, unchanged.
    raw = mod.planar_bgr_to_rgb888(bytes([1, 2, 3, 4, 5, 6]), 2, 1)
    checks += 1
    if raw != bytes([5, 3, 1, 6, 4, 2]):
        print(f"FAIL planar_bgr_to_rgb888: {raw.hex()}")
        fails += 1

    if fails:
        print(f"test_planar_bgr_to_png: {fails} of {checks} checks FAILED")
        return 1
    print(f"test_planar_bgr_to_png: {checks} checks OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
