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

	if (failures != 0) {
		printf("test_cam_convert: %d failure(s)\n", failures);
		return 1;
	}
	printf("test_cam_convert: OK\n");
	return 0;
}
