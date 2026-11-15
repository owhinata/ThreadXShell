/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    camera_svc_f746.c
 * @brief   This board behind the shared `camera` command's contract
 *          (issue #143).
 *
 * svc/camera/camera_svc.h is what shell/cmds/cmd_camera.c speaks; port/camera is what
 * this board has.  This is the translation, and like the `nn` adapter it is a
 * THIN one: it keeps no state.  The last capture, its generation, the stream
 * and its holders already have one owner each in camera.c, and everything here
 * asks that owner rather than remembering a second answer.
 *
 * [!] NO SHELL INSTANCE.  The sentences this board used to print from
 * cmds/cmd_camera.c are written through the writer the shared command passes
 * (camera_names.h: struct camera_out) or, for a failure, formatted into the
 * caller's result -- whose detail is a copy, so two consoles never see each
 * other's words.
 *
 * [!] NOTHING IS WRITTEN WITH THE CAMERA LOCK HELD.  Every camera_* call below
 * takes and drops the driver mutex itself; the lines are written between
 * calls, from what the last call returned.  A slow console never stalls the
 * producer.
 *
 * The stream stop is asynchronous here: camera_svc_stream_stop() hands back the
 * generation of the stream it asked to end (camera.c counts them, svc/camera/cam_gen.h)
 * and camera_svc_stream_ended() answers for THAT stream, so `camera off` cannot
 * mistake a later start -- or a start that withdrew the stop and kept the
 * stream -- for its end.  `save` / `send` read the LAST CAPTURE, stream or no
 * stream (issue #143).
 *
 * Clean-room design; no third-party code reused.
 */
#include "camera_svc.h"
#include "camera_svc_config.h"

#include <stdint.h>
#include <string.h>

#include "camera.h"
#include "camera_names.h"
#include "cam_rgb565.h"   /* the RGB565 statistics, shared with wio-lite-ai */
#include "camera_svc_adapter.h"
#include "fmt.h"

/* ---- results ------------------------------------------------------------- */

/* A CAM_ERR_* code as a contract status.  Only IN_USE and BUSY change what the
   shared command does; every code keeps the board's own sentence. */
static int cam_status(int rc)
{
	switch (rc) {
	case CAM_OK:            return CAMERA_SVC_OK;
	case CAM_ERR_PARAM:     return CAMERA_SVC_ERR_ARG;
	case CAM_ERR_STATE:     return CAMERA_SVC_ERR_STATE;
	case CAM_ERR_FORMAT:    return CAMERA_SVC_ERR_STATE;
	case CAM_ERR_TEST:      return CAMERA_SVC_ERR_STATE;
	case CAM_ERR_NO_FRAME:  return CAMERA_SVC_ERR_NO_FRAME;
	case CAM_ERR_BUSY:      return CAMERA_SVC_ERR_BUSY;
	case CAM_ERR_IN_USE:    return CAMERA_SVC_ERR_IN_USE;
	/* CAM_ERR_TIMEOUT is a capture whose completion semaphore ran out, after
	   which camera.c stops the DCMI and re-initialises its DMA
	   (snapshot_dma_reinit()): nothing finishes later, so it is HW here and
	   not the contract's "may still finish" TIMEOUT. */
	default:                return CAMERA_SVC_ERR_HW;   /* HAL / NO_SENSOR /
	                                                       TIMEOUT: the sensor
	                                                       or DCMI refused */
	}
}

static void cam_res_rc(struct camera_op_result *res, int rc)
{
	if (rc == 0)
		camera_res_ok(res);
	else
		camera_res_fail(res, cam_status(rc), "%s", camera_strerror(rc));
}

/* ---- power and identity --------------------------------------------------- */

void camera_svc_probe(struct camera_op_result *res,
                      camera_svc_write_fn write, void *ctx)
{
	struct camera_out o = { write, ctx, 0 };
	uint32_t id = 0;
	int rc;

	/* The probe power-cycles and soft-resets the sensor; the ID read alone
	   sits ~600 ms behind those settle times. */
	camera_out(&o, "camera: probing OV5640 (takes ~1s) ...\r\n");
	rc = camera_probe(&id);
	cam_res_rc(res, rc);
	if (rc == 0)
		camera_out(&o, "OV5640 detected: chip ID 0x%04lx\r\n",
		           (unsigned long)id);
}

/* `camera on`: the OV5640 cannot be powered without the I2C bring-up (there is
   no power-only state: capture/stream lazily power-cycle whenever the module is
   not yet up), so `on` shares the probe path -- it just powers on without
   reporting the chip ID (that is `camera probe`'s job).  Fails like probe if
   the module is absent. */
void camera_svc_on(struct camera_op_result *res,
                   camera_svc_write_fn write, void *ctx)
{
	struct camera_out o = { write, ctx, 0 };
	int rc;

	camera_out(&o, "camera: powering on (takes ~1s) ...\r\n");
	rc = camera_probe(NULL);
	cam_res_rc(res, rc);
	if (rc == 0)
		camera_out(&o, "camera: on\r\n");
}

void camera_svc_off(struct camera_op_result *res,
                    camera_svc_write_fn write, void *ctx)
{
	struct camera_out o = { write, ctx, 0 };
	int rc = camera_power_off();

	if (rc == CAM_ERR_IN_USE) {
		/* The base is down between an overrun and its recovery, and nn
		   still holds it (issue #142 D8).  camera_power_off() checked under
		   the camera lock; the names are a second look, for the message. */
		const char *users[CAMERA_SVC_USERS_MAX];

		camera_res_in_use(res, users,
		               camera_stream_users(users, (int)CAMERA_SVC_USERS_MAX));
		return;
	}
	cam_res_rc(res, rc);
	if (rc == 0)
		camera_out(&o, "camera: power off\r\n");
}

void camera_svc_info(struct camera_op_result *res,
                     camera_svc_write_fn write, void *ctx)
{
	struct camera_out o = { write, ctx, 0 };
	struct camera_info ci;
	struct camera_settings cs;
	struct camera_mode cm;
	int rc;

	rc = camera_get_info(&ci);
	cam_res_rc(res, rc);
	if (rc != 0)
		return;

	camera_out(&o, "module   : B-CAMS-OMV (OV5640) on P1/DCMI, I2C1 @0x78\r\n");
	camera_out(&o, "power    : %s\r\n", ci.powered ? "on" : "off");
	camera_out(&o, "chip ID  : ");
	if (ci.chip_id != 0)
		camera_out(&o, "0x%04lx\r\n", (unsigned long)ci.chip_id);
	else
		camera_out(&o, "- (not probed)\r\n");
	camera_out(&o, "configured : %s\r\n", ci.configured ? "yes" : "no");
	camera_out(&o, "frame    : %s\r\n", ci.frame_valid ? "valid" : "none");

	if (camera_get_mode(&cm) == 0) {
		camera_out(&o, "mode     : %ux%u %s\r\n", (unsigned)cm.width,
		           (unsigned)cm.height,
		           camera_named_name(camera_format_names, cm.format));
		camera_out(&o, "fps target : %lu.%lu%s\r\n",
		           (unsigned long)(cm.fps_target_x10 / 10u),
		           (unsigned long)(cm.fps_target_x10 % 10u),
		           cm.streamable ? "" : "  (capture only -- too large to stream)");
		camera_out(&o, "fps select : %u", (unsigned)cm.fps_sel);
		if (cm.fps_sel == 30u && cm.fps_eff == 15u) {
			if (cm.fps_clamp == CAM_FPS_CLAMP_LTDC)
				camera_out(&o, "  (clamped to 15: lcd scanout active "
				           "-- `lcd off` for 30fps)");
			else
				camera_out(&o, "  (24MHz: snapshot-only mode)");
		}
		camera_out(&o, "\r\n");
	}

	/* owhinata/stm32f746g-disco#102: attached subscribers + per-sink pipeline
	   stats.  The base capture is a cascade of the internal stats sink plus
	   the enabled gui/nncam/mjpeg subscribers; each attaches only while the
	   base runs and its format matches. */
	{
		struct camera_sub_stat subs[CAMERA_MAX_SUB_STATS];
		int nsub = camera_subscribers_snapshot(subs, CAMERA_MAX_SUB_STATS);

		if (nsub == 0) {
			camera_out(&o, "subscribers : none (base off)\r\n");
		} else {
			camera_out(&o, "subscribers:\r\n");
			for (int i = 0; i < nsub; i++) {
				camera_out(&o, "  %-6s %-6s %-8s "
				           "delivered=%lu dropped=%lu errors=%lu\r\n",
				           subs[i].name,
				           camera_named_name(camera_format_names,
				                             subs[i].fmt),
				           subs[i].attached ? "attached"
				                            : (subs[i].enabled ? "idle" : "-"),
				           (unsigned long)subs[i].delivered,
				           (unsigned long)subs[i].dropped,
				           (unsigned long)subs[i].errors);
			}
		}
	}

	if (camera_get_settings(&cs) == 0) {
		camera_out(&o, "-- settings (camera set) --\r\n");
		camera_settings_write(&o, &cs);
	}
}

/* ---- one-shot capture ----------------------------------------------------- */

/* The warm-up is TIME here (decision D1): what the capture slept and spent
   before its snapshot -- a cold sensor's bring-up and settle (~1 s), next to
   nothing once it is configured.  A running stream refuses the capture, so
   it is only ever paid from cold or idle.  The sleeps are inside the port
   and not cancellable, as before. */
void camera_svc_capture(int test, struct camera_svc_capture_report *rep,
                        struct camera_op_result *res,
                        camera_svc_write_fn write,
                        int (*cancelled)(void *ctx), void *ctx)
{
	struct camera_out o = { write, ctx, 0 };
	uint32_t warm_ms = 0u;
	int rc;

	(void)cancelled;
	camera_out(&o, "camera: capturing %s frame ...\r\n",
	           test ? "colorbar test" : "live");
	rc = camera_capture_warm(test ? 1 : 0, &warm_ms);
	cam_res_rc(res, rc);
	if (rc == 0) {
		rep->warmup = warm_ms;
		rep->warmup_unit = (uint8_t)CAMERA_SVC_WARMUP_MS;
	}
}

/*
 * Bounded scratch for the per-format pixel statistics: the frame is walked in
 * fixed chunks rather than full rows (a WVGA row is 1600 B).  512 is even, so
 * RGB565/YUV422 two-byte pixels never straddle a chunk boundary.  On the
 * console's stack (CLI_INSTANCE_STACK_SIZE is 4 KB on this board).
 */
#define CAM_IO_CHUNK 512u

/* What this board keeps in camera_svc_capture_report::board for its own
   lines (camera_svc_capture_board_lines): a JPEG's two markers. */
#define CAM_REP_KIND    0u   /* CAM_REP_KIND_* below                    */
#define CAM_REP_SOI_OK  1u   /* the stream starts FF D8                 */
#define CAM_REP_EOI_OK  2u   /* the stream ends FF D9                   */
#define CAM_REP_KIND_RASTER 0u
#define CAM_REP_KIND_JPEG   1u

static void cam_rep_chan(struct camera_svc_chan_stat *c, const char *name,
                         uint8_t bits, uint32_t min, uint32_t max,
                         uint32_t sum, uint32_t n)
{
	c->name = name;
	c->bits = bits;
	c->min = (uint16_t)min;
	c->max = (uint16_t)max;
	c->mean_x100 = camera_mean_x100(sum, n);
}

/* A read of the frame the report started on (@p gen0), or a failure in @p res. */
static int cam_rep_read(uint32_t off, void *dst, uint32_t n, uint32_t gen0,
                        struct camera_op_result *res)
{
	uint32_t gen;
	int rc = camera_frame_read(off, dst, n, &gen);

	if (rc != 0) {
		cam_res_rc(res, rc);
		return -1;
	}
	if (gen != gen0) {
		camera_res_fail(res, CAMERA_SVC_ERR_NO_FRAME,
		                "frame changed mid-read (concurrent capture)");
		return -1;
	}
	return 0;
}

/* JPEG: variable length, no raster pixels -- the stream length and the
   SOI/EOI markers (the driver already verified them on capture), read from
   the frame @p gen0 names: the head first, the tail last. */
static void cam_report_jpeg(uint32_t len, uint32_t cap, uint32_t gen0,
                            struct camera_svc_capture_report *rep,
                            struct camera_op_result *res)
{
	uint8_t tail[2] = { 0, 0 };
	uint32_t head = (len < CAMERA_SVC_HEAD_MAX) ? len : CAMERA_SVC_HEAD_MAX;

	/* A length the frame cannot have is a frame that changed under it. */
	if (len < 2u || len > cap) {
		camera_res_fail(res, CAMERA_SVC_ERR_NO_FRAME,
		                "frame changed mid-read (concurrent capture)");
		return;
	}
	if (cam_rep_read(0u, rep->head, head, gen0, res) != 0 ||
	    cam_rep_read(len - 2u, tail, 2u, gen0, res) != 0)
		return;
	rep->head_len = head;
	rep->board[CAM_REP_KIND] = CAM_REP_KIND_JPEG;
	rep->board[CAM_REP_SOI_OK] = (rep->head[0] == 0xFFu && rep->head[1] == 0xD8u);
	rep->board[CAM_REP_EOI_OK] = (tail[0] == 0xFFu && tail[1] == 0xD9u);
	camera_res_ok(res);
}

/*
 * Per-channel statistics over the captured frame, computed chunk by chunk
 * through camera_frame_read (the driver mutex serializes against a concurrent
 * capture).  Sums fit comfortably: WVGA's 384000 px * max 255 < 2^27.  The
 * RGB565 ones and the seam are svc/camera/cam_rgb565's (issue #237).
 *
 * [!] THE GENERATION FIRST, EVERYTHING ELSE AFTER, the order
 * camera_svc_frame_open() uses: one byte is read for the frame's generation,
 * then the length, the format and the size are taken, and every later read --
 * the statistics, the head, a JPEG's tail -- must come from that generation.
 * A `camera res` / `camera format` (which drops the frame) or another capture
 * (which replaces it) landing anywhere in between makes a later read fail or
 * name another generation; either fails the report.  So the numbers never
 * describe a new frame by an old width.  The report succeeds only after a read
 * of that generation that came after the length and the size.
 */
void camera_svc_capture_report(struct camera_svc_capture_report *rep,
                               struct camera_op_result *res,
                               int (*cancelled)(void *ctx), void *ctx)
{
	struct camera_mode m;
	struct camera_info ci;
	uint8_t buf[CAM_IO_CHUNK];
	struct cam_rgb565_stats st;                /* RGB565: R5 / G6 / B5     */
	struct cam_rgb565_walk walk;
	uint32_t rmin = 255, rmax = 0, rsum = 0;   /* YUV422: U                */
	uint32_t gmin = 255, gmax = 0, gsum = 0;   /* YUV422: V                */
	uint32_t ymin = 255, ymax = 0, ysum = 0;   /* luma for YUV422 / Y8     */
	uint32_t gen0, off, npx;
	uint8_t probe;
	int rc;

	rep->head_len = 0u;   /* the warm-up is the capture's (left alone) */
	rep->nchan = 0u;
	rep->nseam = 0u;
	memset(rep->board, 0, sizeof rep->board);

	rc = camera_frame_read(0u, &probe, 1u, &gen0);
	if (rc != 0) {
		cam_res_rc(res, rc);
		return;
	}
	if (camera_get_mode(&m) != 0 || camera_get_info(&ci) != 0) {
		cam_res_rc(res, CAM_ERR_STATE);
		return;
	}
	if (!ci.frame_valid) {
		cam_res_rc(res, CAM_ERR_NO_FRAME);
		return;
	}
	rep->format = camera_named_name(camera_format_names, m.format);
	rep->width = m.width;
	rep->height = m.height;
	if (m.format == CAM_FMT_JPEG) {
		rep->bytes = ci.frame_bytes;
		cam_report_jpeg(ci.frame_bytes, m.frame_bytes, gen0, rep, res);
		return;
	}
	/* A raster frame is the whole of its mode; anything else is not the
	   frame this mode describes. */
	if (ci.frame_bytes != m.frame_bytes) {
		camera_res_fail(res, CAMERA_SVC_ERR_NO_FRAME,
		                "frame changed mid-read (concurrent capture)");
		return;
	}
	rep->bytes = m.frame_bytes;

	npx = (uint32_t)m.width * m.height;
	cam_rgb565_begin(&walk, &st, m.width);

	/* Walk the frame in fixed chunks, accumulating per-format statistics.  A
	   generation change means a concurrent capture replaced the pixels
	   (frame_valid alone cannot detect that -- only the generation). */
	for (off = 0; off < m.frame_bytes; ) {
		uint32_t n = (m.frame_bytes - off < CAM_IO_CHUNK)
		             ? m.frame_bytes - off : CAM_IO_CHUNK;

		if (cam_rep_read(off, buf, n, gen0, res) != 0)
			return;
		if (off == 0u) {
			/* the first bytes, from the same read as the statistics */
			rep->head_len = (n < CAMERA_SVC_HEAD_MAX) ? n : CAMERA_SVC_HEAD_MAX;
			memcpy(rep->head, buf, rep->head_len);
		}
		if (m.format == CAM_FMT_RGB565) {
			/* 512 is even and the walk counts the rows from the width, so
			   a chunk need not end on a row. */
			cam_rgb565_add(&walk, buf, n);
		} else if (m.format == CAM_FMT_YUV422) {
			/* YUYV groups Y0 U Y1 V; off is a multiple of 4, so a group never
			   straddles a chunk.  Y -> luma; U -> r*, V -> g* accumulators. */
			for (uint32_t i = 0; i + 3u < n; i += 4u) {
				uint32_t y0 = buf[i], uu = buf[i + 1u];
				uint32_t y1 = buf[i + 2u], vv = buf[i + 3u];

				if (y0 < ymin) ymin = y0;
				if (y0 > ymax) ymax = y0;
				if (y1 < ymin) ymin = y1;
				if (y1 > ymax) ymax = y1;
				ysum += y0 + y1;
				if (uu < rmin) rmin = uu;
				if (uu > rmax) rmax = uu;
				rsum += uu;
				if (vv < gmin) gmin = vv;
				if (vv > gmax) gmax = vv;
				gsum += vv;
			}
		} else {   /* Y8: one luma byte per pixel */
			for (uint32_t i = 0; i < n; i++) {
				uint32_t yv = buf[i];

				if (yv < ymin) ymin = yv;
				if (yv > ymax) ymax = yv;
				ysum += yv;
			}
		}
		off += n;
		/* Asked once per chunk, as the old in-command walk did: a Ctrl+C
		   ends the report and nothing is printed. */
		if (cancelled != NULL && cancelled(ctx)) {
			res->status = CAMERA_SVC_ERR_CANCELLED;
			res->detail[0] = '\0';
			res->nusers = 0u;
			return;
		}
	}

	if (m.format == CAM_FMT_RGB565) {
		cam_rgb565_end(&walk);
		cam_rep_chan(&rep->chan[0], "R", 5u, st.min[0], st.max[0], st.sum[0], npx);
		cam_rep_chan(&rep->chan[1], "G", 6u, st.min[1], st.max[1], st.sum[1], npx);
		cam_rep_chan(&rep->chan[2], "B", 5u, st.min[2], st.max[2], st.sum[2], npx);
		rep->nchan = 3u;
		/* The seam, as wio-lite-ai's (svc/camera/cam_rgb565.h). */
		rep->seam_row = st.seam_row;
		rep->seam[0] = (struct camera_svc_seam_step){ "R", st.seam_d[0] };
		rep->seam[1] = (struct camera_svc_seam_step){ "G", st.seam_d[1] };
		rep->seam[2] = (struct camera_svc_seam_step){ "B", st.seam_d[2] };
		rep->nseam = 3u;
	} else if (m.format == CAM_FMT_YUV422) {
		uint32_t nc = npx / 2u;   /* one U and one V per two luma samples */

		cam_rep_chan(&rep->chan[0], "Y", 8u, ymin, ymax, ysum, npx);
		cam_rep_chan(&rep->chan[1], "Cb", 8u, rmin, rmax, rsum, nc);
		cam_rep_chan(&rep->chan[2], "Cr", 8u, gmin, gmax, gsum, nc);
		rep->nchan = 3u;
	} else {   /* Y8 */
		cam_rep_chan(&rep->chan[0], "Y", 8u, ymin, ymax, ysum, npx);
		rep->nchan = 1u;
	}
	camera_res_ok(res);
}

/* This board's own lines: a JPEG's markers, nothing for a raster frame. */
int camera_svc_capture_board_lines(const struct camera_svc_capture_report *rep,
                                   camera_svc_write_fn write, void *ctx)
{
	struct camera_out o = { write, ctx, 0 };

	if (rep->board[CAM_REP_KIND] == CAM_REP_KIND_JPEG) {
		camera_out(&o, "SOI      : %s\r\n",
		           rep->board[CAM_REP_SOI_OK] ? "ok (FFD8)" : "MISSING");
		camera_out(&o, "EOI      : %s\r\n",
		           rep->board[CAM_REP_EOI_OK] ? "ok (FFD9)" : "MISSING");
	}
	return o.failed ? -1 : 0;
}

/* ---- reading the last capture out (save / send) -------------------------- */

/* The PC-side converter for a raw format (owhinata/stm32f746g-disco#45), or
   NULL for a JPEG stream, which a PC opens as it is. */
static const char *cam_convert_of(int fmt)
{
	switch (fmt) {
	case CAM_FMT_YUV422: return "scripts/yuv422_to_png.py";
	case CAM_FMT_Y8:     return "scripts/y8_to_png.py";
	case CAM_FMT_JPEG:   return NULL;
	default:             return "scripts/rgb565_to_png.py";
	}
}

void camera_svc_frame_open(struct camera_svc_frame *f,
                           struct camera_op_result *res)
{
	struct camera_mode m;
	struct camera_info ci;
	uint8_t probe;
	uint32_t gen;

	/* The LAST `camera capture`, whether or not a stream runs (issue #143):
	   the stream never writes the capture buffer, so what is saved or sent is
	   the frame the operator captured and saw the statistics of.  With none,
	   the open fails before anything is created. */
	/* The generation FIRST, the length after: a capture landing between the
	   two gives a cursor whose generation is already stale, so the first read
	   fails rather than reading the new frame against the old length. */
	if (camera_frame_read(0, &probe, 1u, &gen) != 0 ||
	    camera_get_mode(&m) != 0 || camera_get_info(&ci) != 0 ||
	    !ci.frame_valid) {
		cam_res_rc(res, CAM_ERR_NO_FRAME);
		return;
	}
	f->gen = gen;
	f->len = ci.frame_bytes;
	f->width = m.width;
	f->height = m.height;
	f->format = camera_named_name(camera_format_names, m.format);
	f->convert = cam_convert_of(m.format);
	f->adapter = 0u;
	camera_res_ok(res);
}

/*
 * camera_frame_read() copies under the driver mutex and returns the generation
 * OF THE BYTES IT COPIED, so comparing that with the cursor's is the check and
 * the copy in one step: the one path that refreshes the stable buffer (a
 * capture) bumps the generation under the same mutex.  A
 * changed frame fails the read; the bytes already in @p dst are not to be
 * used.
 */
void camera_svc_frame_read(const struct camera_svc_frame *f, uint32_t off,
                           void *dst, uint32_t n, struct camera_op_result *res)
{
	uint32_t gen;
	int rc;

	if (off > f->len || n > f->len - off) {
		cam_res_rc(res, CAM_ERR_PARAM);
		return;
	}
	rc = camera_frame_read(off, dst, n, &gen);
	if (rc != 0) {
		cam_res_rc(res, rc);
		return;
	}
	if (gen != f->gen) {
		camera_res_fail(res, CAMERA_SVC_ERR_NO_FRAME,
		             "frame changed by a concurrent capture");
		return;
	}
	camera_res_ok(res);
}

void camera_svc_frame_close(struct camera_svc_frame *f)
{
	(void)f;   /* nothing is held from open to close on this board */
}

/* ---- the stream (owhinata/stm32f746g-disco#46): non-blocking start / stop -- */

void camera_svc_stream_start(int test, struct camera_op_result *res,
                             int *already,
                             camera_svc_write_fn write, void *ctx)
{
	struct camera_out o = { write, ctx, 0 };
	struct camera_mode m;

	*already = 0;
	/* Clearer message than the generic CAM_ERR_STATE: only large RASTER modes
	   are capture-only (the ring slot / one DMA NDTR cannot hold the frame).
	   JPEG streams via the variable-length snapshot-loop producer
	   (owhinata/stm32f746g-disco#63), so it is NOT rejected here.  Also warn
	   when 30 fps is selected but clamped to 15 because the LTDC is scanning
	   out (the stream still runs, just at 15 fps,
	   owhinata/stm32f746g-disco#67). */
	if (camera_get_mode(&m) == 0) {
		if (!m.streamable && !m.is_jpeg) {
			camera_res_fail(res, CAMERA_SVC_ERR_STATE,
			             "%ux%u %s is capture-only -- too large to "
			             "stream; use a smaller resolution",
			             (unsigned)m.width, (unsigned)m.height,
			             camera_named_name(camera_format_names, m.format));
			return;
		}
		if (m.fps_sel == 30u && m.fps_clamp == CAM_FPS_CLAMP_LTDC)
			camera_out(&o, "camera: note: 30fps clamped to 15 (lcd scanout "
			           "active; `lcd off` for 30fps)\r\n");
	}
	cam_res_rc(res, camera_stream_start(test ? 1 : 0, already));
	if (res->status != CAMERA_SVC_OK)
		return;
	if (*already)
		camera_out(&o, "camera: streaming %s already running\r\n",
		           test ? "colorbar" : "live");
	else
		camera_out(&o, "camera: streaming %s started (`camera stream stats` "
		           "/ `stop`)\r\n", test ? "colorbar" : "live");
}

int camera_svc_stream_stop(struct camera_op_result *res, uint32_t *gen)
{
	const char *users[CAMERA_SVC_USERS_MAX];
	int n = 0;
	int rc = camera_stream_stop_gen(users, (int)CAMERA_SVC_USERS_MAX, &n, gen);

	if (rc == CAM_ERR_IN_USE) {
		camera_res_in_use(res, users, n);
		return CAMERA_SVC_STOP_NONE;
	}
	cam_res_rc(res, rc);
	/* The producer tears the stream down later (camera.c's stop is a request). */
	return (rc == 0) ? CAMERA_SVC_STOP_REQUESTED : CAMERA_SVC_STOP_NONE;
}

int camera_svc_stream_ended(uint32_t gen)
{
	return camera_stream_ended(gen);
}

/* The contract's state view of one camera_stream_stats() sample, plus the
   holders -- a separate snapshot under the camera lock (issue #142), as the
   old command always took them. */
static void cam_state_of(struct camera_svc_state *st,
                         const struct camera_stream_info *si)
{
	int lost = 0;
	int n;

	memset(st, 0, sizeof *st);
	st->streaming = si->active ? 1u : 0u;
	st->stop_reason = (!si->active && si->err) ? "overrun" : NULL;
	n = camera_stream_holders(st->users, (int)CAMERA_SVC_USERS_MAX, &lost);
	st->nusers = (n > 0) ? (unsigned)n : 0u;
	st->lost = (lost > 0) ? (uint32_t)lost : 0u;
}

void camera_svc_stream_state(struct camera_svc_state *st,
                             struct camera_op_result *res)
{
	struct camera_stream_info si;
	int rc = camera_stream_stats(&si);

	cam_res_rc(res, rc);
	if (rc != 0) {
		/* `camera off` only needs this, and camera_streaming() never fails
		   (it is what the old `off` read). */
		memset(st, 0, sizeof *st);
		st->streaming = camera_streaming() ? 1u : 0u;
		return;
	}
	cam_state_of(st, &si);
}

void camera_svc_stream_report(struct camera_op_result *res,
                              camera_svc_write_fn write,
                              camera_svc_state_fn head, void *ctx)
{
	struct camera_out o = { write, ctx, 0 };
	struct camera_stream_info si;
	struct camera_svc_state st;
	int rc = camera_stream_stats(&si);   /* the one sample */

	cam_res_rc(res, rc);
	if (rc != 0)
		return;
	cam_state_of(&st, &si);
	head(ctx, &st);
	camera_out(&o, "elapsed  : %lu ms\r\n", (unsigned long)si.elapsed_ms);
	/* owhinata/stm32f746g-disco#102: split the producer counters (frames the
	   DCMI/DMA acquired + published) from the internal stats sink's per-sink
	   delivery counters. */
	camera_out(&o, "-- producer --\r\n");
	camera_out(&o, "captured : %lu\r\n", (unsigned long)si.captured);
	camera_out(&o, "published : %lu\r\n", (unsigned long)si.frames);
	if (si.elapsed_ms != 0) {
		/* frames-per-second x10 (one decimal), no floating point */
		uint32_t f10 = (uint32_t)((uint64_t)si.frames * 10000u /
		                          si.elapsed_ms);

		camera_out(&o, "fps      : %lu.%lu\r\n",
		           (unsigned long)(f10 / 10u), (unsigned long)(f10 % 10u));
	}
	camera_out(&o, "ovr dcmi : %lu\r\n", (unsigned long)si.dcmi_ovr);
	camera_out(&o, "ovr ring : %lu\r\n", (unsigned long)si.ring_ovr);
	camera_out(&o, "dma fe   : %lu\r\n", (unsigned long)si.dma_fe);
	if (si.elapsed_ms != 0) {
		/* DMA FIFO-error RATE (owhinata/stm32f746g-disco#59): the figure of
		   merit for the SDRAM-contention work -- fe per second x10 (one
		   decimal), no floating point. */
		uint32_t fe10 = (uint32_t)((uint64_t)si.dma_fe * 10000u /
		                           si.elapsed_ms);

		camera_out(&o, "dma fe/s : %lu.%lu\r\n",
		           (unsigned long)(fe10 / 10u), (unsigned long)(fe10 % 10u));
	}
	camera_out(&o, "jpeg trunc : %lu\r\n", (unsigned long)si.jpeg_trunc);
	/* Ring arena: owhinata/stm32f746g-disco#65. */
	if (si.slots != 0)
		camera_out(&o, "ring     : %lu slots x %lu B\r\n",
		           (unsigned long)si.slots, (unsigned long)si.slot_bytes);
	camera_out(&o, "-- stat sink (shell) --\r\n");
	camera_out(&o, "delivered : %lu\r\n", (unsigned long)si.delivered);
	camera_out(&o, "dropped  : %lu\r\n", (unsigned long)si.dropped);
	camera_out(&o, "errors   : %lu\r\n", (unsigned long)si.stat_errors);
}

/* The shared command calls each of these only under its macro; a board that
   says it has one must implement it, and this one does all of them. */
_Static_assert(CAMERA_SVC_HAS_POWER && CAMERA_SVC_HAS_INFO &&
               CAMERA_SVC_HAS_TEST && CAMERA_SVC_HAS_SAVE &&
               CAMERA_SVC_HAS_SEND,
               "camera_svc_f746.c implements every camera_svc operation");
