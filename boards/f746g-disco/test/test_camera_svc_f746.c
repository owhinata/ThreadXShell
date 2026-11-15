/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Host test for this board's `camera capture` report (issue #237,
 * port/camera/camera_svc_f746.c) over a fake camera driver.
 *
 * WHY THIS EXISTS.  The report reads the frame in many calls -- one byte for
 * the generation, then the length, the format and the size, then the
 * statistics 512 bytes at a time (or a JPEG's head and tail) -- and another
 * console can replace the frame (`camera capture`) or drop it (`camera res`,
 * `camera format`) between any two of them.  Typing that is not possible; the
 * fake driver does it at every point, and each time the report has to fail
 * (so the shared command prints none of its numbers), never describe a new
 * frame by an old size.  The numbers of a frame left alone are checked too:
 * RGB565 at a width 512-byte reads do not divide (the seam included), YUV422,
 * Y8 and JPEG with and without its markers and at impossible lengths.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "camera.h"
#include "camera_names.h"
#include "camera_svc.h"
#include "camera_svc_adapter.h"

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

/* ---- the fake driver ----------------------------------------------------- */

#define FAKE_CAP (800u * 480u * 2u)
static uint8_t fake_frame[FAKE_CAP];
static struct camera_mode fake_mode;
static struct camera_info fake_info;
static uint32_t fake_gen;

/* What happens, and when: at the Nth call of the driver (counted across
   camera_frame_read, camera_get_mode and camera_get_info), before it answers. */
enum fake_event { EV_NONE, EV_REPLACE, EV_DROP };
static enum fake_event fake_ev;
static int fake_ev_at;
static int fake_calls;
static int fake_reads;

static void fake_tick(void)
{
	if (fake_ev != EV_NONE && ++fake_calls == fake_ev_at) {
		if (fake_ev == EV_REPLACE) {
			/* another console's `camera res` and capture: a new
			   frame, of another shape and the same length */
			fake_gen++;
			memset(fake_frame, 0x5A, sizeof fake_frame);
			fake_mode.width = (uint16_t)(fake_mode.width / 2u);
			fake_mode.height = (uint16_t)(fake_mode.height * 2u);
		} else {
			fake_info.frame_valid = 0;   /* `camera res` / `format` */
			fake_info.frame_bytes = 0u;
		}
	}
}

int camera_frame_read(uint32_t offset, void *dst, uint32_t len, uint32_t *gen)
{
	fake_tick();
	fake_reads++;
	if (dst == NULL || len == 0u)
		return CAM_ERR_PARAM;
	if (!fake_info.frame_valid)
		return CAM_ERR_NO_FRAME;
	if (offset >= fake_info.frame_bytes || len > fake_info.frame_bytes - offset)
		return CAM_ERR_PARAM;
	memcpy(dst, fake_frame + offset, len);
	if (gen != NULL)
		*gen = fake_gen;
	return 0;
}

int camera_get_mode(struct camera_mode *out)
{
	fake_tick();
	*out = fake_mode;
	return 0;
}

int camera_get_info(struct camera_info *out)
{
	fake_tick();
	*out = fake_info;
	return 0;
}

/* The rest of the adapter's calls: not under test. */
int camera_probe(uint32_t *chip_id) { (void)chip_id; return CAM_ERR_STATE; }
int camera_power_off(void) { return CAM_ERR_STATE; }
int camera_capture_warm(int colorbar, uint32_t *warm_ms)
{ (void)colorbar; (void)warm_ms; return CAM_ERR_STATE; }
int camera_streaming(void) { return 0; }
int camera_stream_start(int colorbar, int *already)
{ (void)colorbar; (void)already; return CAM_ERR_STATE; }
int camera_stream_stop_gen(const char **users, int max, int *nusers,
                           uint32_t *gen)
{ (void)users; (void)max; (void)nusers; (void)gen; return CAM_ERR_STATE; }
int camera_stream_ended(uint32_t gen) { (void)gen; return 1; }
int camera_stream_users(const char **users, int max)
{ (void)users; (void)max; return 0; }
int camera_stream_holders(const char **names, int max, int *lost)
{ (void)names; (void)max; (void)lost; return 0; }
int camera_stream_stats(struct camera_stream_info *out)
{ (void)out; return CAM_ERR_STATE; }
int camera_subscribers_snapshot(struct camera_sub_stat *out, int max)
{ (void)out; (void)max; return 0; }
int camera_get_settings(struct camera_settings *out)
{ (void)out; return CAM_ERR_STATE; }

/* ---- frames ---------------------------------------------------------------- */

static void set_raster(enum camera_format fmt, uint16_t w, uint16_t h,
                       uint8_t bpp)
{
	memset(&fake_mode, 0, sizeof fake_mode);
	fake_mode.format = (uint8_t)fmt;
	fake_mode.width = w;
	fake_mode.height = h;
	fake_mode.bytes_per_pixel = bpp;
	fake_mode.frame_bytes = (uint32_t)w * h * bpp;
	memset(&fake_info, 0, sizeof fake_info);
	fake_info.frame_valid = 1;
	fake_info.frame_bytes = fake_mode.frame_bytes;
	fake_gen = 7u;
	fake_ev = EV_NONE;
	fake_calls = 0;
	fake_reads = 0;
}

static void set_jpeg(uint32_t len)
{
	memset(&fake_mode, 0, sizeof fake_mode);
	fake_mode.format = (uint8_t)CAM_FMT_JPEG;
	fake_mode.width = 320u;
	fake_mode.height = 240u;
	fake_mode.is_jpeg = 1u;
	fake_mode.frame_bytes = 4096u;   /* the budget */
	memset(&fake_info, 0, sizeof fake_info);
	fake_info.frame_valid = 1;
	fake_info.frame_bytes = len;
	fake_gen = 7u;
	fake_ev = EV_NONE;
	fake_calls = 0;
	fake_reads = 0;
	memset(fake_frame, 0x11, sizeof fake_frame);
}

static void put565(uint32_t i, uint32_t r, uint32_t g, uint32_t b)
{
	uint32_t p = (r << 11) | (g << 5) | b;

	fake_frame[2u * i] = (uint8_t)p;
	fake_frame[2u * i + 1u] = (uint8_t)(p >> 8);
}

/* ---- running the report --------------------------------------------------- */

static int cancel_now;
static int never_cancel(void *ctx) { (void)ctx; return cancel_now; }

static struct camera_svc_capture_report rep;

static char last_detail[CAMERA_SVC_DETAIL_MAX + 1];   /* the failure's words */

static int report(void)
{
	struct camera_op_result res;

	memset(&rep, 0, sizeof rep);
	rep.warmup = 123u;
	rep.warmup_unit = (uint8_t)CAMERA_SVC_WARMUP_MS;
	memset(&res, 0, sizeof res);
	camera_svc_capture_report(&rep, &res, never_cancel, NULL);
	EQ(rep.warmup, 123u, "the warm-up is the capture's");
	memcpy(last_detail, res.detail, sizeof last_detail);
	return res.status;
}

/* The board's lines, captured. */
static char lines[512];
static size_t lines_len;
static int lines_refuse;

static int lines_write(void *ctx, const char *s, size_t len)
{
	(void)ctx;
	if (lines_refuse)
		return -1;
	if (lines_len + len < sizeof lines) {
		memcpy(lines + lines_len, s, len);
		lines_len += len;
		lines[lines_len] = '\0';
	}
	return (int)len;
}

static int board_lines(void)
{
	lines_len = 0u;
	lines[0] = '\0';
	return camera_svc_capture_board_lines(&rep, lines_write, NULL);
}

/* ---- the numbers of a frame left alone ------------------------------------ */

static void test_rgb565_numbers(void)
{
	uint32_t w = 800u, h = 6u;   /* 1600-byte rows, 512-byte reads */

	set_raster(CAM_FMT_RGB565, (uint16_t)w, (uint16_t)h, 2u);
	for (uint32_t y = 0; y < h; y++)
		for (uint32_t x = 0; x < w; x++)
			put565(y * w + x, y < 4u ? 3u : 10u, x % 2u ? 63u : 0u,
			       y < 4u ? 1u : 0u);
	EQ(report(), CAMERA_SVC_OK, "rgb565: ok");
	EQ(strcmp(rep.format, "rgb565"), 0, "rgb565: the format's word");
	EQ(rep.width, w, "width");
	EQ(rep.height, h, "height");
	EQ(rep.bytes, w * h * 2u, "bytes");
	EQ(rep.nchan, 3u, "three channels");
	EQ(strcmp(rep.chan[0].name, "R"), 0, "R");
	EQ(rep.chan[0].bits, 5u, "R is 5 bits");
	EQ(rep.chan[1].bits, 6u, "G is 6 bits");
	EQ(rep.chan[2].bits, 5u, "B is 5 bits");
	EQ(rep.chan[0].min, 3u, "R min");
	EQ(rep.chan[0].max, 10u, "R max");
	/* R: 4 rows of 3 and 2 of 10 -> 32 / 6 = 5.333.. -> 533 */
	EQ(rep.chan[0].mean_x100, 533u, "R mean x100, truncated");
	EQ(rep.chan[1].mean_x100, 3150u, "G mean 31.50");
	EQ(rep.chan[2].mean_x100, 66u, "B mean 0.666.. -> 0.66");
	EQ(rep.nseam, 3u, "the seam, three colours");
	EQ(rep.seam_row, 4u, "at the first row of the new level");
	EQ(rep.seam[0].d_x100, 700u, "dR 7.00");
	EQ(rep.seam[1].d_x100, 0u, "dG 0.00");
	EQ(rep.seam[2].d_x100, 100u, "dB 1.00");
	EQ(strcmp(rep.seam[1].name, "G"), 0, "dG's name");
	EQ(rep.head_len, CAMERA_SVC_HEAD_MAX, "a head");
	EQ(memcmp(rep.head, fake_frame, CAMERA_SVC_HEAD_MAX), 0,
	   "the head is the frame's first bytes");
	EQ(board_lines(), 0, "raster: the board's lines succeed");
	EQ(lines_len, 0u, "raster: and are none");
}

static void test_yuv422_numbers(void)
{
	uint32_t w = 160u, h = 2u;

	set_raster(CAM_FMT_YUV422, (uint16_t)w, (uint16_t)h, 2u);
	for (uint32_t i = 0; i < w * h * 2u; i += 4u) {
		fake_frame[i] = 255u;          /* Y0 */
		fake_frame[i + 1u] = 100u;     /* U  */
		fake_frame[i + 2u] = 254u;     /* Y1 */
		fake_frame[i + 3u] = (i / 4u) % 3u == 0u ? 1u : 0u;   /* V */
	}
	EQ(report(), CAMERA_SVC_OK, "yuv422: ok");
	EQ(strcmp(rep.format, "yuv422"), 0, "yuv422: the format's word");
	EQ(rep.nchan, 3u, "Y, Cb, Cr");
	EQ(strcmp(rep.chan[0].name, "Y"), 0, "Y");
	EQ(strcmp(rep.chan[1].name, "Cb"), 0, "Cb");
	EQ(strcmp(rep.chan[2].name, "Cr"), 0, "Cr");
	EQ(rep.chan[0].bits, 8u, "8 bits");
	EQ(rep.chan[0].max, 255u, "Y max");
	EQ(rep.chan[0].min, 254u, "Y min");
	EQ(rep.chan[0].mean_x100, 25450u, "Y mean 254.50");
	EQ(rep.chan[1].mean_x100, 10000u, "Cb mean 100.00 over the U samples");
	/* V: 160 groups, every third is 1 -> 54 / 160 = 0.3375 -> 33 */
	EQ(rep.chan[2].mean_x100, 33u, "Cr mean truncated");
	EQ(rep.nseam, 0u, "no seam for yuv422");
}

static void test_y8_numbers(void)
{
	uint32_t w = 800u, h = 480u;   /* the largest: no wrap */

	set_raster(CAM_FMT_Y8, (uint16_t)w, (uint16_t)h, 1u);
	memset(fake_frame, 255, w * h);
	fake_frame[w * h - 1u] = 0u;
	EQ(report(), CAMERA_SVC_OK, "y8: ok");
	EQ(strcmp(rep.format, "y8"), 0, "y8: the format's word");
	EQ(rep.nchan, 1u, "one channel");
	EQ(rep.chan[0].min, 0u, "Y min");
	EQ(rep.chan[0].max, 255u, "Y max");
	/* (384000 * 255 - 255) / 384000 = 254.99933.. -> 25499 */
	EQ(rep.chan[0].mean_x100, 25499u, "Y mean at WVGA, truncated, no wrap");
	EQ(rep.nseam, 0u, "no seam for y8");
}

static void test_jpeg_numbers(void)
{
	set_jpeg(100u);
	fake_frame[0] = 0xFFu; fake_frame[1] = 0xD8u;
	fake_frame[98] = 0xFFu; fake_frame[99] = 0xD9u;
	EQ(report(), CAMERA_SVC_OK, "jpeg: ok");
	EQ(strcmp(rep.format, "jpeg"), 0, "jpeg: the format's word");
	EQ(rep.bytes, 100u, "the stream's length, not the budget");
	EQ(rep.nchan, 0u, "no channel lines");
	EQ(rep.nseam, 0u, "no seam");
	EQ(rep.head_len, CAMERA_SVC_HEAD_MAX, "a head");
	EQ(board_lines(), 0, "the markers' lines");
	EQ(strcmp(lines, "SOI      : ok (FFD8)\r\nEOI      : ok (FFD9)\r\n"), 0,
	   "both markers there");

	/* the start's marker there, the end's not: FF D9 one byte early and one
	   byte late (past the stream), never at the last two bytes -- an end
	   looked for anywhere but len - 2 would find one */
	set_jpeg(100u);
	fake_frame[0] = 0xFFu; fake_frame[1] = 0xD8u;
	fake_frame[97] = 0xFFu; fake_frame[98] = 0xD9u;   /* one early     */
	fake_frame[99] = 0xFFu; fake_frame[100] = 0xD9u;  /* one late      */
	fake_frame[30] = 0xFFu; fake_frame[31] = 0xD9u;   /* in the head   */
	EQ(report(), CAMERA_SVC_OK, "jpeg with no end marker: ok");
	EQ(rep.board[0], 1u, "jpeg: the board's kind");
	EQ(rep.board[1], 1u, "jpeg: the start's marker found");
	EQ(rep.board[2], 0u, "jpeg: the end's marker not found off its place");
	(void)board_lines();
	EQ(strcmp(lines, "SOI      : ok (FFD8)\r\nEOI      : MISSING\r\n"), 0,
	   "start there, end missing");

	set_jpeg(10u);
	EQ(report(), CAMERA_SVC_OK, "jpeg without markers: ok");
	EQ(rep.head_len, 10u, "a head as long as the stream");
	(void)board_lines();
	EQ(strcmp(lines, "SOI      : MISSING\r\nEOI      : MISSING\r\n"), 0,
	   "both markers missing");

	lines_refuse = 1;
	EQ(board_lines() < 0, 1, "a refused line is said");
	lines_refuse = 0;

	set_jpeg(2u);
	fake_frame[0] = 0xFFu; fake_frame[1] = 0xD9u;
	EQ(report(), CAMERA_SVC_OK, "jpeg of two bytes: read");
	set_jpeg(1u);
	EQ(report(), CAMERA_SVC_ERR_NO_FRAME, "jpeg of one byte: refused");
	EQ(fake_reads, 1u, "after the generation, nothing is read");
	set_jpeg(0u);
	/* (the driver's first read already refuses an empty frame) */
	EQ(report() != CAMERA_SVC_OK, 1, "jpeg of none: refused");
	set_jpeg(4097u);
	EQ(report(), CAMERA_SVC_ERR_NO_FRAME, "jpeg past the budget: refused");
	EQ(fake_reads, 1u, "after the generation, nothing is read");
}

/* ---- the frame changing under the report ----------------------------------- */

/* Every call of the driver the report makes, the frame replaced or dropped
   just before it: every one fails the report. */
static void walk_events(const char *what, void (*setup)(void))
{
	int total;

	setup();
	EQ(report(), CAMERA_SVC_OK, what);
	total = 0;
	setup();
	fake_ev = EV_REPLACE;
	fake_ev_at = 1000000;
	(void)report();
	total = fake_calls;   /* how many driver calls the report made */
	for (int ev = EV_REPLACE; ev <= EV_DROP; ev++) {
		for (int at = 2; at <= total; at++) {
			int st;

			setup();
			fake_ev = (enum fake_event)ev;
			fake_ev_at = at;
			st = report();
			checks++;
			if (st == CAMERA_SVC_OK) {
				printf("  FAIL %s: the frame %s before driver call %d of "
				       "%d, and the report succeeded\n", what,
				       ev == EV_REPLACE ? "replaced" : "dropped",
				       at, total);
				fails++;
			}
		}
	}
	EQ(total >= 4, 1, "the report reads after taking the size");
}

static void setup_rgb565(void)
{
	set_raster(CAM_FMT_RGB565, 800u, 3u, 2u);   /* 4800 B: 10 reads */
}

static void setup_y8(void)
{
	set_raster(CAM_FMT_Y8, 160u, 4u, 1u);
}

static void setup_yuv(void)
{
	set_raster(CAM_FMT_YUV422, 160u, 4u, 2u);
}

static void setup_jpeg(void)
{
	set_jpeg(300u);
}

static void test_changes(void)
{
	walk_events("rgb565", setup_rgb565);
	walk_events("yuv422", setup_yuv);
	walk_events("y8", setup_y8);
	walk_events("jpeg", setup_jpeg);

	/* no frame at all */
	setup_rgb565();
	fake_info.frame_valid = 0;
	EQ(report(), CAMERA_SVC_ERR_NO_FRAME, "no frame: no captured frame");

	/* a raster length that is not the mode's */
	setup_rgb565();
	fake_info.frame_bytes -= 2u;
	EQ(report(), CAMERA_SVC_ERR_NO_FRAME, "a raster frame shorter than its mode");

	/* the words each one gives: another console's capture is "changed
	   mid-read", a `camera res` / `format` (which drops the frame) is "no
	   captured frame" -- the README quotes both */
	for (int at = 2; at <= 4; at++) {
		setup_rgb565();
		fake_ev = EV_REPLACE;
		fake_ev_at = at;
		EQ(report(), CAMERA_SVC_ERR_NO_FRAME, "a replaced frame");
		EQ(strcmp(last_detail, "frame changed mid-read (concurrent capture)"),
		   0, "is changed mid-read");
		setup_rgb565();
		fake_ev = EV_DROP;
		fake_ev_at = at;
		EQ(report(), CAMERA_SVC_ERR_NO_FRAME, "a dropped frame");
		EQ(strcmp(last_detail, "no captured frame"), 0, "is no captured frame");
	}
	setup_jpeg();
	fake_ev = EV_DROP;
	fake_ev_at = 4;   /* before the tail's read */
	EQ(report(), CAMERA_SVC_ERR_NO_FRAME, "a jpeg dropped before its tail");
	EQ(strcmp(last_detail, "no captured frame"), 0, "is no captured frame");

	/* a cancel between reads */
	setup_rgb565();
	cancel_now = 1;
	EQ(report(), CAMERA_SVC_ERR_CANCELLED, "a cancel ends the report");
	cancel_now = 0;
}

int main(void)
{
	test_rgb565_numbers();
	test_yuv422_numbers();
	test_y8_numbers();
	test_jpeg_numbers();
	test_changes();

	if (fails) {
		printf("test_camera_svc_f746: %d of %d FAILED\n", fails, checks);
		return 1;
	}
	printf("test_camera_svc_f746: all %d checks passed\n", checks);
	return 0;
}
