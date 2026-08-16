/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Planar B/G/R -> RGB565 packing and per-plane statistics (issue #35).
 * See cam_convert.h for why this is a translation unit of its own.
 */
#include <stddef.h>
#include <string.h>

#include "cam_convert.h"

/*
 * sRGB encode: linear 8-bit in, display-encoded 8-bit out.
 *
 * WHY THE PIPELINE NEEDS THIS AT ALL.  A camera sensor measures light, so its
 * output is LINEAR in luminance.  Displays -- this panel included, which is why
 * its init table programs gamma curves -- expect the opposite: values already
 * encoded with roughly a 1/2.2 power law, because human vision is far more
 * sensitive to differences in the dark end than in the bright.
 *
 * Sending linear samples to a display that expects encoded ones does not fail;
 * it produces a picture that is dark and flat, with the midtones crushed toward
 * black.  Measured on this board, a perfectly ordinary indoor scene averaged 49
 * -- which as a LINEAR value is a normal midtone, and as a display value is
 * nearly black.  Through this table it becomes 121, and the histogram lands
 * where the eye expects it.
 *
 * Generated from the standard sRGB transfer function:
 *     S = 12.92 * L                     for L <= 0.0031308
 *     S = 1.055 * L^(1/2.4) - 0.055     otherwise
 * with L = i/255, rounded to nearest.  256 bytes of .rodata, no runtime
 * arithmetic and no libm -- which this image does not link.
 */
static const uint8_t cam_srgb_encode[256] = {
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

/* (v - black) * gain/256: pedestal off first, then scale, saturating at 255.
 *
 * Applied in LINEAR light and without the encode.  Black level and gain are
 * corrections to a linear measurement and only mean what they say there;
 * encoding first would make the gain a contrast control and the black level a
 * crush.  The curve comes last, after saturation, which is linear-light too. */
static uint8_t cam_gain8(uint8_t v, uint16_t gain, uint8_t black)
{
	uint32_t s;

	v = (v > black) ? (uint8_t)(v - black) : 0u;
	s = ((uint32_t)v * gain) >> 8;
	if (s > 255u)
		s = 255u;
	return (uint8_t)s;
}

/* ---- the tone tables ----------------------------------------------------- */

/*
 * WHY THERE ARE TABLES HERE AT ALL (issue #58).
 *
 * Nearly every step of this pipeline is a function of ONE 8-bit number: the
 * black level, the per-channel gain, the sRGB encode and the 5/6-bit truncation
 * each depend only on the sample being converted.  A function of one 8-bit
 * number is a 256-entry table, and the frame is 76,800 pixels -- so doing the
 * arithmetic per pixel means computing the same 256 answers three hundred times
 * over.  Measured on hardware before this change, `pack` cost 17.2 ms per frame
 * -- and since issue #57 moved the LCD blit onto its own thread, that is the
 * largest single thing the producer does.  It costs twice over: the epic that
 * wants 30 fps (issue #56) budgets it directly, and the panel thread sits below
 * the producer, so a blit that finishes its DMA mid-pack waits out whatever is
 * left of it before it can present -- which is how frames get dropped at a
 * tuned VTS (issue #64).
 *
 * So the settings are compiled into tables once, and the inner loop is loads.
 * What CANNOT fold is saturation: it pulls a channel away from the pixel's own
 * luma, so it depends on the other two channels as well and is not a function
 * of one number.  Its multiply-and-divide still folds -- that part depends only
 * on the DIFFERENCE from the luma, which is one number again (cam_tone.spread).
 *
 * THE TABLES ARE REBUILT FROM THE SETTINGS THEMSELVES, not on request.  The
 * white balance moves under the automatic loop every few frames, and a "tell
 * the converter when it changed" contract has exactly one failure mode -- a
 * caller that forgets -- whose symptom is a picture that ignores a typed
 * command until something unrelated happens to invalidate the cache.  Comparing
 * against the settings the tables were built from cannot go stale, and the
 * comparison is ten bytes against a 76,800-pixel loop.
 *
 * Rebuilding costs 2,047 entries where the loop it serves costs 230,400, and it
 * happens INSIDE the interval `camera stats` reports as `pack`, so the number
 * that reports the saving already carries the price of it.
 *
 * [!] NOT REENTRANT.  These tables are the camera producer thread's private
 * state -- it is the only caller (cam_publish), and the console reaches the
 * settings through camera_set_wb() rather than through here.
 */

/* Largest stored saturation offset.  y is 0..255 and the result is clamped to
 * 0..255 anyway, so an offset of +256 already forces white for every y and
 * -256 already forces black: storing more resolution than that would change no
 * output, and this keeps the table int16 for any gain the API accepts. */
#define CAM_SPREAD_LIMIT 256

/*
 * The encode table covers the UNCLAMPED range, so that the clamp is a property
 * of the table instead of two compare-and-branches per channel per pixel.
 *
 * [!] These bounds are not a margin, they are exactly what saturation can
 * produce: y is 0..255 (it is a weighted mean of three 8-bit values) and the
 * offset is +-CAM_SPREAD_LIMIT, so the sum spans -256..511 and nothing else.
 * They are DERIVED from that limit rather than written out, because an index
 * this table does not cover is a read off the end of it -- and the loop no
 * longer has a clamp that would notice.
 */
#define CAM_ENC_LO (-CAM_SPREAD_LIMIT)
#define CAM_ENC_HI (255 + CAM_SPREAD_LIMIT)
#define CAM_ENC_N  (CAM_ENC_HI - CAM_ENC_LO + 1)

static struct {
	/** The settings these tables were built from; see cam_tone_sync(). */
	struct cam_wb key;
	uint8_t       built;
	/** Raw sample -> the finished RGB565 field, for the no-saturation path. */
	uint16_t direct[3][256];
	/** Raw sample -> black level and gain applied, still linear. */
	uint8_t  linear[3][256];
	/** Linear value -> the finished RGB565 field: the clamp to 0..255, the
	 *  encode, the truncation and the shift into place, all in one entry.
	 *  Indexed by value - CAM_ENC_LO; the loop carries biased pointers. */
	uint16_t encode[3][CAM_ENC_N];
	/** (v - y) * sat / 256, clamped to +-CAM_SPREAD_LIMIT, at index
	 *  (v - y) + 255 -- the loop carries a pointer biased by that 255. */
	int16_t  spread[511];
} cam_tone;

/*
 * [!] THE KEY IS COMPARED BYTE-WISE, so struct cam_wb must have no padding: a
 * hole would make the comparison read indeterminate bytes and rebuild the
 * tables at random.  Stating the size here is also what makes a FIELD ADDED
 * LATER safe -- memcmp covers it automatically, and this assertion fails the
 * build if that new field brings a hole with it.  A hand-written field-by-field
 * comparison would silently ignore it instead, and the symptom would be a
 * setting that does nothing.
 */
_Static_assert(sizeof(struct cam_wb) ==
               3u * sizeof(uint16_t) + 2u * sizeof(uint8_t) + sizeof(uint16_t),
               "struct cam_wb has padding; the tone-table key cannot memcmp it");

/* Channel index 0/1/2 is B/G/R -- the order WDMA3 writes the planes in, so the
 * tables are indexed with the same number as the plane they belong to. */
static const uint8_t cam_field_drop[3]  = { 3u, 2u, 3u };
static const uint8_t cam_field_shift[3] = { 0u, 5u, 11u };

static void cam_tone_build(const struct cam_wb *wb)
{
	const uint16_t gain[3] = { wb->b, wb->g, wb->r };
	uint32_t c, v;
	int32_t t;

	for (c = 0u; c < 3u; c++) {
		for (v = 0u; v < (uint32_t)CAM_ENC_N; v++) {
			/* The clamp lives here, once per setting change,
			 * instead of twice per channel per pixel. */
			int32_t s = (int32_t)v + CAM_ENC_LO;
			uint8_t e;

			if (s < 0)
				s = 0;
			if (s > 255)
				s = 255;
			e = wb->gamma ? cam_srgb_encode[s] : (uint8_t)s;

			cam_tone.encode[c][v] =
			        (uint16_t)((uint32_t)(e >> cam_field_drop[c])
			                   << cam_field_shift[c]);
		}
		for (v = 0u; v < 256u; v++) {
			uint8_t lin = cam_gain8((uint8_t)v, gain[c], wb->black);

			cam_tone.linear[c][v] = lin;
			/* The whole chain for this channel, in one entry: the
			 * saturation-free path is three of these ORed together
			 * and nothing else. */
			cam_tone.direct[c][v] =
			        cam_tone.encode[c][lin - CAM_ENC_LO];
		}
	}

	for (t = -255; t <= 255; t++) {
		int32_t d = (t * (int32_t)wb->sat) / 256;

		if (d > CAM_SPREAD_LIMIT)
			d = CAM_SPREAD_LIMIT;
		if (d < -CAM_SPREAD_LIMIT)
			d = -CAM_SPREAD_LIMIT;
		cam_tone.spread[t + 255] = (int16_t)d;
	}

	cam_tone.key = *wb;
	cam_tone.built = 1u;
}

/*
 * Make the tables agree with @p wb, and hand back the settings they were built
 * from.
 *
 * The caller works from the RETURNED copy rather than from @p wb, and that is
 * the point of returning it: the console may write those fields while this
 * thread is packing, so a loop that re-read them per pixel could pack a frame
 * under two different saturations.  One snapshot is taken, the tables are built
 * from it, and the loop below reads only it -- so whatever a frame was packed
 * with, the tables and the code agree on it.
 */
static struct cam_wb cam_tone_sync(const struct cam_wb *wb)
{
	static const struct cam_wb unity = { CAM_WB_UNITY, CAM_WB_UNITY,
	                                     CAM_WB_UNITY, 0u, 0u,
	                                     CAM_SAT_UNITY };
	struct cam_wb now = (wb != 0) ? *wb : unity;

	if (!cam_tone.built ||
	    memcmp(&cam_tone.key, &now, sizeof now) != 0)
		cam_tone_build(&now);
	return now;
}

/* Saturation, the part that does not fold: push a channel away from the pixel's
 * own luma.  Signed, and deliberately NOT clamped -- it returns a value in
 * CAM_ENC_LO..CAM_ENC_HI, which is exactly the range the encode table covers,
 * so the clamp costs nothing here instead of two branches per channel.
 *
 * [!] always_inline is not decoration.  This translation unit is built -Os,
 * which does NOT inline it -- measured on the linked image, the loop below
 * called it three times per pixel, which is 230,400 calls per frame in the one
 * loop the whole point of this file is to make cheap.
 *
 * [!] AND THE CALLS COST MORE THAN TIME.  Live values across three calls put
 * the register allocator under enough pressure that GCC 13.3 at -Os spilled a
 * constant into P0 -- VPR[15:0], an MVE register this image is supposed never
 * to touch (VMSR P0, r3 at the top of the loop).  Inlining removed the pressure
 * and the spill with it.  That is a happy accident and not a guarantee:
 * check_mve_predication.py passed the build, because the pinned objdump does
 * not decode MVE at all and prints the register as "<impl def 0xd>" (issue
 * #66).  Until that gate can fail, the only thing standing behind "no MVE here"
 * is reading the disassembly. */
static inline __attribute__((always_inline)) int32_t cam_spread(
        const int16_t *spread, uint8_t v, int32_t y)
{
	return y + spread[(int32_t)v - y];
}

void cam_bgr_planar_to_rgb565(const uint8_t *bgr, uint16_t *out,
                              uint32_t pixels)
{
	cam_bgr_planar_to_rgb565_wb(bgr, out, pixels, 0);
}

void cam_bgr_planar_to_rgb565_wb(const uint8_t *bgr, uint16_t *out,
                                 uint32_t pixels, const struct cam_wb *wb)
{
	const uint8_t *b = bgr;
	const uint8_t *g = bgr + pixels;
	const uint8_t *r = bgr + 2u * pixels;
	/* Biased once, here, rather than per channel per pixel: both offsets are
	 * instructions a pixel folded into pointers the loop already keeps. */
	const int16_t  *spread = &cam_tone.spread[255];
	const uint16_t *enc_r  = &cam_tone.encode[2][-CAM_ENC_LO];
	const uint16_t *enc_g  = &cam_tone.encode[1][-CAM_ENC_LO];
	const uint16_t *enc_b  = &cam_tone.encode[0][-CAM_ENC_LO];
	struct cam_wb now;
	uint32_t i;

	if (bgr == 0 || out == 0)
		return;

	now = cam_tone_sync(wb);

	/* Three sequential reads and one sequential write per pixel.  Written as
	 * plain scalar C on purpose: the obvious "improvement" here is to widen
	 * it, and MVE is exactly what the image may not contain. */
	if (now.sat == CAM_SAT_UNITY || now.sat == 0u) {
		/* Everything folded: one table lookup per channel is the entire
		 * conversion, black level and gain and curve included.  Unity
		 * gains land here too and pay nothing extra for it, which is why
		 * the separate unity fast path this replaced is gone. */
		for (i = 0u; i < pixels; i++)
			out[i] = (uint16_t)(cam_tone.direct[2][r[i]] |
			                    cam_tone.direct[1][g[i]] |
			                    cam_tone.direct[0][b[i]]);
		return;
	}

	for (i = 0u; i < pixels; i++) {
		uint8_t bv = cam_tone.linear[0][b[i]];
		uint8_t gv = cam_tone.linear[1][g[i]];
		uint8_t rv = cam_tone.linear[2][r[i]];
		/* Luma, weighted the way the eye is: green carries most of the
		 * brightness.  (R + 2G + B) / 4 is the cheap standard
		 * approximation and needs no multiplies. */
		int32_t y = ((int32_t)rv + 2 * (int32_t)gv + (int32_t)bv) / 4;

		out[i] = (uint16_t)(enc_r[cam_spread(spread, rv, y)] |
		                    enc_g[cam_spread(spread, gv, y)] |
		                    enc_b[cam_spread(spread, bv, y)]);
	}
}

void cam_plane_stats(const uint8_t *plane, uint32_t pixels,
                     struct cam_plane_stats *out)
{
	uint32_t sum = 0u;
	uint8_t  min = 255u;
	uint8_t  max = 0u;
	uint32_t i;

	if (out == 0)
		return;
	if (plane == 0 || pixels == 0u) {
		out->min = 255u;
		out->max = 0u;
		out->mean_x100 = 0u;
		return;
	}

	for (i = 0u; i < pixels; i++) {
		uint8_t v = plane[i];

		if (v < min)
			min = v;
		if (v > max)
			max = v;
		sum += v;
	}

	out->min = min;
	out->max = max;
	/* 320*240 pixels * 255 * 100 fits in 32 bits with room to spare
	 * (1.96e9 < 4.29e9), so the scaling happens before the divide and the
	 * mean keeps two decimals without any floating point. */
	out->mean_x100 = (sum * 100u) / pixels;
}

uint32_t cam_plane_mosaic_x100(const uint8_t *plane, uint32_t w, uint32_t h)
{
	uint32_t sum[4] = { 0u, 0u, 0u, 0u };
	uint32_t cnt[4] = { 0u, 0u, 0u, 0u };
	uint32_t mean[4];
	uint32_t lo, hi, i, x, y;

	if (plane == 0 || w < 2u || h < 2u)
		return 0u;

	for (y = 0u; y < h; y++) {
		const uint8_t *row = plane + (size_t)y * w;
		uint32_t phase_y = (y & 1u) << 1;

		for (x = 0u; x < w; x++) {
			uint32_t ph = phase_y | (x & 1u);

			sum[ph] += row[x];
			cnt[ph]++;
		}
	}

	for (i = 0u; i < 4u; i++) {
		if (cnt[i] == 0u)
			return 0u;
		mean[i] = (sum[i] * 100u) / cnt[i];
	}

	lo = mean[0];
	hi = mean[0];
	for (i = 1u; i < 4u; i++) {
		if (mean[i] < lo)
			lo = mean[i];
		if (mean[i] > hi)
			hi = mean[i];
	}
	return hi - lo;
}

void cam_frame_means_x100(const uint8_t *bgr, uint32_t pixels, uint32_t step,
                          uint32_t out[3])
{
	uint32_t sum[3] = { 0u, 0u, 0u };
	uint32_t n = 0u;
	uint32_t i, c;

	if (out == 0)
		return;
	for (c = 0u; c < 3u; c++)
		out[c] = 0u;
	if (bgr == 0 || pixels == 0u || step == 0u)
		return;

	for (i = 0u; i < pixels; i += step) {
		sum[0] += bgr[i];
		sum[1] += bgr[pixels + i];
		sum[2] += bgr[2u * pixels + i];
		n++;
	}
	if (n == 0u)
		return;
	for (c = 0u; c < 3u; c++)
		out[c] = (sum[c] * 100u) / n;
}

void cam_bayer_phase_means_x100(const uint8_t *plane, uint32_t w, uint32_t h,
                                uint32_t out[4])
{
	uint32_t sum[4] = { 0u, 0u, 0u, 0u };
	uint32_t cnt[4] = { 0u, 0u, 0u, 0u };
	uint32_t i, x, y;

	if (out == 0)
		return;
	for (i = 0u; i < 4u; i++)
		out[i] = 0u;
	if (plane == 0 || w < 2u || h < 2u)
		return;

	for (y = 0u; y < h; y++) {
		const uint8_t *row = plane + (size_t)y * w;
		uint32_t phase_y = (y & 1u) << 1;

		for (x = 0u; x < w; x++) {
			uint32_t ph = phase_y | (x & 1u);

			sum[ph] += row[x];
			cnt[ph]++;
		}
	}
	for (i = 0u; i < 4u; i++)
		if (cnt[i] != 0u)
			out[i] = (sum[i] * 100u) / cnt[i];
}

uint32_t cam_frame_colour_x100(const uint8_t *bgr, uint32_t pixels)
{
	const uint8_t *b = bgr;
	const uint8_t *g = bgr + pixels;
	const uint8_t *r = bgr + 2u * pixels;
	uint32_t sum = 0u;
	uint32_t i;

	if (bgr == 0 || pixels == 0u)
		return 0u;

	for (i = 0u; i < pixels; i++) {
		uint8_t lo = b[i], hi = b[i];

		if (g[i] < lo) lo = g[i];
		if (g[i] > hi) hi = g[i];
		if (r[i] < lo) lo = r[i];
		if (r[i] > hi) hi = r[i];
		sum += (uint32_t)(hi - lo);
	}
	/* 320*240 * 255 * 100 = 1.96e9: fits, but only just, so the scale goes
	 * on the sum and not on the accumulator. */
	return (sum * 100u) / pixels;
}

uint32_t cam_plane_row_seam_x100(const uint8_t *plane, uint32_t w, uint32_t h,
                                 uint32_t *row_out)
{
	uint32_t prev_mean_x100 = 0u;
	uint32_t worst = 0u;
	uint32_t worst_row = 0u;
	uint32_t y;

	if (row_out != 0)
		*row_out = 0u;
	if (plane == 0 || w == 0u || h < 2u)
		return 0u;

	for (y = 0u; y < h; y++) {
		const uint8_t *row = plane + (size_t)y * w;
		uint32_t sum = 0u;
		uint32_t mean_x100;
		uint32_t x;

		for (x = 0u; x < w; x++)
			sum += row[x];
		mean_x100 = (sum * 100u) / w;

		if (y > 0u) {
			uint32_t step = (mean_x100 > prev_mean_x100) ?
			                (mean_x100 - prev_mean_x100) :
			                (prev_mean_x100 - mean_x100);

			if (step > worst) {
				worst = step;
				worst_row = y;
			}
		}
		prev_mean_x100 = mean_x100;
	}

	if (row_out != 0)
		*row_out = worst_row;
	return worst;
}
