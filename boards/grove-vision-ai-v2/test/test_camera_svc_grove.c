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
 * [!] AND THE NUMBERS ARE THE FRAME THE CAPTURE HELD (issue #245).  The real
 * driver hands the frame to the adapter's reader while it still holds the
 * camera's API mutex, and lets go only after; the next command that writes
 * the landing buffer can start the moment it lets go.  The fake does the same
 * in that order -- reader, then "release", then another writer's frame over
 * the buffer and another Bayer phase -- so a report that reads the buffer
 * after the capture has returned shows the second frame and fails here.
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

/* issue #245: what the fake does around the reader */
static int fake_rc;                 /* what the capture answers (CAM_OK)    */
static int fake_reads;              /* times the reader was called          */
static int fake_skip_read;          /* answer CAM_OK without calling it     */
static int fake_read_on_failure;    /* call it even when answering an error */
static int fake_overwrite;          /* another writer after the "release"   */
static uint8_t fake_bayer = 2u;

TX_THREAD *tx_thread_identify(void) { return fake_current; }

/* Another command's frame landing in the same buffer, and another phase. */
static void fake_other_writer(void)
{
	memset(fake_raw, 0xEE, sizeof fake_raw);
	fake_bayer = 1u;
}

int camera_capture_settled(uint32_t warm, uint32_t *dropped,
                           int (*cancelled)(void *ctx), void *ctx,
                           camera_frame_reader_fn read, void *read_arg)
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
	/* The real one: the reader on success only, the mutex still held. */
	if (read != NULL && !fake_skip_read &&
	    (fake_rc == CAM_OK || fake_read_on_failure)) {
		fake_reads++;
		read(fake_raw, read_arg);
	}
	/* ...and here it lets go, after which anyone may write the buffer. */
	if (fake_overwrite)
		fake_other_writer();
	return fake_rc;
}

/* No camera_raw_frame() here (issue #245): the adapter reads the frame only
   through its reader, so a call to it would not link. */
uint8_t cam_dp_bayer(void) { return fake_bayer; }
const char *cam_dp_bayer_name(uint8_t pattern)
{
	return pattern == 2u ? "GBRG" : pattern == 1u ? "GRBG" : "?";
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
	fake_rc = CAM_OK;
	fake_reads = 0;
	fake_skip_read = 0;
	fake_read_on_failure = 0;
	fake_overwrite = 0;
	fake_bayer = 2u;
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

/* ---- issue #245: the frame the capture held ------------------------------ */

/* Frame A: B 10, G 20 with one bright pixel, R 30, and a recognisable head. */
static void fill_frame_a(void)
{
	for (uint32_t k = 0; k < 3u; k++)
		memset(fake_raw + k * CAM_FRAME_PIXELS, (int)(10u * (k + 1u)),
		       CAM_FRAME_PIXELS);
	fake_raw[CAM_FRAME_PIXELS] = 200u;
	for (uint32_t i = 0; i < CAMERA_SVC_HEAD_MAX; i++)
		fake_raw[i] = (uint8_t)(i + 1u);
}

static void report(struct camera_svc_capture_report *rep, int *status)
{
	struct camera_op_result res;

	memset(&res, 0, sizeof res);
	camera_svc_capture_report(rep, &res, ask_cancel, NULL);
	*status = res.status;
}

/* [!] the buffer is overwritten the moment the capture lets go: the report
   still prints frame A, every number of it */
static void test_report_is_the_held_frame(void)
{
	struct camera_svc_capture_report rep;
	uint8_t head_a[CAMERA_SVC_HEAD_MAX];
	int status;

	reset();
	fill_frame_a();
	memcpy(head_a, fake_raw, sizeof head_a);
	fake_overwrite = 1;
	EQ(capture(0, &rep), CAMERA_SVC_OK, "a capture");
	EQ(fake_reads, 1, "the reader ran once, inside the capture");
	EQ(fake_raw[0], 0xEEu, "and the buffer was overwritten after it");

	report(&rep, &status);
	EQ(status, CAMERA_SVC_OK, "the report");
	EQ(rep.chan[0].min, 1u, "B min: frame A's head starts at 1");
	EQ(rep.chan[0].max, 32u, "B max: frame A's head ends at 32");
	EQ(rep.chan[1].min, 20u, "G min: frame A, not 0xEE");
	EQ(rep.chan[1].max, 200u, "G max: frame A's bright pixel");
	EQ(rep.chan[2].min, 30u, "R min: frame A");
	EQ(rep.chan[2].mean_x100, 3000u, "R mean 30.00: frame A");
	EQ(rep.seam_row, 1u, "the seam: frame A's bright pixel");
	EQ(memcmp(rep.head, head_a, CAMERA_SVC_HEAD_MAX), 0,
	   "the head: frame A's first bytes");
	EQ(rep.board[3], 2u, "the Bayer phase the frame was taken with");

	out_clear();
	EQ(camera_svc_capture_board_lines(&rep, cap_write, NULL), 0,
	   "the board's lines");
	EQ(HAS("bayer    : GBRG   colour separation "), 1,
	   "the phase of frame A, not the one set after");
}

/* No reader, no record: a failed, cancelled or unread capture leaves the
   last finished one as it was. */
static void test_no_read_no_record(void)
{
	static const int fail_rc[] = {
		CAM_ERR_TIMEOUT, CAM_ERR_HAL, CAM_ERR_BUSY, CAM_ERR_STATE,
	};
	struct camera_svc_capture_report rep;
	uint8_t head_a[CAMERA_SVC_HEAD_MAX];
	int status;

	/* frame A is the last finished capture */
	reset();
	fill_frame_a();
	memcpy(head_a, fake_raw, sizeof head_a);
	EQ(capture(0, &rep), CAMERA_SVC_OK, "frame A");

	for (unsigned i = 0; i < sizeof fail_rc / sizeof fail_rc[0]; i++) {
		/* as the real driver: an error and no reader */
		reset();
		fake_other_writer();
		fake_rc = fail_rc[i];
		EQ(capture(0, &rep) != CAMERA_SVC_OK, 1, "the capture fails");
		EQ(fake_reads, 0, "the reader was not called");
		report(&rep, &status);
		EQ(status, CAMERA_SVC_OK, "the last finished capture reports");
		EQ(rep.chan[1].max, 200u, "and it is still frame A");

		/* an error even if a reader ran: still not installed */
		reset();
		fake_other_writer();
		fake_rc = fail_rc[i];
		fake_read_on_failure = 1;
		EQ(capture(0, &rep) != CAMERA_SVC_OK, 1, "the capture fails");
		EQ(fake_reads, 1, "the (misbehaving) fake did read");
		report(&rep, &status);
		EQ(rep.chan[1].max, 200u, "an error installs nothing");
		EQ(memcmp(rep.head, head_a, CAMERA_SVC_HEAD_MAX), 0,
		   "frame A's head");
	}

	/* cancelled among the frames let go: the reader never runs */
	reset();
	fake_other_writer();
	fake_cancel_at = 1;
	EQ(capture(0, &rep), CAMERA_SVC_ERR_CANCELLED, "cancelled");
	EQ(fake_reads, 0, "no reader on a cancel");
	report(&rep, &status);
	EQ(rep.chan[1].max, 200u, "a cancel installs nothing");

	/* a success the reader never saw: refused, nothing installed */
	reset();
	fake_other_writer();
	fake_skip_read = 1;
	EQ(capture(0, &rep), CAMERA_SVC_ERR_HW, "an unread success fails closed");
	report(&rep, &status);
	EQ(status, CAMERA_SVC_OK, "the last finished capture reports");
	EQ(rep.chan[1].max, 200u, "and it is still frame A");
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
	test_report_is_the_held_frame();
	test_no_read_no_record();

	if (fails) {
		printf("test_camera_svc_grove: %d of %d FAILED\n", fails, checks);
		return 1;
	}
	printf("test_camera_svc_grove: all %d checks passed\n", checks);
	return 0;
}
