/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    camera_svc_grove.c
 * @brief   This board behind the shared `camera` command's contract
 *          (issue #143).
 *
 * svc/camera_svc.h is what shell/cmds/cmd_camera.c speaks; port/camera is what
 * this board has.  Only the operations this board's camera_svc_config.h says it
 * has are implemented -- probe, capture and its report, and the stream -- and
 * the shared command calls nothing else (an implementation it would call and
 * this file lacks is a link error, not a silent no-op).
 *
 * [!] NO SHELL INSTANCE.  The sentences cmds/cmd_camera.c used to print are
 * written through the shared command's writer, one whole line per write, or --
 * a failure -- formatted into the caller's result.  A failure here is often
 * several lines (the reason the driver latched, then what to do about it), and
 * they travel together as one detail (CAMERA_SVC_DETAIL_MAX).
 *
 * [!] THE DATAPATH AND THE STOP ORDER ARE NOT THIS FILE'S.  Every call below is
 * one the old command made, in the order it made them: camera_capture_settled()
 * is the one-shot (camera_capture() letting the sensor settle first, #143)
 * through the producer's own teardown (#129), the stream is
 * cam_lcd_sink_hold() / _release() (#142), and nothing here touches WDMA3,
 * the bus owner or the stop sequence.
 *
 * Clean-room design; no third-party code reused.
 */
#include "camera_svc.h"
#include "camera_svc_adapter.h"
#include "camera_svc_config.h"
#include "camera_svc_grove.h"

#include <stdarg.h>
#include <stdint.h>
#include <string.h>

#include "tx_api.h"       /* TX_DISABLE, tx_thread_identify(): the record */

#include "cam_convert.h"
#include "cam_dp.h"
#include "cam_lcd_sink.h"
#include "cam_sensor.h"
#include "cam_state.h"    /* CAM_ST_*: `camera info` names the state */
#include "camera.h"
#include "fmt.h"
#include "nn_camera.h"    /* the sink row's stage split under `nn stream` */

/* ---- results ------------------------------------------------------------- */

/* Append to the detail already in @p res (fmt_vsnformat stops at the end). */
static void cam_detail_add(struct camera_op_result *res, const char *f, ...)
	__attribute__((format(printf, 2, 3)));
static void cam_detail_add(struct camera_op_result *res, const char *f, ...)
{
	size_t len = strlen(res->detail);
	va_list ap;

	if (len + 1u >= sizeof res->detail)
		return;
	va_start(ap, f);
	(void)fmt_vsnformat(res->detail + len, sizeof res->detail - len, f, ap);
	va_end(ap);
}

/*
 * "<what> failed: <reason>", and under it the reason the driver latched and
 * the last datapath status, when there are any -- the old cam_report().  The
 * driver latches a specific reason for every failure it can name, and making
 * the operator run `camera stats` to see it would hide the one useful
 * sentence behind a second command.
 */
static void cam_res_report(struct camera_op_result *res, int status,
                           const char *what, int rc)
{
	struct camera_stats st;

	camera_res_fail(res, status, "%s failed: %s", what, camera_strerror(rc));
	camera_stream_stats(&st);
	if (st.fault != NULL)
		cam_detail_add(res, "\r\n        %s", st.fault);
	if (st.last_dp_status != 0)
		cam_detail_add(res, "\r\n        last datapath status %ld",
		               (long)st.last_dp_status);
}

static int cam_status(int rc)
{
	switch (rc) {
	case CAM_ERR_PARAM:  return CAMERA_SVC_ERR_ARG;
	case CAM_ERR_STATE:  return CAMERA_SVC_ERR_STATE;
	case CAM_ERR_BUSY:   return CAMERA_SVC_ERR_BUSY;
	case CAM_ERR_LOCKED: return CAMERA_SVC_ERR_BUSY;
	case CAM_ERR_NO_FRAME: return CAMERA_SVC_ERR_NO_FRAME;
	/* CAM_ERR_TIMEOUT on a capture is "no frame arrived" after the bounded
	   restarts: nothing finishes later, so HW, not the contract's TIMEOUT. */
	default:             return CAMERA_SVC_ERR_HW;
	}
}

/* ---- probe --------------------------------------------------------------- */

void camera_svc_probe(struct camera_op_result *res,
                      camera_svc_write_fn write, void *ctx)
{
	struct camera_out o = { write, ctx, 0 };
	struct camera_probe_info info;
	int rc = camera_probe(&info);

	if (rc != CAM_OK) {
		cam_res_report(res, cam_status(rc), "probe", rc);
		/*
		 * "camera busy" on its own reads like something to retry, and
		 * retrying is the one thing that will not help (issue #74).
		 * Deliberately naming BOTH causes: this code comes back either
		 * from the streaming refusal or from cam_api_enter() failing to
		 * take the API mutex, and the two are indistinguishable here.
		 */
		if (rc == CAM_ERR_BUSY)
			cam_detail_add(res, "\r\n        a stream is running (the "
			               "producer owns the sensor's I2C bus), or "
			               "another\r\n        camera command holds the "
			               "API; `camera stream stats` says which");
		return;
	}
	camera_res_ok(res);
	camera_out(&o, "chip     : %08lx%s\r\n",
	           (unsigned long)info.chip_version,
	           info.rev_c ? " (rev C: per-frame MIPI bounce)" : "");
	camera_out(&o, "sensor   : %s (id 0x%04X, on-chip AEC)\r\n",
	           cam_sensor_name(), info.sensor_id);
	camera_out(&o, "frame    : %ux%u planar B/G/R, %lu bytes\r\n",
	           (unsigned)CAM_FRAME_WIDTH, (unsigned)CAM_FRAME_HEIGHT,
	           (unsigned long)CAM_RAW_BYTES);
}

/* ---- one-shot capture ----------------------------------------------------- */

/*
 * [!] THE FRAME IS DESCRIBED THE MOMENT THE CAPTURE RETURNS, inside
 * camera_svc_capture(), as the old command did it -- before anything else can
 * reuse the landing buffer -- and the description is what the report prints.
 * The record names the thread that captured it: a report prints it only for
 * that thread, so a console never prints another console's capture (A
 * captures, B captures, A's report would otherwise show B's frame).
 *
 * Board code, so it may keep state; two consoles may capture at once, so it is
 * written and read whole inside a ThreadX critical section.  CPU-only data:
 * plain .bss, which this board's ldscript puts in DTCM.
 */
struct cam_plane_rec {
	uint8_t  min, max;
	uint32_t mean_x100;
	uint32_t mosaic_x100;
};

struct cam_capture_rec {
	uint8_t  valid;
	TX_THREAD *owner;
	struct cam_plane_rec plane[3];   /* B, G, R: the hardware's order */
	uint32_t seam_x100, seam_row;
	uint32_t colour_x100;
	uint8_t  bayer;
	uint8_t  head[CAMERA_SVC_HEAD_MAX];
};

static struct cam_capture_rec cam_last_capture;

static void cam_plane_rec_of(struct cam_plane_rec *p, const uint8_t *plane)
{
	struct cam_plane_stats st;

	cam_plane_stats(plane, CAM_FRAME_PIXELS, &st);
	p->min = st.min;
	p->max = st.max;
	p->mean_x100 = st.mean_x100;
	p->mosaic_x100 = cam_plane_mosaic_x100(plane, CAM_FRAME_WIDTH,
	                                       CAM_FRAME_HEIGHT);
}

/* A capture right after bring-up lets the sensor's AEC/AGC settle first, by
   the same count as a one-shot `nn run` (issue #115) -- one constant, checked
   here rather than trusted. */
_Static_assert(CAMERA_ONESHOT_WARM_FRAMES == NN_ONESHOT_WARM_FRAMES,
               "camera capture and nn run settle the sensor by the same count");

void camera_svc_capture(int test, struct camera_svc_capture_report *rep,
                        struct camera_op_result *res,
                        camera_svc_write_fn write,
                        int (*cancelled)(void *ctx), void *ctx)
{
	struct cam_capture_rec r;
	const uint8_t *raw;
	uint32_t dropped = 0u;
	int rc;
	TX_INTERRUPT_SAVE_AREA

	(void)write;   /* no announcement on this board, as before */
	if (test) {   /* CAMERA_SVC_HAS_TEST 0: the shared command refuses first */
		camera_res_fail(res, CAMERA_SVC_ERR_NOSUP, "no test pattern");
		return;
	}
	/* Frames since bring-up at or below the count are let go, the
	   operator's Ctrl+C asked between them (issue #143). */
	rc = camera_capture_settled(CAMERA_ONESHOT_WARM_FRAMES, &dropped,
	                            cancelled, ctx);
	if (rc == CAM_ERR_CANCELLED) {
		camera_res_fail(res, CAMERA_SVC_ERR_CANCELLED, "cancelled");
		return;
	}
	if (rc != CAM_OK) {
		cam_res_report(res, cam_status(rc), "capture", rc);
		return;
	}
	rep->warmup = dropped;
	rep->warmup_unit = (uint8_t)CAMERA_SVC_WARMUP_FRAMES;

	memset(&r, 0, sizeof r);
	raw = camera_raw_frame();
	/* The plane ORDER is the hardware's, not a preference: WDMA3 writes B
	   first.  Printing them in that order is what makes a swapped pair
	   visible against a scene somebody can describe. */
	for (unsigned k = 0; k < 3u; k++)
		cam_plane_rec_of(&r.plane[k], raw + k * CAM_FRAME_PIXELS);
	r.seam_x100 = cam_plane_row_seam_x100(raw + CAM_FRAME_PIXELS,
	                                      CAM_FRAME_WIDTH, CAM_FRAME_HEIGHT,
	                                      &r.seam_row);
	r.colour_x100 = cam_frame_colour_x100(raw, CAM_FRAME_PIXELS);
	r.bayer = cam_dp_bayer();
	memcpy(r.head, raw, CAMERA_SVC_HEAD_MAX);
	r.owner = tx_thread_identify();
	r.valid = 1u;

	TX_DISABLE
	cam_last_capture = r;
	TX_RESTORE
	camera_res_ok(res);
}

/*
 * The two figures answer DIFFERENT questions, and conflating them was a
 * mistake worth not repeating:
 *
 *   mosaic  -- did a demosaic run at all?  Single digits yes, tens no.  It
 *              says NOTHING about whether the phase was right.
 *   colour  -- was the phase right?  A wrong phase interpolates red and blue
 *              from where they are not, dragging both toward green, so the
 *              frame desaturates.  Try all four `camera bayer` values on
 *              something strongly coloured: the right one maximises this.
 *
 * Nothing here reads the landing buffer, so there is nothing to cancel.
 */
void camera_svc_capture_report(struct camera_svc_capture_report *rep,
                               struct camera_op_result *res,
                               camera_svc_write_fn write,
                               int (*cancelled)(void *ctx), void *ctx)
{
	static const char *const name[3] = {
		"B(plane 0)", "G(plane 1)", "R(plane 2)"
	};
	struct camera_out o = { write, ctx, 0 };
	struct cam_capture_rec r;
	TX_INTERRUPT_SAVE_AREA

	(void)cancelled;
	rep->head_len = 0u;   /* the warm-up is the capture's (left alone) */
	TX_DISABLE
	r = cam_last_capture;
	TX_RESTORE
	if (!r.valid || r.owner != tx_thread_identify()) {
		camera_res_fail(res, CAMERA_SVC_ERR_NO_FRAME,
		                "frame changed by a concurrent capture");
		return;
	}
	camera_res_ok(res);

	camera_out(&o, "frame    : %ux%u planar B/G/R\r\n",
	           (unsigned)CAM_FRAME_WIDTH, (unsigned)CAM_FRAME_HEIGHT);
	for (unsigned k = 0; k < 3u; k++)
		camera_out(&o, "%s : min %3u  max %3u  mean %3lu.%02lu  "
		           "mosaic %3lu.%02lu\r\n", name[k],
		           r.plane[k].min, r.plane[k].max,
		           (unsigned long)(r.plane[k].mean_x100 / 100u),
		           (unsigned long)(r.plane[k].mean_x100 % 100u),
		           (unsigned long)(r.plane[k].mosaic_x100 / 100u),
		           (unsigned long)(r.plane[k].mosaic_x100 % 100u));
	camera_out(&o, "seam     : row %3lu  dG %lu.%02lu  "
	           "(single digits = settled)\r\n",
	           (unsigned long)r.seam_row, (unsigned long)(r.seam_x100 / 100u),
	           (unsigned long)(r.seam_x100 % 100u));
	camera_out(&o, "bayer    : %s   colour separation %lu.%02lu\r\n",
	           cam_dp_bayer_name(r.bayer),
	           (unsigned long)(r.colour_x100 / 100u),
	           (unsigned long)(r.colour_x100 % 100u));
	camera_out(&o, "           (mosaic: single digits = a demosaic ran. "
	           "colour: bigger = the phase fits)\r\n");

	memcpy(rep->head, r.head, CAMERA_SVC_HEAD_MAX);
	rep->head_len = CAMERA_SVC_HEAD_MAX;
}

/* ---- the stream (issue #142: the operator's hold on the panel stream) ---- */

void camera_svc_stream_start(int test, struct camera_op_result *res,
                             int *already,
                             camera_svc_write_fn write, void *ctx)
{
	struct camera_out o = { write, ctx, 0 };
	int started = 0, rc;

	*already = 0;
	if (test) {   /* CAMERA_SVC_HAS_TEST 0: the shared command refuses first */
		camera_res_fail(res, CAMERA_SVC_ERR_NOSUP, "no test pattern");
		return;
	}
	rc = cam_lcd_sink_hold(NULL, NULL, &started);
	if (rc == CAM_ERR_BUSY) {
		camera_res_fail(res, CAMERA_SVC_ERR_BUSY,
		                "busy -- a stream start or stop, `camera bench`, "
		                "or another command owns the camera; retry");
		return;
	}
	if (rc != CAM_OK) {
		cam_res_report(res, cam_status(rc), "stream start", rc);
		return;
	}
	camera_res_ok(res);
	*already = !started;
	camera_out(&o, "camera: streaming to the panel%s -- `camera stream stats` / "
	           "`camera stream stop`\r\n",
	           started ? "" : " (already running)");
}

/*
 * The operator lets go.  [!] While nn holds the stream this is REFUSED and
 * changes nothing (issue #142 D8).  The last holder's leave stops the camera
 * and, only on a confirmed stop, unlinks the panel sink (issue #48) -- so a
 * success is a confirmed stop.
 */
int camera_svc_stream_stop(struct camera_op_result *res, uint32_t *gen)
{
	const char *users[CAMERA_SVC_USERS_MAX];
	int n = 0;
	int rc = cam_lcd_sink_release(NULL, users, (int)CAMERA_SVC_USERS_MAX, &n);

	*gen = 0u;
	if (rc == CAM_LCD_ERR_IN_USE) {
		camera_res_in_use(res, users, n);
		return CAMERA_SVC_STOP_NONE;
	}
	if (rc == CAM_ERR_BUSY) {
		camera_res_fail(res, CAMERA_SVC_ERR_BUSY,
		                "a stream start or stop is in progress, or the "
		                "panel has not let go yet -- retry");
		return CAMERA_SVC_STOP_NONE;
	}
	if (rc != CAM_OK) {
		/* A stop that did not confirm, said accurately (issue #65): the
		   failures leave the board in different places.  A locked stop
		   changed nothing and can be typed again (the holder table still
		   has the operator); the other two leave a camera only a reboot
		   recovers. */
		int status = (rc == CAM_ERR_LOCKED) ? CAMERA_SVC_ERR_BUSY
		           : (rc == CAM_ERR_TIMEOUT || rc == CAM_ERR_STATE)
		                     ? CAMERA_SVC_ERR_LOST : CAMERA_SVC_ERR_HW;

		cam_res_report(res, status, "stream stop", rc);
		if (rc == CAM_ERR_LOCKED)
			cam_detail_add(res, "\r\n        nothing was asked of the "
			               "producer and nothing was torn down -- retry");
		else if (rc == CAM_ERR_TIMEOUT)
			cam_detail_add(res, "\r\n        the producer is still "
			               "running; the camera is unusable until reboot");
		else if (rc == CAM_ERR_STATE)
			cam_detail_add(res, "\r\n        the camera is unusable until "
			               "reboot");
		return CAMERA_SVC_STOP_NONE;
	}
	camera_res_ok(res);
	return CAMERA_SVC_STOP_DONE;
}

/* ---- the counters -------------------------------------------------------- */

/*
 * One line of the stage profile (issue #38).  Microseconds and tenths of a
 * percent, both assembled from integers because svc/fmt.c has no %f -- and the
 * percentages are of prof_total_us, measured over the same window as the
 * parts, so the column adds up.
 */
static void cam_prof_line(struct camera_out *o, const char *name,
                          uint32_t us, uint32_t total_us, uint32_t iters,
                          const char *what)
{
	uint32_t pct10 = (total_us != 0u)
	               ? (uint32_t)(((uint64_t)us * 1000u) / total_us) : 0u;
	uint32_t per   = (iters != 0u) ? (us / iters) : 0u;

	camera_out(o, "  %-7s: %6lu us/frame  %2lu.%lu%%   %s\r\n",
	           name, (unsigned long)per,
	           (unsigned long)(pct10 / 10u), (unsigned long)(pct10 % 10u),
	           what);
}

/*
 * The producer's per-stage breakdown, since the last stream start.  [!] The
 * point of this is the RATIO, not the fps: if `wait` dominates, the sensor is
 * not offering frames any faster and pipelining the CPU work would buy nothing;
 * if `pack` and `sink` dominate, it would (issue #38).
 */
void camera_grove_profile_write(struct camera_out *o,
                                const struct camera_stats *st)
{
	uint32_t fps10;

	if (!st->prof_ok) {
		camera_out(o, "profile  : -- (%s)\r\n",
		           st->prof_why != NULL ? st->prof_why : "no time source");
		return;
	}
	if (st->prof_iters == 0u || st->prof_total_us == 0u) {
		camera_out(o, "profile  : -- (no frames since the last stream "
		           "start)\r\n");
		return;
	}

	fps10 = (uint32_t)(((uint64_t)st->prof_iters * 10000000u) /
	                   st->prof_total_us);
	camera_out(o, "profile  : %lu iterations, %lu us/frame (%lu.%lu fps)"
	           "  [since stream start]\r\n",
	           (unsigned long)st->prof_iters,
	           (unsigned long)(st->prof_total_us / st->prof_iters),
	           (unsigned long)(fps10 / 10u), (unsigned long)(fps10 % 10u));

	/* In LOOP order (issue #59 rearranged it): the arm goes ahead of the
	 * work, and tune ahead of pack, so the rows read as the iteration runs. */
	cam_prof_line(o, "wait", st->prof_wait_us, st->prof_total_us,
	              st->prof_iters, "asleep: sensor exposure + readout");
	cam_prof_line(o, "invald", st->prof_inval_us, st->prof_total_us,
	              st->prof_iters, "D-cache invalidate, one buffer, 225 KB");
	/* The next capture is running from here down: this row is the price of
	 * the overlap, and the one to watch if `wait` ever grows. */
	cam_prof_line(o, "arm", st->prof_arm_us, st->prof_total_us,
	              st->prof_iters, "arm next capture + wrap reassert");
	cam_prof_line(o, "tune", st->prof_tune_us, st->prof_total_us,
	              st->prof_iters, "means + sensor read-back + wb");
	cam_prof_line(o, "pack", st->prof_pack_us, st->prof_total_us,
	              st->prof_iters, "planar B/G/R -> RGB565, 76800 px");
	/* NOT the blit -- that left this thread in #57 and has its own row.
	 * What is left is whatever the sinks do on the producer: the frame's
	 * preparation for the inference worker under `nn stream` (#129), and
	 * the panel thread's staging copy (#64). */
	cam_prof_line(o, "sink", st->prof_sink_us, st->prof_total_us,
	              st->prof_iters, "sinks consume: nn prep + panel staging");
	cam_prof_line(o, "other", st->prof_other_us, st->prof_total_us,
	              st->prof_iters, "verify, error recheck, loop");
}

/*
 * `camera stream stats` (issue #143 decision 4: the board's `camera stats`
 * moved here, its stage profile to `camera profile`): ONE sample of the camera and of the panel sink, the head handed the
 * state and holders from it, then every other line from the same sample.  The
 * holders and the nn stage split are separate looks, as they always were.
 */
void camera_svc_stream_report(struct camera_op_result *res,
                              camera_svc_write_fn write,
                              camera_svc_state_fn head, void *ctx)
{
	struct camera_out o = { write, ctx, 0 };
	struct cam_lcd_sink_stats sk;
	struct camera_stats st;
	struct camera_svc_state ss;
	int lost = 0;
	int n;

	camera_stream_stats(&st);
	cam_lcd_sink_stats(&sk);
	camera_res_ok(res);

	memset(&ss, 0, sizeof ss);
	ss.streaming = st.streaming ? 1u : 0u;
	/* Who holds the stream to the panel (issue #142): a snapshot taken
	   under the holder lock.  Negative while a start or stop is in flight. */
	n = cam_lcd_sink_holders(ss.users, (int)CAMERA_SVC_USERS_MAX, &lost);
	if (n < 0)
		ss.transition = 1u;
	else
		ss.nusers = (unsigned)n;
	ss.lost = (lost > 0) ? (uint32_t)lost : 0u;
	head(ctx, &ss);

	camera_out(&o, "frames   : %lu\r\n", (unsigned long)st.frames);
	camera_out(&o, "timeouts : %lu (restarts %lu)\r\n",
	           (unsigned long)st.timeouts, (unsigned long)st.retries);
	camera_out(&o, "overruns : %lu\r\n", (unsigned long)st.overruns);
	camera_out(&o, "csirx err: %lu (relock fail %lu)\r\n",
	           (unsigned long)st.csirx_errors,
	           (unsigned long)st.relock_fails);
	if (st.last_dp_status != 0)
		camera_out(&o, "dp errors: %lu (last status %ld)\r\n",
		           (unsigned long)st.dp_errors, (long)st.last_dp_status);
	else
		camera_out(&o, "dp errors: %lu\r\n", (unsigned long)st.dp_errors);
	/* Stops that found the API locked (issue #65).  Zero is the normal
	   answer and the line is still worth printing: a collision leaves no
	   other trace. */
	camera_out(&o, "stop lock: %lu contended (longest wait %lu ms)\r\n",
	           (unsigned long)st.lock_contended,
	           (unsigned long)st.lock_wait_max_ms);
	/* The EDM observer (issue #68), only when non-zero: a standing "edm: 0"
	   would train the eye to skip the one thing worth noticing.  After a
	   hang this line is gone -- `dmesg` is the readout that survives. */
	if (st.edm.events != 0u)
		camera_out(&o, "edm      : %lu event(s), first %08lx last %08lx "
		           "(see dmesg)\r\n",
		           (unsigned long)st.edm.events,
		           (unsigned long)st.edm.first_status,
		           (unsigned long)st.edm.last_status);
	/* The double-buffer evidence (issue #59): equal-ish per-buffer counts
	   prove the alternation is real; the premature-disable count proves the
	   mask around the arm's disable works, and acceptance requires it 0. */
	if (st.premature_disables != 0u)
		camera_out(&o, "buffers  : %lu / %lu frames (b0/b1), %lu "
		           "premature-disable\r\n",
		           (unsigned long)st.buf_frames[0],
		           (unsigned long)st.buf_frames[1],
		           (unsigned long)st.premature_disables);
	else
		camera_out(&o, "buffers  : %lu / %lu frames (b0/b1)\r\n",
		           (unsigned long)st.buf_frames[0],
		           (unsigned long)st.buf_frames[1]);
	/* The stage profile is `camera profile` now (decision D3), and `camera
	   bench` prints it too; this report is the counters. */
	camera_out(&o, "sink lcd : %lu shown, %lu dropped, %lu busy, %lu dark, "
	           "%lu err\r\n",
	           (unsigned long)sk.delivered, (unsigned long)sk.dropped,
	           (unsigned long)sk.busy, (unsigned long)sk.dark,
	           (unsigned long)sk.errors);
	/* [!] WHERE THE 26 ms WENT (issue #57): the blit is timed on the panel
	   thread; only this row distinguishes "moved" from "lost". */
	if (!sk.prof_ok)
		camera_out(&o, "  blit   : -- (%s)\r\n",
		           sk.prof_why != NULL ? sk.prof_why : "no time source");
	else if (sk.blit_frames == 0u)
		camera_out(&o, "  blit   : -- (no frames since the last "
		           "attach)\r\n");
	else
		camera_out(&o, "  blit   : %6lu us/frame over %lu frame(s) "
		           "[panel thread]\r\n",
		           (unsigned long)(sk.blit_us / sk.blit_frames),
		           (unsigned long)sk.blit_frames);
	/* [!] AND HOW LONG THE SLOT IS HELD (issue #71), which is NOT the row
	   above: this stops when the pin goes back at the staging seam. */
	if (sk.prof_ok && sk.hold_frames != 0u)
		camera_out(&o, "  held   : %6lu us/frame over %lu frame(s) "
		           "[to pin returned]\r\n",
		           (unsigned long)(sk.hold_us / sk.hold_frames),
		           (unsigned long)sk.hold_frames);
	/* [!] WHAT IS INSIDE THE PRODUCER'S `sink` ROW under `nn stream` (issue
	   #60): since #129 only `prep` is in it; invoke and decode run on the
	   worker.  These reset when an `nn stream` arms the overlay, so they
	   describe the LAST nn run. */
	{
		struct nn_camera_stats ns;

		nn_camera_stats(&ns);
		if (ns.prof_frames != 0u && ns.prof_ok) {
			camera_out(&o, "nn sink  : %lu frame(s), last nn stream "
			           "[prep: producer; rest: worker]\r\n",
			           (unsigned long)ns.prof_frames);
			camera_out(&o, "  prep   : %6lu us/frame   setup + "
			           "crop/resize\r\n",
			           (unsigned long)(ns.prep_us / ns.prof_frames));
			camera_out(&o, "  invoke : %6lu us/frame   NPU inference\r\n",
			           (unsigned long)(ns.invoke_us / ns.prof_frames));
			camera_out(&o, "  decode : %6lu us/frame   outputs -> "
			           "the decoder's result\r\n",
			           (unsigned long)(ns.decode_us / ns.prof_frames));
		} else if (ns.prof_frames != 0u) {
			camera_out(&o, "nn sink  : -- (the EPK time source is not "
			           "trusted)\r\n");
		}
	}
	/* A stream in flight has one frame out, so `accepted` leading `puts`
	   by one is normal; any other gap is worth seeing at once. */
	if (sk.accepted != sk.puts)
		camera_out(&o, "  handed : %lu accepted, %lu released%s\r\n",
		           (unsigned long)sk.accepted, (unsigned long)sk.puts,
		           (sk.accepted - sk.puts == 1u) ? " (one in flight)"
		                                         : " [!]");
	if (sk.fault != NULL)
		camera_out(&o, "sink err : %s\r\n", sk.fault);
	/* Only after an `nn stream`: a frame whose inference was refused is
	   still SHOWN, just unannotated. */
	if (sk.overlay_errors != 0u)
		camera_out(&o, "overlay  : %lu frame(s) shown unannotated\r\n",
		           (unsigned long)sk.overlay_errors);
	if (st.fault != NULL)
		camera_out(&o, "fault    : %s\r\n", st.fault);
}

/* ---- info (issue #143 decision 5) --------------------------------------- */

static const char *cam_state_word(uint8_t st)
{
	switch (st) {
	case CAM_ST_DOWN:      return "down (nothing brought up)";
	case CAM_ST_READY:     return "ready";
	case CAM_ST_STREAMING: return "streaming";
	case CAM_ST_FAULTED:   return "faulted (the next use rebuilds it)";
	case CAM_ST_LOST:      return "lost (a stop was never acknowledged; reboot)";
	default:               return "?";
	}
}

/*
 * What the port knows, from one sample, and the values the driver last WROTE
 * -- the software side.  [!] NO I2C AND NO MUTEX: this is the command that
 * still answers while a stream runs or a command holds the camera, so it reads
 * nothing off the sensor (`camera probe` and `camera vts` are the ones that
 * look).  The sensor line says which part this build drives, not that it
 * answered.
 */
void camera_svc_info(struct camera_op_result *res,
                     camera_svc_write_fn write, void *ctx)
{
	struct camera_out o = { write, ctx, 0 };
	struct camera_info_sample is;
	struct cam_wb wb;
	uint16_t lines, dgain;
	uint8_t again;
	int sh_valid;

	camera_info_sample(&is);
	camera_get_wb(&wb);
	cam_sensor_get_exposure_gains(&lines, &again, &dgain);
	sh_valid = cam_sensor_exposure_gains_valid();
	camera_res_ok(res);

	camera_out(&o, "state    : %s\r\n", cam_state_word(is.state));
	if (is.state == CAM_ST_DOWN)
		camera_out(&o, "chip     : -- (no bring-up yet; `camera probe`)\r\n");
	else
		camera_out(&o, "chip     : %08lx%s\r\n",
		           (unsigned long)cam_dp_chip_version(),
		           is.rev_c ? " (rev C: per-frame MIPI bounce)" : "");
	camera_out(&o, "sensor   : %s (id 0x%04X expected; `camera probe` reads it)\r\n",
	           cam_sensor_name(), cam_sensor_id());
	camera_out(&o, "frames   : %lu since the last bring-up\r\n",
	           (unsigned long)is.frames_since_bringup);
	if (is.capture_valid)
		camera_out(&o, "capture  : %ux%u planar B/G/R, %lu bytes "
		           "(capture %lu) -- `camera send`\r\n",
		           (unsigned)CAM_FRAME_WIDTH, (unsigned)CAM_FRAME_HEIGHT,
		           (unsigned long)CAM_RAW_BYTES,
		           (unsigned long)is.capture_gen);
	else
		camera_out(&o, "capture  : none (or replaced by a stream or "
		           "`camera raw`)\r\n");
	if (sh_valid)
		camera_out(&o, "exposure : %u lines   again %u   dgain %u\r\n",
		           (unsigned)lines, (unsigned)again, (unsigned)dgain);
	else
		camera_out(&o, "exposure : -- (nothing has read the sensor back yet)\r\n");
	camera_out(&o, "vts      : %u lines (programmed)\r\n",
	           (unsigned)cam_sensor_frame_length());
	camera_out(&o, "auto     : %s\r\n", camera_auto() ? "on" : "off");
	camera_out(&o, "wb       : r %u  g %u  b %u   black %u   sat %u   gamma %s\r\n",
	           (unsigned)wb.r, (unsigned)wb.g, (unsigned)wb.b,
	           (unsigned)wb.black, (unsigned)wb.sat,
	           wb.gamma ? "sRGB" : "off");
	camera_out(&o, "bayer    : %s\r\n", cam_dp_bayer_name(cam_dp_bayer()));
}

/* ---- the last capture out (`camera send`, issue #143 decision 5 / D4) ---- */

static void cam_res_frame(struct camera_op_result *res, int rc)
{
	if (rc == CAM_ERR_NO_FRAME)
		camera_res_fail(res, CAMERA_SVC_ERR_NO_FRAME,
		                "no frame captured yet (a stream or `camera raw` "
		                "replaces it)");
	else
		camera_res_fail(res, cam_status(rc), "%s", camera_strerror(rc));
}

void camera_svc_frame_open(struct camera_svc_frame *f,
                           struct camera_op_result *res)
{
	uint32_t gen = 0u;
	int rc;

	memset(f, 0, sizeof *f);
	rc = camera_capture_last(&gen);
	if (rc != CAM_OK) {
		cam_res_frame(res, rc);
		return;
	}
	f->gen = gen;
	f->len = CAM_RAW_BYTES;
	f->width = (uint16_t)CAM_FRAME_WIDTH;
	f->height = (uint16_t)CAM_FRAME_HEIGHT;
	f->format = "planar B/G/R";
	f->convert = "boards/grove-vision-ai-v2/scripts/planar_bgr_to_png.py";
	camera_res_ok(res);
}

/*
 * One hold of the API mutex per read (TX_NO_WAIT, BUSY when another command
 * has it), never across the transfer: the check that it is still capture
 * @p f->gen and the copy happen in that one hold (camera_capture_read()).
 */
void camera_svc_frame_read(const struct camera_svc_frame *f, uint32_t off,
                           void *dst, uint32_t n, struct camera_op_result *res)
{
	int rc = camera_capture_read(f->gen, off, dst, n);

	if (rc == CAM_ERR_NO_FRAME)
		camera_res_fail(res, CAMERA_SVC_ERR_NO_FRAME,
		                "frame changed by a concurrent capture");
	else if (rc != CAM_OK)
		camera_res_fail(res, cam_status(rc), "%s", camera_strerror(rc));
	else
		camera_res_ok(res);
}

void camera_svc_frame_close(struct camera_svc_frame *f)
{
	(void)f;   /* nothing is held from open to close */
}

_Static_assert(!CAMERA_SVC_HAS_POWER && CAMERA_SVC_HAS_INFO &&
               !CAMERA_SVC_HAS_TEST && !CAMERA_SVC_HAS_SAVE &&
               CAMERA_SVC_HAS_SEND,
               "camera_svc_grove.c implements probe / info / capture / send / "
               "stream");
