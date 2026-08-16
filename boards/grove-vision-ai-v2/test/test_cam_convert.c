/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Host test for the planar B/G/R -> RGB565 packer and the frame statistics
 * (issue #35, port/camera/cam_convert.c).
 *
 * WHY THIS EXISTS.  Every mistake this code can make produces a picture.  Swap
 * the R and B planes and you get a picture with the colours wrong; get the
 * endianness backwards and you get a picture with the colours wrong; read the
 * planes in the wrong order and you get a picture with the colours wrong.  On
 * the glass they are indistinguishable from a sensor whose Bayer pattern is
 * misconfigured, from a demosaic fed the wrong mirror setting, and from a
 * camera pointed at something red.  Debugging that costs a flash cycle per
 * hypothesis on a part with ~100k of them and a manual reset button.
 *
 * So the byte-level contract is pinned here, where a wrong answer is a diff and
 * not a photograph.
 */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cam_convert.h"

static int failures;

#define CHECK(cond, ...)                                                       \
	do {                                                                   \
		if (!(cond)) {                                                 \
			printf("FAIL %s:%d: ", __FILE__, __LINE__);            \
			printf(__VA_ARGS__);                                   \
			printf("\n");                                          \
			failures++;                                            \
		}                                                              \
	} while (0)

/* ---- 1. channel order and bit placement --------------------------------- */

static void test_channel_order(void)
{
	/* Three pixels, planar: B = {0xFF,0,0}, G = {0,0xFF,0}, R = {0,0,0xFF}
	 * -- i.e. pure blue, pure green, pure red, in that order. */
	static const uint8_t bgr[9] = {
		0xFFu, 0x00u, 0x00u,   /* B plane */
		0x00u, 0xFFu, 0x00u,   /* G plane */
		0x00u, 0x00u, 0xFFu,   /* R plane */
	};
	uint16_t out[3];

	memset(out, 0xAA, sizeof out);
	cam_bgr_planar_to_rgb565(bgr, out, 3u);

	/* R5<<11 | G6<<5 | B5.  Pure blue is the low five bits set, pure green
	 * the middle six, pure red the top five.  Getting this backwards is the
	 * classic BGR/RGB swap, and it is silent on the panel. */
	CHECK(out[0] == 0x001Fu, "pure blue packed as %04X, wanted 001F",
	      out[0]);
	CHECK(out[1] == 0x07E0u, "pure green packed as %04X, wanted 07E0",
	      out[1]);
	CHECK(out[2] == 0xF800u, "pure red packed as %04X, wanted F800",
	      out[2]);
}

/* ---- 2. the samples are LITTLE-endian in memory -------------------------- */

static void test_memory_byte_order(void)
{
	static const uint8_t bgr[3] = { 0x00u, 0x00u, 0xFFu };  /* pure red */
	uint16_t out[1];
	uint8_t  bytes[2];

	cam_bgr_planar_to_rgb565(bgr, out, 1u);
	memcpy(bytes, out, sizeof bytes);

	/*
	 * The pipeline's FRAME_FMT_RGB565 is documented little-endian, and the
	 * ST7789 wants the opposite on the wire.  Which end owns that swap is a
	 * decision, not an accident: the driver does it (lcd_blit_le), so a slot
	 * never holds bytes in an order its format tag denies.  If this
	 * assertion ever has to be relaxed, the format tag has to change with
	 * it.
	 *
	 * On a little-endian host, 0xF800 stored as a uint16_t reads back 00 F8.
	 */
	CHECK(bytes[0] == 0x00u && bytes[1] == 0xF8u,
	      "pure red is %02X %02X in memory, wanted 00 F8 (little-endian)",
	      bytes[0], bytes[1]);
}

/* ---- 3. truncation is exact at the ends of the range --------------------- */

static void test_quantisation(void)
{
	static const uint8_t bgr[3] = { 0xFFu, 0xFFu, 0xFFu };  /* white */
	static const uint8_t blk[3] = { 0x00u, 0x00u, 0x00u };  /* black */
	static const uint8_t mid[3] = { 0x80u, 0x80u, 0x80u };
	uint16_t out[1];

	cam_bgr_planar_to_rgb565(bgr, out, 1u);
	CHECK(out[0] == 0xFFFFu, "white packed as %04X, wanted FFFF", out[0]);

	cam_bgr_planar_to_rgb565(blk, out, 1u);
	CHECK(out[0] == 0x0000u, "black packed as %04X, wanted 0000", out[0]);

	/* 0x80 >> 3 == 0x10, 0x80 >> 2 == 0x20: 10000b / 100000b / 10000b. */
	cam_bgr_planar_to_rgb565(mid, out, 1u);
	CHECK(out[0] == ((0x10u << 11) | (0x20u << 5) | 0x10u),
	      "mid grey packed as %04X, wanted 8410", out[0]);
}

/* ---- 4. a full frame: every pixel written, nothing past the end ---------- */

#define FRAME_W 320u
#define FRAME_H 240u
#define FRAME_PX (FRAME_W * FRAME_H)

static uint8_t  frame_bgr[FRAME_PX * 3u];
static uint16_t frame_out[FRAME_PX + 2u];

static void test_full_frame(void)
{
	uint32_t i;
	int mismatch = -1;

	for (i = 0u; i < FRAME_PX; i++) {
		frame_bgr[i]                 = (uint8_t)(i & 0xFFu);
		frame_bgr[FRAME_PX + i]      = (uint8_t)((i >> 3) & 0xFFu);
		frame_bgr[2u * FRAME_PX + i] = (uint8_t)((i >> 5) & 0xFFu);
	}
	memset(frame_out, 0xAA, sizeof frame_out);

	cam_bgr_planar_to_rgb565(frame_bgr, frame_out, FRAME_PX);

	for (i = 0u; i < FRAME_PX; i++) {
		uint16_t want = (uint16_t)
			(((uint16_t)(frame_bgr[2u * FRAME_PX + i] >> 3) << 11) |
			 ((uint16_t)(frame_bgr[FRAME_PX + i] >> 2) << 5) |
			  (uint16_t)(frame_bgr[i] >> 3));

		if (frame_out[i] != want) {
			mismatch = (int)i;
			break;
		}
	}
	CHECK(mismatch < 0, "pixel %d differs (%04X)", mismatch,
	      mismatch < 0 ? 0 : frame_out[mismatch]);

	/* The two guard samples past the frame must be untouched: a loop that
	 * runs one pixel long here is 2 bytes into whatever the linker put
	 * next, and on the board that is the other pipeline slot. */
	CHECK(frame_out[FRAME_PX] == 0xAAAAu &&
	      frame_out[FRAME_PX + 1u] == 0xAAAAu,
	      "the packer wrote past the end of the frame");
}

/* ---- 5. plane statistics ------------------------------------------------- */

static void test_plane_stats(void)
{
	static const uint8_t ramp[4] = { 0u, 10u, 20u, 30u };
	static const uint8_t flat[4] = { 7u, 7u, 7u, 7u };
	struct cam_plane_stats st;

	cam_plane_stats(ramp, 4u, &st);
	CHECK(st.min == 0u && st.max == 30u, "ramp min/max %u/%u, wanted 0/30",
	      st.min, st.max);
	CHECK(st.mean_x100 == 1500u, "ramp mean_x100 %lu, wanted 1500",
	      (unsigned long)st.mean_x100);

	cam_plane_stats(flat, 4u, &st);
	CHECK(st.min == 7u && st.max == 7u && st.mean_x100 == 700u,
	      "flat plane reported %u/%u/%lu", st.min, st.max,
	      (unsigned long)st.mean_x100);

	/* Empty must read as empty, not as a plausible dark frame. */
	cam_plane_stats(ramp, 0u, &st);
	CHECK(st.min == 255u && st.max == 0u && st.mean_x100 == 0u,
	      "an empty plane reported %u/%u/%lu instead of the empty marker",
	      st.min, st.max, (unsigned long)st.mean_x100);

	/* A saturated full-size plane is where a 32-bit sum would overflow if
	 * the *100 scaling were applied to the accumulator instead of the sum
	 * (320*240*255*100 = 1.96e9, which fits -- but only just). */
	memset(frame_bgr, 0xFFu, FRAME_PX);
	cam_plane_stats(frame_bgr, FRAME_PX, &st);
	CHECK(st.min == 255u && st.max == 255u && st.mean_x100 == 25500u,
	      "a saturated 320x240 plane reported mean_x100 %lu, wanted 25500",
	      (unsigned long)st.mean_x100);
}

/* ---- 6. the row seam metric --------------------------------------------- */

static void test_row_seam(void)
{
	uint8_t plane[4u * 4u];
	uint32_t row = 99u;
	uint32_t seam;
	uint32_t y, x;

	/* Uniform: no seam anywhere. */
	memset(plane, 100u, sizeof plane);
	seam = cam_plane_row_seam_x100(plane, 4u, 4u, &row);
	CHECK(seam == 0u, "a uniform plane reported a seam of %lu",
	      (unsigned long)seam);

	/* A step of 50 between row 1 and row 2 -- an exposure change landing
	 * mid-readout. */
	for (y = 0u; y < 4u; y++)
		for (x = 0u; x < 4u; x++)
			plane[y * 4u + x] = (y < 2u) ? 100u : 150u;
	seam = cam_plane_row_seam_x100(plane, 4u, 4u, &row);
	CHECK(seam == 5000u, "the seam is %lu, wanted 5000",
	      (unsigned long)seam);
	CHECK(row == 2u, "the seam is at row %lu, wanted 2",
	      (unsigned long)row);

	/* Fewer than two rows: there is no adjacent pair to compare. */
	seam = cam_plane_row_seam_x100(plane, 4u, 1u, &row);
	CHECK(seam == 0u && row == 0u,
	      "a single-row plane reported seam %lu at row %lu",
	      (unsigned long)seam, (unsigned long)row);
}

/* ---- 7. the white balance ------------------------------------------------ */

/*
 * The gains exist because the IMX219 has no per-channel ones and an
 * uncorrected Bayer frame is green-heavy (measured on hardware: R 58 / G 66 /
 * B 54).  Two things have to hold, and the second is the one that bites: unity
 * must be exactly a no-op, so an unbalanced preview is still evidence about the
 * sensor; and a gain that overflows must SATURATE, because a wrapped highlight
 * is not a bright spot, it is a black one surrounded by colour noise.
 */
static void test_white_balance(void)
{
	static const uint8_t bgr[3] = { 100u, 100u, 100u };
	struct cam_wb wb;
	uint16_t plain[1], out[1];

	/* Unity is bit-for-bit the plain packer. */
	wb.r = CAM_WB_UNITY;
	wb.g = CAM_WB_UNITY;
	wb.b = CAM_WB_UNITY;
	wb.black = 0u;
	wb.gamma = 0u;
	wb.sat = CAM_SAT_UNITY;
	cam_bgr_planar_to_rgb565(bgr, plain, 1u);
	cam_bgr_planar_to_rgb565_wb(bgr, out, 1u, &wb);
	CHECK(out[0] == plain[0],
	      "unity gains changed the pixel: %04X vs %04X", out[0], plain[0]);

	/* NULL means unity too -- the plain entry point is that call. */
	cam_bgr_planar_to_rgb565_wb(bgr, out, 1u, NULL);
	CHECK(out[0] == plain[0], "a NULL white balance was not unity");

	/* Halving green: 100 * 128/256 = 50 -> 50 >> 2 == 12 in the 6-bit
	 * field, where unity gave 100 >> 2 == 25. */
	wb.r = CAM_WB_UNITY;
	wb.black = 0u;
	wb.g = CAM_WB_UNITY / 2u;
	cam_bgr_planar_to_rgb565_wb(bgr, out, 1u, &wb);
	CHECK(((out[0] >> 5) & 0x3Fu) == 12u,
	      "green at half gain landed on %u, wanted 12",
	      (unsigned)((out[0] >> 5) & 0x3Fu));
	/* ...and left the other two alone. */
	CHECK((out[0] & 0x1Fu) == (plain[0] & 0x1Fu),
	      "the green gain moved blue");
	CHECK((out[0] >> 11) == (plain[0] >> 11),
	      "the green gain moved red");

	/* Saturation: 200 * 4x = 800, which must clamp to 255 and not wrap to
	 * 32.  A wrapped highlight reads as a dark speckle in a bright area --
	 * the exact artefact that gets blamed on the sensor. */
	{
		static const uint8_t bright[3] = { 200u, 200u, 200u };

		wb.r = CAM_WB_UNITY * 4u;
		wb.g = CAM_WB_UNITY * 4u;
		wb.b = CAM_WB_UNITY * 4u;
		wb.black = 0u;
		cam_bgr_planar_to_rgb565_wb(bright, out, 1u, &wb);
		CHECK(out[0] == 0xFFFFu,
		      "a 4x gain on 200 gave %04X, wanted FFFF (it wrapped)",
		      out[0]);
	}

	/* Black level: subtracted BEFORE the gain, and clamped at zero so a
	 * pedestal larger than the sample cannot wrap to white -- which is the
	 * failure that would turn shadows into snow. */
	{
		static const uint8_t dark[3] = { 10u, 20u, 30u };

		wb.r = CAM_WB_UNITY;
		wb.g = CAM_WB_UNITY;
		wb.b = CAM_WB_UNITY;
		wb.black = 16u;
		wb.gamma = 0u;
	wb.sat = CAM_SAT_UNITY;
		cam_bgr_planar_to_rgb565_wb(dark, out, 1u, &wb);
		/* B 10-16 -> 0, G 20-16 -> 4, R 30-16 -> 14. */
		CHECK((out[0] & 0x1Fu) == 0u,
		      "blue below the black level did not clamp to 0");
		CHECK(((out[0] >> 5) & 0x3Fu) == (4u >> 2),
		      "green did not have the black level subtracted");
		CHECK((out[0] >> 11) == (14u >> 3),
		      "red did not have the black level subtracted");

		/* Order matters: subtract THEN scale.  With a 2x gain,
		 * (30-16)*2 = 28, not 30*2-16 = 44. */
		wb.r = CAM_WB_UNITY * 2u;
		cam_bgr_planar_to_rgb565_wb(dark, out, 1u, &wb);
		CHECK((out[0] >> 11) == (28u >> 3),
		      "the gain was applied before the black subtraction");
	}

	/* Zero gain is a legal request and must produce black, not garbage. */
	wb.r = 0u;
	wb.g = 0u;
	wb.b = 0u;
	cam_bgr_planar_to_rgb565_wb(bgr, out, 1u, &wb);
	CHECK(out[0] == 0x0000u, "zero gains gave %04X, wanted 0000", out[0]);
}

/* ---- 8. the mosaic metric ------------------------------------------------ */

/*
 * This one exists to settle a question that otherwise gets settled by squinting
 * at a photograph: is WDMA3 handing us demosaiced colour planes, or raw Bayer?
 * The two look similar on the glass -- flat and oddly coloured either way -- so
 * the metric has to be sharp, and its two ends are pinned here against
 * synthesised planes rather than against a scene.
 */
static void test_mosaic_metric(void)
{
	static uint8_t plane[64u * 64u];
	uint32_t x, y;

	/* A demosaiced plane: one channel, interpolated.  Give it real image
	 * structure (a gradient) so the test cannot pass merely because the
	 * input is flat -- what must be near zero is the 2x2 PHASE difference,
	 * not the variance. */
	for (y = 0u; y < 64u; y++)
		for (x = 0u; x < 64u; x++)
			plane[y * 64u + x] = (uint8_t)(x + y);
	/*
	 * A linear ramp is the WORST case for a smooth plane, and not zero: its
	 * four phase means are offset by the ramp's own slope, here 2 counts
	 * (200 in hundredths).  The point is the separation -- the mosaic below
	 * reads 60x higher -- not that a real image reads exactly zero.
	 */
	CHECK(cam_plane_mosaic_x100(plane, 64u, 64u) <= 300u,
	      "a smooth gradient reported mosaic %lu (a ramp's own slope is "
	      "about 200; anything much above that is not phase-flat)",
	      (unsigned long)cam_plane_mosaic_x100(plane, 64u, 64u));

	/* A raw BGGR mosaic: B at (even,even), G at the two off-diagonals, R at
	 * (odd,odd), with the channel levels a coloured scene would give. */
	for (y = 0u; y < 64u; y++) {
		for (x = 0u; x < 64u; x++) {
			uint8_t v;

			if ((y & 1u) == 0u && (x & 1u) == 0u)
				v = 60u;               /* B */
			else if ((y & 1u) == 1u && (x & 1u) == 1u)
				v = 180u;              /* R */
			else
				v = 120u;              /* G */
			plane[y * 64u + x] = v;
		}
	}
	CHECK(cam_plane_mosaic_x100(plane, 64u, 64u) >= 10000u,
	      "a raw BGGR mosaic reported only %lu; the metric cannot tell "
	      "demosaiced from raw",
	      (unsigned long)cam_plane_mosaic_x100(plane, 64u, 64u));

	/* Degenerate sizes are not an answer. */
	CHECK(cam_plane_mosaic_x100(plane, 1u, 64u) == 0u,
	      "a one-pixel-wide plane produced a mosaic figure");
	CHECK(cam_plane_mosaic_x100(NULL, 64u, 64u) == 0u,
	      "a NULL plane produced a mosaic figure");
}

/* ---- 9. the colour-separation metric ------------------------------------- */

/*
 * This is the one that tells a correct Bayer phase from a wrong one, so what it
 * has to do is separate "the channels genuinely differ" from "the channels have
 * all been dragged together", which is what a wrong phase does.
 */
static void test_colour_metric(void)
{
	static uint8_t f[3u * 16u];
	uint32_t i;

	/* Grey: no separation at all, whatever the phase.  This is the case
	 * that makes a low score MEANINGLESS -- worth pinning so nobody reads a
	 * grey-scene measurement as evidence about the phase. */
	for (i = 0u; i < 16u; i++) {
		f[i] = 100u;            /* B */
		f[16u + i] = 100u;      /* G */
		f[32u + i] = 100u;      /* R */
	}
	CHECK(cam_frame_colour_x100(f, 16u) == 0u,
	      "a grey frame reported colour %lu, wanted 0",
	      (unsigned long)cam_frame_colour_x100(f, 16u));

	/* Strongly coloured: max-min is 150 per pixel. */
	for (i = 0u; i < 16u; i++) {
		f[i] = 30u;             /* B */
		f[16u + i] = 90u;       /* G */
		f[32u + i] = 180u;      /* R */
	}
	CHECK(cam_frame_colour_x100(f, 16u) == 15000u,
	      "a saturated frame reported colour %lu, wanted 15000",
	      (unsigned long)cam_frame_colour_x100(f, 16u));

	/* The wrong-phase shape: R and B dragged toward G.  Must score much
	 * lower than the same scene resolved correctly. */
	for (i = 0u; i < 16u; i++) {
		f[i] = 85u;
		f[16u + i] = 90u;
		f[32u + i] = 95u;
	}
	CHECK(cam_frame_colour_x100(f, 16u) < 2000u,
	      "a desaturated frame reported colour %lu; the metric cannot "
	      "separate a wrong phase from a right one",
	      (unsigned long)cam_frame_colour_x100(f, 16u));

	CHECK(cam_frame_colour_x100(f, 0u) == 0u,
	      "an empty frame produced a colour figure");
	CHECK(cam_frame_colour_x100(NULL, 16u) == 0u,
	      "a NULL frame produced a colour figure");
}

/* ---- 10. reading the Bayer phase off raw mosaic data --------------------- */

/*
 * The automation behind `camera raw`.  What it has to get right is that the two
 * GREEN positions come out highest -- everything downstream of that (which
 * diagonal, therefore which phase family) is arithmetic on the answer.
 */
static void test_bayer_phase_means(void)
{
	static uint8_t plane[64u * 64u];
	uint32_t m[4];
	uint32_t x, y;

	/* An RGGB tile: R at (0,0), G at (1,0) and (0,1), B at (1,1), with
	 * green the brightest as it is in almost any scene. */
	for (y = 0u; y < 64u; y++) {
		for (x = 0u; x < 64u; x++) {
			uint8_t v;

			if ((y & 1u) == 0u && (x & 1u) == 0u)
				v = 80u;                /* R */
			else if ((y & 1u) == 1u && (x & 1u) == 1u)
				v = 50u;                /* B */
			else
				v = 140u;               /* G */
			plane[y * 64u + x] = v;
		}
	}
	cam_bayer_phase_means_x100(plane, 64u, 64u, m);

	CHECK(m[0] == 8000u, "(0,0) mean %lu, wanted 8000",
	      (unsigned long)m[0]);
	CHECK(m[1] == 14000u && m[2] == 14000u,
	      "the two greens read %lu and %lu, wanted 14000 each",
	      (unsigned long)m[1], (unsigned long)m[2]);
	CHECK(m[3] == 5000u, "(1,1) mean %lu, wanted 5000",
	      (unsigned long)m[3]);

	/* The greens must be the two HIGHEST -- that is the property the
	 * phase-naming depends on. */
	CHECK(m[1] > m[0] && m[1] > m[3] && m[2] > m[0] && m[2] > m[3],
	      "the greens are not the two highest positions");

	/* Degenerate input must not produce a confident answer. */
	cam_bayer_phase_means_x100(NULL, 64u, 64u, m);
	CHECK(m[0] == 0u && m[1] == 0u && m[2] == 0u && m[3] == 0u,
	      "a NULL plane produced phase means");
}

/* ---- 11. the sRGB encode ------------------------------------------------- */

/*
 * The curve is what makes a linear sensor frame look like a picture rather than
 * a dark one, so what has to hold is that it is a real transfer function --
 * monotonic, anchored at both ends -- and that it is applied in the right
 * ORDER.  Order is the subtle one: black level and gain correct a linear
 * measurement, so encoding before them would turn the gain into a contrast
 * control and the black level into a crush.
 */
static void test_gamma(void)
{
	static const uint8_t mid[3] = { 49u, 49u, 49u };
	struct cam_wb wb;
	uint16_t lin[1], enc[1];
	uint32_t i;
	uint8_t prev;

	wb.r = CAM_WB_UNITY;
	wb.g = CAM_WB_UNITY;
	wb.b = CAM_WB_UNITY;
	wb.black = 0u;
	wb.sat = CAM_SAT_UNITY;

	wb.gamma = 0u;
	cam_bgr_planar_to_rgb565_wb(mid, lin, 1u, &wb);
	wb.gamma = 1u;
	cam_bgr_planar_to_rgb565_wb(mid, enc, 1u, &wb);

	/* 49 linear is an ordinary midtone and must come out far brighter --
	 * this is the whole reason the curve exists. */
	CHECK((enc[0] >> 11) > (lin[0] >> 11) + 8u,
	      "sRGB encoding barely moved a midtone: red %u -> %u (5-bit)",
	      (unsigned)(lin[0] >> 11), (unsigned)(enc[0] >> 11));

	/* Endpoints are fixed points: black stays black, white stays white. */
	{
		static const uint8_t blk[3] = { 0u, 0u, 0u };
		static const uint8_t wht[3] = { 255u, 255u, 255u };

		cam_bgr_planar_to_rgb565_wb(blk, enc, 1u, &wb);
		CHECK(enc[0] == 0x0000u, "gamma moved black off zero (%04X)",
		      enc[0]);
		cam_bgr_planar_to_rgb565_wb(wht, enc, 1u, &wb);
		CHECK(enc[0] == 0xFFFFu, "gamma moved white off full (%04X)",
		      enc[0]);
	}

	/* Monotonic: a curve that folded back would invert shading somewhere. */
	prev = 0u;
	for (i = 0u; i < 256u; i++) {
		uint8_t one[3];
		uint8_t got;

		one[0] = (uint8_t)i;
		one[1] = (uint8_t)i;
		one[2] = (uint8_t)i;
		cam_bgr_planar_to_rgb565_wb(one, enc, 1u, &wb);
		got = (uint8_t)((enc[0] >> 5) & 0x3Fu);
		CHECK(got >= prev, "the curve is not monotonic at %lu",
		      (unsigned long)i);
		prev = got;
	}

	/* ORDER: black subtract, then gain, then encode.  With black 16 and a
	 * 2x gain on a sample of 30, linear gives (30-16)*2 = 28; the encoded
	 * answer must be the curve OF 28, not 28 applied to a curved input. */
	{
		static const uint8_t dark[3] = { 30u, 30u, 30u };
		uint16_t a[1], b[1];

		wb.black = 16u;
		wb.r = CAM_WB_UNITY * 2u;
		wb.gamma = 0u;
	wb.sat = CAM_SAT_UNITY;
		cam_bgr_planar_to_rgb565_wb(dark, a, 1u, &wb);
		CHECK((a[0] >> 11) == (28u >> 3), "the linear reference moved");

		wb.gamma = 1u;
		cam_bgr_planar_to_rgb565_wb(dark, b, 1u, &wb);
		CHECK((b[0] >> 11) > (a[0] >> 11),
		      "encoding after the gain did not brighten the result");
	}
}

/* ---- 12. saturation ------------------------------------------------------ */

/*
 * Standing in for a colour correction matrix, so what it must do is widen the
 * gap between the channels WITHOUT moving the pixel's brightness -- otherwise
 * it is a contrast control wearing the wrong name, and it fights the exposure
 * loop.  And it must not wrap: a channel pushed past the ends is a bright pixel
 * turning dark, which is the ugliest artefact in the whole chain.
 */
static void test_saturation(void)
{
	static const uint8_t px[3] = { 40u, 80u, 120u };   /* B, G, R */
	struct cam_wb wb;
	uint16_t lo[1], hi[1];

	wb.r = CAM_WB_UNITY;
	wb.g = CAM_WB_UNITY;
	wb.b = CAM_WB_UNITY;
	wb.black = 0u;
	wb.gamma = 0u;

	wb.sat = CAM_SAT_UNITY;
	cam_bgr_planar_to_rgb565_wb(px, lo, 1u, &wb);
	wb.sat = CAM_SAT_UNITY * 2u;
	cam_bgr_planar_to_rgb565_wb(px, hi, 1u, &wb);

	/* Wider: red was above the luma and must rise, blue below it and fall. */
	CHECK((hi[0] >> 11) > (lo[0] >> 11),
	      "the channel above luma did not rise (%u -> %u)",
	      (unsigned)(lo[0] >> 11), (unsigned)(hi[0] >> 11));
	CHECK((hi[0] & 0x1Fu) < (lo[0] & 0x1Fu),
	      "the channel below luma did not fall (%u -> %u)",
	      (unsigned)(lo[0] & 0x1Fu), (unsigned)(hi[0] & 0x1Fu));

	/* Unity really is a no-op, so an unsaturated preview is evidence about
	 * the sensor and not about this. */
	{
		uint16_t plain[1];

		wb.sat = CAM_SAT_UNITY;
		cam_bgr_planar_to_rgb565(px, plain, 1u);
		cam_bgr_planar_to_rgb565_wb(px, lo, 1u, &wb);
		CHECK(lo[0] == plain[0], "unity saturation changed the pixel");
	}

	/* Grey stays grey at any saturation -- there is no colour to widen, and
	 * a stage that tinted neutrals would be visible on every wall. */
	{
		static const uint8_t grey[3] = { 100u, 100u, 100u };

		wb.sat = CAM_SAT_UNITY * 4u;
		cam_bgr_planar_to_rgb565_wb(grey, hi, 1u, &wb);
		CHECK((hi[0] >> 11) == (100u >> 3) &&
		      ((hi[0] >> 5) & 0x3Fu) == (100u >> 2) &&
		      (hi[0] & 0x1Fu) == (100u >> 3),
		      "saturation tinted a neutral pixel (%04X)", hi[0]);
	}

	/* Extreme gain must clamp, not wrap. */
	{
		static const uint8_t strong[3] = { 10u, 128u, 250u };

		wb.sat = 2048u;
		cam_bgr_planar_to_rgb565_wb(strong, hi, 1u, &wb);
		CHECK((hi[0] >> 11) == 0x1Fu,
		      "a strongly boosted channel did not clamp to full (%04X)",
		      hi[0]);
		CHECK((hi[0] & 0x1Fu) == 0u,
		      "a strongly cut channel did not clamp to zero (%04X)",
		      hi[0]);
	}
}

/* ---- 13. the tone tables against the arithmetic they replaced ------------ */

/*
 * WHY A SECOND COPY OF THE PIPELINE LIVES HERE (issue #58).
 *
 * The converter used to do the black level, the gain, the curve and the 5/6-bit
 * truncation as arithmetic on every one of 76,800 pixels; it now compiles them
 * into per-channel tables and the inner loop is loads.  The claim that change
 * rests on is EQUIVALENCE -- the same frame in, the same 16-bit words out, for
 * every input and every setting -- and nothing else in the project can check it.
 * On the panel the two are indistinguishable by construction when they agree,
 * and when they disagree by a count or two they are STILL indistinguishable,
 * which is the dangerous half: a table built with an off-by-one somewhere in the
 * dark end would ship looking perfect.
 *
 * So the OLD implementation is transcribed below, exactly as it stood at
 * 9ee4c5b, and the two are swept against each other.  The duplication is the
 * point, the same way test_cam_mipi_calc.c keeps the SDK's original floating
 * point formula: this file is not trying to avoid drifting from cam_convert.c,
 * it is trying to notice.  Its sRGB table is a copy for that reason too -- if
 * the shipped one is ever edited, these tests must fail until somebody says why.
 *
 * The thing the sweeps are really hunting is the STALE TABLE.  Tables are built
 * from the settings and reused while the settings hold, so a key that does not
 * cover every field would answer a `camera sat` or a `camera black` with the
 * previous frame's tables -- a picture that ignores a typed command, which looks
 * like the command being broken and not like a cache.  Hence cases that differ
 * in exactly one field, and hence the alternation.
 */

static const uint8_t ref_srgb[256] = {
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
};

static uint8_t ref_gain8(uint8_t v, uint16_t gain, uint8_t black)
{
	uint32_t s;

	v = (v > black) ? (uint8_t)(v - black) : 0u;
	s = ((uint32_t)v * gain) >> 8;
	if (s > 255u)
		s = 255u;
	return (uint8_t)s;
}

static uint8_t ref_sat8(uint8_t v, uint32_t y, uint16_t sat)
{
	int32_t d = ((int32_t)v - (int32_t)y) * (int32_t)sat / 256;
	int32_t s = (int32_t)y + d;

	if (s < 0)
		return 0u;
	if (s > 255)
		return 255u;
	return (uint8_t)s;
}

static uint16_t ref_pixel(uint8_t b, uint8_t g, uint8_t r,
                          const struct cam_wb *wb)
{
	uint8_t rv = ref_gain8(r, wb->r, wb->black);
	uint8_t gv = ref_gain8(g, wb->g, wb->black);
	uint8_t bv = ref_gain8(b, wb->b, wb->black);

	if (wb->sat != CAM_SAT_UNITY && wb->sat != 0u) {
		uint32_t y = ((uint32_t)rv + 2u * gv + bv) / 4u;

		rv = ref_sat8(rv, y, wb->sat);
		gv = ref_sat8(gv, y, wb->sat);
		bv = ref_sat8(bv, y, wb->sat);
	}
	if (wb->gamma) {
		rv = ref_srgb[rv];
		gv = ref_srgb[gv];
		bv = ref_srgb[bv];
	}
	return (uint16_t)(((uint16_t)(rv >> 3) << 11) |
	                  ((uint16_t)(gv >> 2) << 5) |
	                   (uint16_t)(bv >> 3));
}

static uint8_t  ref_planes[3u * 256u];
static uint16_t ref_got[256u];

/* One batch through the real converter, every pixel of it against the
 * transcription.  Stops at the first disagreement: a table that is wrong is
 * wrong for millions of pixels, and sixteen million identical FAIL lines hide
 * the one piece of information in them, which is the FIRST input that broke. */
static int ref_batch(const struct cam_wb *wb, const uint8_t *b,
                     const uint8_t *g, const uint8_t *r, uint32_t n,
                     const char *what)
{
	uint32_t i;

	memcpy(ref_planes, b, n);
	memcpy(ref_planes + n, g, n);
	memcpy(ref_planes + 2u * n, r, n);
	cam_bgr_planar_to_rgb565_wb(ref_planes, ref_got, n, wb);

	for (i = 0u; i < n; i++) {
		uint16_t want = ref_pixel(b[i], g[i], r[i], wb);

		if (ref_got[i] != want) {
			CHECK(0, "%s: B%u G%u R%u packed as %04X, the "
			      "arithmetic it replaced says %04X "
			      "(wb %u/%u/%u black %u gamma %u sat %u)",
			      what, (unsigned)b[i], (unsigned)g[i],
			      (unsigned)r[i], ref_got[i], want,
			      (unsigned)wb->r, (unsigned)wb->g, (unsigned)wb->b,
			      (unsigned)wb->black, (unsigned)wb->gamma,
			      (unsigned)wb->sat);
			return 1;
		}
	}
	return 0;
}

/*
 * Every value of every channel, with the other two held at black, mid and
 * white.  This is the sweep that guarantees EVERY ENTRY of every table is read
 * back at least once for this setting -- a single wrong entry is a single wrong
 * shade, which no photograph would ever convict.
 */
static void sweep_axes(const struct cam_wb *wb, const char *what)
{
	static const uint8_t levels[3] = { 0u, 128u, 255u };
	uint8_t b[256], g[256], r[256];
	uint32_t ch, lv, i;

	for (ch = 0u; ch < 3u; ch++)
		for (lv = 0u; lv < 3u; lv++) {
			for (i = 0u; i < 256u; i++) {
				b[i] = (ch == 0u) ? (uint8_t)i : levels[lv];
				g[i] = (ch == 1u) ? (uint8_t)i : levels[lv];
				r[i] = (ch == 2u) ? (uint8_t)i : levels[lv];
			}
			if (ref_batch(wb, b, g, r, 256u, what) != 0)
				return;
		}
}

/*
 * Colour TRIPLES on a grid.  The axes above cannot catch saturation: it pulls a
 * channel away from the pixel's own luma, so its answer depends on the other
 * two channels, and the folded form reaches it through a table indexed by the
 * difference.  Only combinations exercise that.
 */
static void sweep_grid(const struct cam_wb *wb, uint32_t step, const char *what)
{
	uint8_t b[256], g[256], r[256];
	uint32_t rv, gv, bv, n;

	for (rv = 0u; rv < 256u; rv += step)
		for (gv = 0u; gv < 256u; gv += step) {
			n = 0u;
			for (bv = 0u; bv < 256u; bv += step) {
				b[n] = (uint8_t)bv;
				g[n] = (uint8_t)gv;
				r[n] = (uint8_t)rv;
				n++;
			}
			if (ref_batch(wb, b, g, r, n, what) != 0)
				return;
		}
}

/*
 * Case 0 is what the firmware boots with (camera.c).  Cases 1..6 differ from it
 * in EXACTLY ONE field each, which is what makes them a test of the table key
 * rather than of the arithmetic; the rest are the ends of every range the
 * console will accept.
 */
static const struct cam_wb tone_cases[] = {
	{ 256u, 256u, 256u,  16u, 1u,  600u },   /* the shipped default */
	{ 512u, 256u, 256u,  16u, 1u,  600u },   /* red gain alone */
	{ 256u, 320u, 256u,  16u, 1u,  600u },   /* green gain alone */
	{ 256u, 256u, 400u,  16u, 1u,  600u },   /* blue gain alone */
	{ 256u, 256u, 256u,   0u, 1u,  600u },   /* black level alone */
	{ 256u, 256u, 256u,  16u, 0u,  600u },   /* gamma alone */
	{ 256u, 256u, 256u,  16u, 1u,  256u },   /* saturation alone */
	{ 256u, 256u, 256u,   0u, 0u,  256u },   /* the identity */
	{ 256u, 256u, 256u,  16u, 1u,    0u },   /* sat 0 also means unity */
	{   0u,   0u,   0u,   0u, 1u,  600u },   /* zero gains: black frame */
	{ 4096u, 4096u, 4096u, 0u, 1u, 600u },   /* the console's ceiling */
	{ 256u, 256u, 256u, 255u, 1u,  600u },   /* a pedestal above every
	                                          * sample: also black */
	{ 256u, 256u, 256u,  16u, 1u, 2048u },   /* saturation ceiling */
	{ 256u, 256u, 256u,  16u, 1u,    1u },   /* very nearly greyscale */
	{ 300u, 256u, 380u,  16u, 0u, 1024u },   /* an AWB-shaped pair, no
	                                          * curve... */
	{ 300u, 256u, 380u,  16u, 1u, 1024u },   /* ...and with it */
};

static void test_tone_tables(void)
{
	const uint32_t n = (uint32_t)(sizeof tone_cases / sizeof tone_cases[0]);
	uint32_t i;

	for (i = 0u; i < n; i++) {
		sweep_axes(&tone_cases[i], "axes");
		sweep_grid(&tone_cases[i], 8u, "grid");
	}

	/*
	 * Back to the default between every case: a stale table answers with
	 * the PREVIOUS settings, so a key that ignores one field only shows up
	 * when that field is the one that changed and something else changed
	 * back.  Running the list once in order would let such a key pass on
	 * half the cases by luck.
	 */
	for (i = 1u; i < n; i++) {
		sweep_axes(&tone_cases[0], "alternation: back to the default");
		sweep_axes(&tone_cases[i], "alternation: the variant");
	}

	/* And the setting the board actually runs in, over every colour there
	 * is -- 16.7 million of them, because this is the one case where "we
	 * sampled it" is not the same claim as "it is equivalent". */
	sweep_grid(&tone_cases[0], 1u, "the default, exhaustively");
}

int main(void)
{
	test_channel_order();
	test_memory_byte_order();
	test_quantisation();
	test_full_frame();
	test_plane_stats();
	test_row_seam();
	test_white_balance();
	test_mosaic_metric();
	test_colour_metric();
	test_bayer_phase_means();
	test_gamma();
	test_saturation();
	test_tone_tables();

	if (failures != 0) {
		printf("test_cam_convert: %d failure(s)\n", failures);
		return 1;
	}
	printf("test_cam_convert: OK\n");
	return 0;
}
