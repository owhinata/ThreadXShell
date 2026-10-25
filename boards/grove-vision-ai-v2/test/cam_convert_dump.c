/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Host driver for test_planar_bgr_to_png.py (issue #151): run the FIRMWARE's
 * planar B/G/R -> RGB565 packer over a frame read from stdin and dump what it
 * produced, so the PC-side script (scripts/planar_bgr_to_png.py) can be held
 * to it byte for byte.
 *
 * cam_convert.c is INCLUDED rather than linked, so that its static tables are
 * in scope: the RGB565 output alone truncates 3 or 2 bits per channel, and a
 * one-count error in the sRGB table or a spread entry could hide under that.
 * Dumping the tables the packer actually used closes it.
 *
 * usage: cam_convert_dump R G B BLACK GAMMA SAT < planar.raw > dump
 *
 * stdout, all little-endian:
 *   pixels * u16   the RGB565 frame (pixels = stdin length / 3)
 *   3 * 256 * u8   cam_tone.linear  (B, G, R: black level and gain applied)
 *   511 * s16      cam_tone.spread  (index t + 255)
 *   256 * u8       cam_srgb_encode
 */
#include <stdio.h>
#include <stdlib.h>

#include "cam_convert.c"

#define MAX_PIXELS (1024u * 1024u)

static uint8_t  in[3u * MAX_PIXELS + 1u];
static uint16_t px[MAX_PIXELS];

static void put16(uint16_t v)
{
	putchar((int)(v & 0xFFu));
	putchar((int)(v >> 8));
}

int main(int argc, char **argv)
{
	struct cam_wb wb;
	size_t n;
	uint32_t pixels, i, c;

	if (argc != 7) {
		fprintf(stderr, "usage: %s R G B BLACK GAMMA SAT < planar\n",
		        argv[0]);
		return 2;
	}
	wb.r = (uint16_t)strtoul(argv[1], 0, 0);
	wb.g = (uint16_t)strtoul(argv[2], 0, 0);
	wb.b = (uint16_t)strtoul(argv[3], 0, 0);
	wb.black = (uint8_t)strtoul(argv[4], 0, 0);
	wb.gamma = (uint8_t)strtoul(argv[5], 0, 0);
	wb.sat = (uint16_t)strtoul(argv[6], 0, 0);

	n = fread(in, 1u, sizeof in, stdin);
	if (n == 0u || n % 3u != 0u || n > 3u * MAX_PIXELS) {
		fprintf(stderr, "cam_convert_dump: bad input length %zu\n", n);
		return 2;
	}
	pixels = (uint32_t)(n / 3u);

	cam_bgr_planar_to_rgb565_wb(in, px, pixels, &wb);

	for (i = 0u; i < pixels; i++)
		put16(px[i]);
	for (c = 0u; c < 3u; c++)
		for (i = 0u; i < 256u; i++)
			putchar(cam_tone.linear[c][i]);
	for (i = 0u; i < 511u; i++)
		put16((uint16_t)cam_tone.spread[i]);
	for (i = 0u; i < 256u; i++)
		putchar(cam_srgb_encode[i]);
	return fflush(stdout) == 0 ? 0 : 1;
}
