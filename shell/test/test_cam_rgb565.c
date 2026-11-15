/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Host test for `camera capture`'s RGB565 statistics (issue #237,
 * svc/camera/cam_rgb565.c -- shared by f746g-disco and wio-lite-ai).
 *
 * WHY THIS EXISTS.  Both boards used to compute these themselves, and step 1
 * of issue #237 replaced both computations with this one without changing a
 * character either board prints.  The proof is here: the two computations as
 * they were (copied below verbatim from boards/f746g-disco/port/camera/
 * camera_svc_f746.c and boards/wio-lite-ai/port/camera/camera_svc_wio.c at
 * 34c295a, the commit before) are run on the same frames, read the way each
 * board reads them (f746g-disco 512 bytes at a time, wio-lite-ai one row at a
 * time), and every number has to agree.  The cases a camera does not hand you
 * on demand are walked too: a read that does not end on a row (WVGA's 1600-byte
 * rows in 512-byte reads), equal steps, a frame with no step at all, a short
 * last row, and the largest values at the largest size.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cam_rgb565.h"

static int fails;
static int checks;

#define EQ(got, want, what)                                                  \
	do {                                                                 \
		unsigned long g_ = (unsigned long)(got);                     \
		unsigned long w_ = (unsigned long)(want);                    \
		checks++;                                                    \
		if (g_ != w_) {                                              \
			printf("  FAIL %s:%d %s: got %lu, wanted %lu\n",     \
			       __func__, __LINE__, what, g_, w_);            \
			fails++;                                             \
		}                                                            \
	} while (0)

/* ---- the two computations as they were (34c295a) ------------------------ */

/* wio-lite-ai: one 320-pixel row per read; the row chunk is a parameter here
   so the same code runs on the f746g-disco widths too. */
struct rgb_stats {
	uint32_t min[3], max[3], sum[3];
	uint32_t npx;
};

static void stats_init(struct rgb_stats *s)
{
	for (int i = 0; i < 3; i++) {
		s->min[i] = 0xFFFFFFFFu;
		s->max[i] = 0u;
		s->sum[i] = 0u;
	}
	s->npx = 0u;
}

static void stats_add_rgb565(struct rgb_stats *s, const uint8_t *buf, uint32_t n)
{
	for (uint32_t i = 0; i + 1u < n; i += 2u) {
		uint32_t p = (uint32_t)buf[i] | ((uint32_t)buf[i + 1u] << 8);
		uint32_t ch[3] = { (p >> 11) & 0x1Fu, (p >> 5) & 0x3Fu, p & 0x1Fu };

		for (int k = 0; k < 3; k++) {
			if (ch[k] < s->min[k]) s->min[k] = ch[k];
			if (ch[k] > s->max[k]) s->max[k] = ch[k];
			s->sum[k] += ch[k];
		}
		s->npx++;
	}
}

static void row_means_x100(const uint8_t *buf, uint32_t n, uint32_t out[3])
{
	uint32_t sum[3] = { 0u, 0u, 0u };
	uint32_t npx = 0u;

	for (uint32_t i = 0; i + 1u < n; i += 2u) {
		uint32_t p = (uint32_t)buf[i] | ((uint32_t)buf[i + 1u] << 8);

		sum[0] += (p >> 11) & 0x1Fu;
		sum[1] += (p >> 5) & 0x3Fu;
		sum[2] += p & 0x1Fu;
		npx++;
	}
	for (int k = 0; k < 3; k++)
		out[k] = npx ? (sum[k] * 100u) / npx : 0u;
}

struct seam_max {
	uint32_t row;
	uint32_t d[3];
	uint32_t total;
};

static void seam_track(struct seam_max *s, uint32_t row,
                       const uint32_t a[3], const uint32_t b[3])
{
	uint32_t d[3], total = 0u;

	for (int k = 0; k < 3; k++) {
		d[k] = (a[k] > b[k]) ? a[k] - b[k] : b[k] - a[k];
		total += d[k];
	}
	if (total > s->total) {
		s->total = total;
		s->row   = row;
		for (int k = 0; k < 3; k++)
			s->d[k] = d[k];
	}
}

/* cam_capture_walk()'s loop, without the reads. */
static void old_wio_walk(const uint8_t *frame, uint32_t frame_bytes,
                         uint32_t chunk, struct rgb_stats *st,
                         struct seam_max *seam)
{
	uint32_t prev[3] = { 0u, 0u, 0u };
	uint32_t off, row;

	memset(seam, 0, sizeof *seam);
	stats_init(st);
	for (off = 0, row = 0; off < frame_bytes; row++) {
		uint32_t n = (frame_bytes - off < chunk) ? frame_bytes - off : chunk;
		uint32_t rmean[3];

		stats_add_rgb565(st, frame + off, n);
		row_means_x100(frame + off, n, rmean);
		if (row != 0u)
			seam_track(seam, row, prev, rmean);
		prev[0] = rmean[0]; prev[1] = rmean[1]; prev[2] = rmean[2];
		off += n;
	}
}

/* f746g-disco: camera_svc_capture_report()'s RGB565 branch, 512 B reads. */
static void old_f746_walk(const uint8_t *frame, uint32_t frame_bytes,
                          uint32_t min[3], uint32_t max[3], uint32_t sum[3])
{
	uint32_t rmin = 255, rmax = 0, rsum = 0;
	uint32_t gmin = 255, gmax = 0, gsum = 0;
	uint32_t bmin = 255, bmax = 0, bsum = 0;

	for (uint32_t off = 0; off < frame_bytes; ) {
		uint32_t n = (frame_bytes - off < 512u) ? frame_bytes - off : 512u;
		const uint8_t *buf = frame + off;

		for (uint32_t i = 0; i + 1u < n; i += 2u) {
			uint32_t p = (uint32_t)buf[i] |
			             ((uint32_t)buf[i + 1u] << 8);
			uint32_t r = (p >> 11) & 0x1Fu;
			uint32_t gg = (p >> 5) & 0x3Fu;
			uint32_t b = p & 0x1Fu;

			if (r < rmin) rmin = r;
			if (r > rmax) rmax = r;
			rsum += r;
			if (gg < gmin) gmin = gg;
			if (gg > gmax) gmax = gg;
			gsum += gg;
			if (b < bmin) bmin = b;
			if (b > bmax) bmax = b;
			bsum += b;
		}
		off += n;
	}
	min[0] = rmin; min[1] = gmin; min[2] = bmin;
	max[0] = rmax; max[1] = gmax; max[2] = bmax;
	sum[0] = rsum; sum[1] = gsum; sum[2] = bsum;
}

/* ---- the new one, fed in pieces of `chunk` bytes ------------------------- */

static void new_walk(const uint8_t *frame, uint32_t frame_bytes, uint32_t width,
                     uint32_t chunk, struct cam_rgb565_stats *st)
{
	struct cam_rgb565_walk w;

	cam_rgb565_begin(&w, st, width);
	for (uint32_t off = 0; off < frame_bytes; ) {
		uint32_t n = (frame_bytes - off < chunk) ? frame_bytes - off : chunk;

		cam_rgb565_add(&w, frame + off, n);
		off += n;
	}
	cam_rgb565_end(&w);
}

/* ---- frames --------------------------------------------------------------- */

#define MAX_W 800u
#define MAX_H 480u
static uint8_t frame[MAX_W * MAX_H * 2u];   /* the host test's own storage */

static uint32_t rng = 0x12345678u;

static uint32_t next(void)
{
	rng ^= rng << 13;
	rng ^= rng >> 17;
	rng ^= rng << 5;
	return rng;
}

static void put_px(uint32_t i, uint32_t r, uint32_t g, uint32_t b)
{
	uint32_t p = (r << 11) | (g << 5) | b;

	frame[2u * i] = (uint8_t)p;
	frame[2u * i + 1u] = (uint8_t)(p >> 8);
}

/* Noise, plus a band of rows whose level jumps (a gain change landing
   mid-readout, what the seam is for), so the seam is not near zero. */
static void fill_random(uint32_t w, uint32_t h)
{
	uint32_t jump = h / 3u;

	for (uint32_t y = 0; y < h; y++) {
		for (uint32_t x = 0; x < w; x++) {
			uint32_t r = next() % 32u, g = next() % 64u, b = next() % 32u;

			if (y >= jump && y < jump + 5u) {   /* the upper half */
				r = 16u + r / 2u;
				g = 32u + g / 2u;
			}
			put_px(y * w + x, r, g, b);
		}
	}
}

/* ---- the comparisons ------------------------------------------------------ */

/* Every number both old computations produced, against the new one. */
static void compare(uint32_t w, uint32_t h, const char *what)
{
	uint32_t bytes = w * h * 2u;
	static const uint32_t chunks[] = { 2u, 6u, 512u, 514u, 1000u, 0u };
	struct rgb_stats ost;
	struct seam_max oseam;
	uint32_t fmin[3], fmax[3], fsum[3];
	struct cam_rgb565_stats st;
	char tag[96];

	old_wio_walk(frame, bytes, w * 2u, &ost, &oseam);
	old_f746_walk(frame, bytes, fmin, fmax, fsum);

	for (unsigned c = 0; c < sizeof chunks / sizeof chunks[0]; c++) {
		uint32_t chunk = chunks[c] ? chunks[c] : w * 2u;   /* 0: a row */

		new_walk(frame, bytes, w, chunk, &st);
		snprintf(tag, sizeof tag, "%s %ux%u by %u", what, (unsigned)w,
		         (unsigned)h, (unsigned)chunk);
		for (int k = 0; k < 3; k++) {
			/* wio-lite-ai's numbers */
			EQ(st.min[k], ost.min[k], tag);
			EQ(st.max[k], ost.max[k], tag);
			EQ(st.sum[k], ost.sum[k], tag);
			EQ(st.seam_d[k], oseam.d[k], tag);
			/* f746g-disco's numbers */
			EQ(st.min[k], fmin[k], tag);
			EQ(st.max[k], fmax[k], tag);
			EQ(st.sum[k], fsum[k], tag);
		}
		EQ(st.npx, ost.npx, tag);
		EQ(st.npx, w * h, tag);
		EQ(st.seam_row, oseam.row, tag);
		EQ(st.seam_total, oseam.total, tag);
	}
}

/* f746g-disco's four sizes and wio-lite-ai's one, on noise with a jump. */
static void test_same_as_before(void)
{
	static const uint32_t sizes[][2] = {
		{ 160u, 120u }, { 320u, 240u }, { 640u, 480u }, { 800u, 480u },
	};

	for (unsigned i = 0; i < sizeof sizes / sizeof sizes[0]; i++) {
		for (int rep = 0; rep < 3; rep++) {
			fill_random(sizes[i][0], sizes[i][1]);
			compare(sizes[i][0], sizes[i][1], "noise");
		}
	}
}

/* The seam is where the jump is, with the exact step: rows 0..9 at one level,
   rows 10.. at another (row means are exact, every pixel equal in a row). */
static void test_seam_value(void)
{
	struct cam_rgb565_stats st;
	uint32_t w = 800u, h = 20u;

	for (uint32_t y = 0; y < h; y++)
		for (uint32_t x = 0; x < w; x++)
			put_px(y * w + x, y < 10u ? 3u : 10u, y < 10u ? 40u : 1u, 7u);
	new_walk(frame, w * h * 2u, w, 512u, &st);   /* 1600 B rows, 512 B reads */
	EQ(st.seam_row, 10u, "the first row of the new level");
	EQ(st.seam_d[0], 700u, "dR 7.00");
	EQ(st.seam_d[1], 3900u, "dG 39.00");
	EQ(st.seam_d[2], 0u, "dB 0.00");
	EQ(st.seam_total, 4600u, "ranked on the sum");
	compare(w, h, "two levels");
}

/* A truncated row mean: one pixel of 1 in a 3-pixel row is 33 (x100), not 33.3. */
static void test_row_mean_truncates(void)
{
	struct cam_rgb565_stats st;

	memset(frame, 0, 3u * 2u * 2u);
	put_px(3u, 1u, 0u, 0u);   /* row 1, first pixel: R = 1 */
	new_walk(frame, 3u * 2u * 2u, 3u, 2u, &st);
	EQ(st.seam_row, 1u, "row 1");
	EQ(st.seam_d[0], 33u, "1 * 100 / 3 truncated");
}

/* Two equal steps: the earlier row is kept. */
static void test_tie_keeps_first(void)
{
	struct cam_rgb565_stats st;
	uint32_t w = 160u, h = 6u;
	static const uint32_t lvl[6] = { 0u, 5u, 5u, 0u, 0u, 0u };

	/* row 1 steps up by 5, row 3 steps down by 5: equal totals */
	for (uint32_t y = 0; y < h; y++)
		for (uint32_t x = 0; x < w; x++)
			put_px(y * w + x, lvl[y], 0u, 0u);
	new_walk(frame, w * h * 2u, w, 512u, &st);
	EQ(st.seam_row, 1u, "the first of two equal steps");
	EQ(st.seam_d[0], 500u, "its step");
	compare(w, h, "tie");
}

/* No step anywhere: row 0, zero steps. */
static void test_flat_frame(void)
{
	struct cam_rgb565_stats st;
	uint32_t w = 320u, h = 240u;

	for (uint32_t i = 0; i < w * h; i++)
		put_px(i, 17u, 33u, 9u);
	new_walk(frame, w * h * 2u, w, 512u, &st);
	EQ(st.seam_row, 0u, "flat: row 0");
	EQ(st.seam_total, 0u, "flat: no step");
	for (int k = 0; k < 3; k++)
		EQ(st.seam_d[k], 0u, "flat: zero");
	EQ(st.min[0], 17u, "min R");
	EQ(st.max[1], 33u, "max G");
	EQ(st.sum[2], 9u * w * h, "sum B");
	compare(w, h, "flat");
}

/* Every pixel at its maximum at the largest size: nothing wraps. */
static void test_largest(void)
{
	struct cam_rgb565_stats st;
	uint32_t w = MAX_W, h = MAX_H;

	memset(frame, 0xFF, w * h * 2u);
	new_walk(frame, w * h * 2u, w, 512u, &st);
	EQ(st.sum[0], 31u * w * h, "R sum");
	EQ(st.sum[1], 63u * w * h, "G sum");
	EQ(st.sum[2], 31u * w * h, "B sum");
	EQ(st.max[0], 31u, "R max");
	EQ(st.max[1], 63u, "G max");
	EQ(st.max[2], 31u, "B max");
	EQ(st.min[1], 63u, "G min");
	EQ(st.npx, w * h, "pixels");
	EQ(st.seam_total, 0u, "no step");
	compare(w, h, "saturated");
}

/* A frame that is not a whole number of rows: the short last row counts as a
   row of its own length, as wio-lite-ai's one-row reads made it. */
static void test_short_last_row(void)
{
	struct cam_rgb565_stats st;
	struct rgb_stats ost;
	struct seam_max oseam;
	uint32_t w = 320u, bytes = 320u * 4u * 2u + 100u;   /* 4 rows + 50 px */

	for (uint32_t i = 0; i < 4u * w; i++)
		put_px(i, 10u, 20u, 10u);
	for (uint32_t i = 4u * w; i < 4u * w + 50u; i++)
		put_px(i, 31u, 63u, 31u);   /* a step the last row alone makes */
	old_wio_walk(frame, bytes, w * 2u, &ost, &oseam);
	new_walk(frame, bytes, w, 512u, &st);
	EQ(st.seam_row, oseam.row, "the short row's step");
	EQ(st.seam_row, 4u, "is the short row");
	for (int k = 0; k < 3; k++)
		EQ(st.seam_d[k], oseam.d[k], "its value");
	EQ(st.npx, ost.npx, "pixels");
}

/* No pixel: the answer is the initial one (minimum not yet seen). */
static void test_empty(void)
{
	struct cam_rgb565_stats st;

	new_walk(frame, 0u, 320u, 512u, &st);
	EQ(st.npx, 0u, "no pixel");
	EQ(st.min[0], 0xFFFFFFFFu, "min unseen");
	EQ(st.max[0], 0u, "max");
	EQ(st.seam_row, 0u, "seam row");
}

int main(void)
{
	test_same_as_before();
	test_seam_value();
	test_row_mean_truncates();
	test_tie_keeps_first();
	test_flat_frame();
	test_largest();
	test_short_last_row();
	test_empty();

	if (fails) {
		printf("test_cam_rgb565: %d of %d FAILED\n", fails, checks);
		return 1;
	}
	printf("test_cam_rgb565: all %d checks passed\n", checks);
	return 0;
}
