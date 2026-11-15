/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Host test for this board's `camera capture` (issue #237,
 * port/camera/camera_svc_grove.c) over a fake camera driver.
 *
 * WHY THIS EXISTS.  Since #237 the capture announces itself before the frames
 * it lets go, as f746g-disco and wio-lite-ai do, and a Ctrl+C among those
 * frames must leave that one line and nothing else: the capture answers
 * CANCELLED, the shared command then prints nothing (shell/test/
 * test_camera_cmd.c holds that half).  Which frame the operator's Ctrl+C lands
 * on cannot be chosen at a console; the fake driver asks the cancel question
 * where the real one does.  The report's numbers -- B, G, R at 8 bits, the
 * seam of G alone, this board's own values and lines -- are checked against
 * the same frame.
 *
 * The real cam_convert.c computes the numbers (its own test is
 * test_cam_convert.c); camera_svc_shim/ stands in for tx_api.h and the one SDK
 * type cam_sensor.h names.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "tx_api.h"
#include "camera.h"
#include "cam_dp.h"   /* cam_dp_bayer(), _name(): the fakes below match these */
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

#define HAS(s) (strstr(out, (s)) != NULL)

/* ---- what the console got -------------------------------------------------- */

static char out[2048];
static size_t out_len;
static int write_calls;

static int cap_write(void *ctx, const char *s, size_t len)
{
	(void)ctx;
	write_calls++;
	if (out_len + len < sizeof out) {
		memcpy(out + out_len, s, len);
		out_len += len;
		out[out_len] = '\0';
	}
	return (int)len;
}

static void out_clear(void)
{
	out_len = 0u;
	out[0] = '\0';
	write_calls = 0;
}

/* ---- the fake driver ----------------------------------------------------- */

static uint8_t fake_raw[CAM_RAW_BYTES];
static uint32_t fake_warm_frames;   /* frames the fake lets go          */
static int fake_cancel_at;          /* the Nth question says yes (0: never) */
static int fake_questions;
static int fake_settle_calls;
static size_t fake_out_at_settle;   /* what had been written when it began */
static TX_THREAD fake_thread, other_thread;
static TX_THREAD *fake_current = &fake_thread;   /* the calling thread */

TX_THREAD *tx_thread_identify(void) { return fake_current; }

int camera_capture_settled(uint32_t warm, uint32_t *dropped,
                           int (*cancelled)(void *ctx), void *ctx)
{
	(void)warm;
	fake_settle_calls++;
	fake_out_at_settle = out_len;
	*dropped = 0u;
	/* the real one asks between the frames it lets go */
	for (uint32_t k = 0; k < fake_warm_frames; k++) {
		fake_questions++;
		if (cancelled != NULL && cancelled(ctx))
			return CAM_ERR_CANCELLED;
		(*dropped)++;
	}
	return CAM_OK;
}

const uint8_t *camera_raw_frame(void) { return fake_raw; }
uint8_t cam_dp_bayer(void) { return 2u; }
const char *cam_dp_bayer_name(uint8_t pattern)
{
	return pattern == 2u ? "GBRG" : "?";
}
const char *camera_strerror(int rc) { (void)rc; return "fake error"; }
void camera_stream_stats(struct camera_stats *st)
{
	memset(st, 0, sizeof *st);   /* no latched reason, no datapath status */
}

/* ---- the cancel question the shared command answers ----------------------- */

static int ask_cancel(void *ctx)
{
	(void)ctx;
	return fake_cancel_at != 0 && fake_questions >= fake_cancel_at;
}

static void reset(void)
{
	out_clear();
	fake_warm_frames = 3u;
	fake_cancel_at = 0;
	fake_questions = 0;
	fake_settle_calls = 0;
	fake_out_at_settle = 0u;
}

static int capture(int test, struct camera_svc_capture_report *rep)
{
	struct camera_op_result res;

	memset(rep, 0, sizeof *rep);
	memset(&res, 0, sizeof res);
	camera_svc_capture(test, rep, &res, cap_write, ask_cancel, NULL);
	return res.status;
}

/* ---- the announcement ---------------------------------------------------- */

static void test_announced_first(void)
{
	struct camera_svc_capture_report rep;

	reset();
	EQ(capture(0, &rep), CAMERA_SVC_OK, "a capture");
	EQ(strcmp(out, "camera: capturing live frame ...\r\n"), 0,
	   "the announcement, f746g-disco's sentence");
	EQ(fake_out_at_settle, strlen("camera: capturing live frame ...\r\n"),
	   "written before the frames are let go");
	EQ(rep.warmup, 3u, "the frames it let go");
	EQ(rep.warmup_unit, CAMERA_SVC_WARMUP_FRAMES, "counted in frames");
}

/* [!] a Ctrl+C among the frames let go: the announcement and nothing else */
static void test_cancel_leaves_the_announcement(void)
{
	struct camera_svc_capture_report rep;

	for (int at = 1; at <= 3; at++) {
		reset();
		fake_cancel_at = at;
		EQ(capture(0, &rep), CAMERA_SVC_ERR_CANCELLED, "cancelled");
		EQ(strcmp(out, "camera: capturing live frame ...\r\n"), 0,
		   "the announcement only");
		EQ(write_calls, 1, "one line written");
		EQ(rep.warmup_unit, CAMERA_SVC_WARMUP_NONE, "no warm-up answered");
	}
}

/* a test pattern is refused before it is announced */
static void test_refused_unannounced(void)
{
	struct camera_svc_capture_report rep;

	reset();
	EQ(capture(1, &rep), CAMERA_SVC_ERR_NOSUP, "no test pattern");
	EQ(out_len, 0u, "nothing announced");
	EQ(fake_settle_calls, 0, "nothing captured");
}

/* ---- the report's numbers ------------------------------------------------- */

static void test_report(void)
{
	struct camera_svc_capture_report rep;
	struct camera_op_result res;

	reset();
	for (uint32_t k = 0; k < 3u; k++)
		memset(fake_raw + k * CAM_FRAME_PIXELS, (int)(10u * (k + 1u)),
		       CAM_FRAME_PIXELS);
	fake_raw[CAM_FRAME_PIXELS] = 200u;   /* one bright G pixel */
	EQ(capture(0, &rep), CAMERA_SVC_OK, "a capture");
	memset(&res, 0, sizeof res);
	camera_svc_capture_report(&rep, &res, ask_cancel, NULL);
	EQ(res.status, CAMERA_SVC_OK, "the report");
	EQ(rep.warmup, 3u, "the warm-up is the capture's");
	EQ(strcmp(rep.format, "planar B/G/R"), 0, "the format's words");
	EQ(rep.width, CAM_FRAME_WIDTH, "width");
	EQ(rep.height, CAM_FRAME_HEIGHT, "height");
	EQ(rep.bytes, CAM_RAW_BYTES, "bytes");
	EQ(rep.nchan, 3u, "three planes");
	EQ(strcmp(rep.chan[0].name, "B"), 0, "B first, the hardware's order");
	EQ(strcmp(rep.chan[1].name, "G"), 0, "then G");
	EQ(strcmp(rep.chan[2].name, "R"), 0, "then R");
	EQ(rep.chan[0].bits, 8u, "8 bits");
	EQ(rep.chan[0].min, 10u, "B min");
	EQ(rep.chan[1].max, 200u, "G max");
	EQ(rep.chan[2].mean_x100, 3000u, "R mean 30.00");
	EQ(rep.nseam, 1u, "the seam is G's alone");
	EQ(strcmp(rep.seam[0].name, "G"), 0, "dG");
	EQ(rep.seam_row, 1u, "at the row after the bright pixel's");
	EQ(rep.head_len, CAMERA_SVC_HEAD_MAX, "a head");
	EQ(memcmp(rep.head, fake_raw, CAMERA_SVC_HEAD_MAX), 0, "the frame's first bytes");

	out_clear();
	EQ(camera_svc_capture_board_lines(&rep, cap_write, NULL), 0, "the board's lines");
	EQ(HAS("mosaic   : B "), 1, "the mosaic line");
	EQ(HAS("bayer    : GBRG   colour separation "), 1, "the Bayer line");
	EQ(HAS("(mosaic: single digits = a demosaic ran. colour: bigger = the "
	       "phase fits)\r\n"), 1, "and how to read them");
	EQ(HAS("(single digits = settled)"), 0, "the seam's old note is gone");

	/* a cancelled capture does not replace the record: the last finished
	   one still reports */
	reset();
	fake_cancel_at = 1;
	(void)capture(0, &rep);
	memset(&res, 0, sizeof res);
	camera_svc_capture_report(&rep, &res, ask_cancel, NULL);
	EQ(res.status, CAMERA_SVC_OK, "the last finished capture still reports");

	/* another console's: the record is this thread's, so another thread's
	   report fails rather than print a capture it did not take */
	fake_current = &other_thread;
	memset(&res, 0, sizeof res);
	camera_svc_capture_report(&rep, &res, ask_cancel, NULL);
	EQ(res.status, CAMERA_SVC_ERR_NO_FRAME, "another thread's report fails");
	EQ(strcmp(res.detail, "frame changed by a concurrent capture"), 0,
	   "in the board's words");
	fake_current = &fake_thread;
}

/* Before any capture the record is not valid: the report fails.  Run first,
   while the adapter's record is still the zero it starts as.  The calling
   thread is made the zero record's owner (NULL), so only the validity can
   refuse it. */
static void test_report_before_any_capture(void)
{
	struct camera_svc_capture_report rep;
	struct camera_op_result res;

	memset(&rep, 0, sizeof rep);
	memset(&res, 0, sizeof res);
	fake_current = NULL;
	camera_svc_capture_report(&rep, &res, ask_cancel, NULL);
	fake_current = &fake_thread;
	EQ(res.status, CAMERA_SVC_ERR_NO_FRAME, "no capture yet: the report fails");
	EQ(rep.head_len, 0u, "and hands back no head");
}

int main(void)
{
	test_report_before_any_capture();
	test_announced_first();
	test_cancel_leaves_the_announcement();
	test_refused_unannounced();
	test_report();

	if (fails) {
		printf("test_camera_svc_grove: %d of %d FAILED\n", fails, checks);
		return 1;
	}
	printf("test_camera_svc_grove: all %d checks passed\n", checks);
	return 0;
}
