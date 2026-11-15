/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    camera_svc_wio.c
 * @brief   This board behind the shared `camera` command's contract
 *          (issue #143).
 *
 * svc/camera/camera_svc.h is what shell/cmds/cmd_camera.c speaks; port/camera is what
 * this board has.  A thin translation: the last capture, its generation, the
 * stream and its holders each already have one owner in camera.c.  It keeps
 * only two things of its own -- whether a frame cursor holds the OCTOSPI1
 * guard, and the last capture's statistics, taken under the capture's guard
 * (cam_report_rec).
 *
 * [!] NO SHELL INSTANCE.  The sentences cmds/cmd_camera.c used to print are
 * written through the shared command's writer, one whole line per write
 * (svc/camera/camera_svc_adapter.h), or formatted into the caller's result.
 *
 * [!] THE OCTOSPI1 SHARED GUARD, IN THREE PLACES, EACH INSIDE ONE CALL.  The
 * snapshot lives in the PSRAM, and `psram` / `membench` / `devmem` / `wifi
 * flash` can RETUNE that bus: a memory-mapped access to a half-configured
 * OCTOSPI1 stalls the AXI (owhinata/wio-lite-ai#3).  psram_acquire_shared()
 * refuses only such a reconfigurer -- not the LTDC scanning out of the same
 * memory, which is what lets the preview run.  It is held:
 *   - across a capture (the DMA writes the PSRAM) AND the statistics walk
 *     over it, in one hold inside camera_svc_capture() -- the walk is inside
 *     the capture's guard (see cam_report_rec);
 *   - from camera_svc_frame_open() to camera_svc_frame_close() (save / send
 *     read the PSRAM between them);
 *   - across the arm of a stream start (camera_svc_stream_start()); a stream
 *     is open-ended, so from then on camera_streaming() is what psram.c
 *     consults.
 * Lines written while the guard is held keep only a bus reconfigurer waiting,
 * never the producer -- as the old command did.
 *
 * `camera off` stops the operator's stream and then powers down (issue #143,
 * CAMERA_SVC_OFF_STOPS_STREAM 1), and save / send read the LAST capture --
 * the band stream fills no frame.
 *
 * Clean-room design; no third-party code reused.
 */
#include "camera_svc.h"
#include "camera_svc_adapter.h"
#include "camera_svc_config.h"

#include <stdint.h>
#include <string.h>

#include "camera.h"
#include "cam_rgb565.h"   /* the RGB565 statistics, shared with f746g-disco */
#include "mem_sections.h" /* DTCM_BSS: CPU-only data out of AXI-SRAM (issue #46) */
#include "tx_api.h"      /* TX_DISABLE, tx_thread_identify(): the report record */
#if BSP_ENABLE_PSRAM
#include "psram.h"        /* psram_acquire_shared(): the OCTOSPI1 guard */
#endif
#if BSP_ENABLE_LCD
#include "cam_preview.h"  /* the preview's counters (info, stream start) */
#include "ltdc_display.h" /* ltdc_display_on(): whether the preview shows */
#endif

/* ---- results ------------------------------------------------------------- */

/* A CAM_ERR_* code as a contract status.  Only IN_USE and BUSY change what the
   shared command does; every code keeps the board's own sentence. */
static int cam_status(int rc)
{
	switch (rc) {
	case CAM_OK:            return CAMERA_SVC_OK;
	case CAM_ERR_PARAM:     return CAMERA_SVC_ERR_ARG;
	case CAM_ERR_STATE:     return CAMERA_SVC_ERR_STATE;
	case CAM_ERR_TEST:      return CAMERA_SVC_ERR_STATE;
	case CAM_ERR_NO_FRAME:  return CAMERA_SVC_ERR_NO_FRAME;
	case CAM_ERR_BUSY:      return CAMERA_SVC_ERR_BUSY;
	case CAM_ERR_STILL:     return CAMERA_SVC_ERR_BUSY;   /* "-- retry" */
	case CAM_ERR_IN_USE:    return CAMERA_SVC_ERR_IN_USE;
	/* CAM_ERR_TIMEOUT is a capture that never completed (camera.c stops the
	   DCMI and drains it): nothing finishes later, so HW and not the
	   contract's "may still finish" TIMEOUT. */
	default:                return CAMERA_SVC_ERR_HW;     /* HAL / NO_SENSOR /
	                                                         TIMEOUT */
	}
}

static void cam_res_rc(struct camera_op_result *res, int rc)
{
	if (rc == CAM_OK)
		camera_res_ok(res);
	else
		camera_res_fail(res, cam_status(rc), "%s", cam_strerror(rc));
}

/* ---- the OCTOSPI1 shared guard ------------------------------------------- */

/* 1 when held; 0 with @p res filled (BUSY) when a reconfigurer has the bus. */
static int cam_psram_take(struct camera_op_result *res)
{
#if BSP_ENABLE_PSRAM
	if (!psram_acquire_shared()) {
		camera_res_fail(res, CAMERA_SVC_ERR_BUSY,
		                "OCTOSPI1 busy (wait for "
		                "psram/membench/devmem/wifi flash)");
		return 0;
	}
#else
	(void)res;
#endif
	return 1;
}

static void cam_psram_give(void)
{
#if BSP_ENABLE_PSRAM
	psram_release_shared();
#endif
}

/* ---- power and identity --------------------------------------------------- */

void camera_svc_probe(struct camera_op_result *res,
                      camera_svc_write_fn write, void *ctx)
{
	struct camera_out o = { write, ctx, 0 };
	struct camera_info ci;
	int rc;

	camera_out(&o, "camera: powering up and probing (takes ~50 ms) ...\r\n");
	rc = camera_probe();
	if (camera_get_info(&ci) != CAM_OK) {
		cam_res_rc(res, CAM_ERR_STATE);
		return;
	}
	if (rc != CAM_OK) {
		if (ci.i2c_addr != 0u)
			camera_res_fail(res, CAMERA_SVC_ERR_HW,
			                "a device ACKs at SCCB 0x%02x but its ID "
			                "matched no known sensor -- try `camera reg "
			                "0x%02x <reg>`",
			                (unsigned)ci.i2c_addr, (unsigned)ci.i2c_addr);
		else
			camera_res_fail(res, CAMERA_SVC_ERR_HW,
			                "nothing answered on SCCB (XCLK %lu.%02lu MHz) -- "
			                "module connected?",
			                (unsigned long)(ci.xclk_hz / 1000000u),
			                (unsigned long)(ci.xclk_hz % 1000000u / 10000u));
		return;
	}
	camera_res_ok(res);
	if (ci.manuf_id != 0u)
		camera_out(&o, "camera: %s, chip ID 0x%04x, manufacturer 0x%04x at "
		           "SCCB 0x%02x (%u-bit registers)\r\n",
		           camera_sensor_name(ci.sensor), (unsigned)ci.chip_id,
		           (unsigned)ci.manuf_id, (unsigned)ci.i2c_addr,
		           (unsigned)ci.reg_width);
	else
		camera_out(&o, "camera: %s, chip ID 0x%04x at SCCB 0x%02x "
		           "(%u-bit registers)\r\n",
		           camera_sensor_name(ci.sensor), (unsigned)ci.chip_id,
		           (unsigned)ci.i2c_addr, (unsigned)ci.reg_width);
}

void camera_svc_on(struct camera_op_result *res,
                   camera_svc_write_fn write, void *ctx)
{
	struct camera_out o = { write, ctx, 0 };
	int rc = camera_on();

	cam_res_rc(res, rc);
	if (rc == CAM_OK)
		camera_out(&o, "camera: XCLK on, PWDN released, RESETB released\r\n");
}

/* The deepest state this board reaches: the 2V8 rail comes off a fixed LDO
   with no enable pin (schematic U8).  The shared command has stopped the
   operator's stream first (CAMERA_SVC_OFF_STOPS_STREAM 1); camera_off() looks
   again under the camera lock and refuses a stream still running (BUSY) or a
   holder that needs it (IN_USE). */
void camera_svc_off(struct camera_op_result *res,
                    camera_svc_write_fn write, void *ctx)
{
	struct camera_out o = { write, ctx, 0 };
	int rc = camera_off();

	if (rc == CAM_ERR_IN_USE) {
		/* camera_off() looked at the holders under the camera lock; the
		   names are a second look, for the message. */
		const char *users[CAMERA_SVC_USERS_MAX];

		camera_res_in_use(res, users,
		                  camera_stream_users(users, (int)CAMERA_SVC_USERS_MAX));
		return;
	}
	cam_res_rc(res, rc);
	if (rc == CAM_OK)
		camera_out(&o, "camera: PWDN asserted, RESETB low, XCLK stopped "
		           "(the 2V8 rail stays on -- no enable pin)\r\n");
}

void camera_svc_info(struct camera_op_result *res,
                     camera_svc_write_fn write, void *ctx)
{
	struct camera_out o = { write, ctx, 0 };
	struct camera_info ci;
	struct camera_mode m;
	int rc = camera_get_info(&ci);

	cam_res_rc(res, rc);
	if (rc != CAM_OK)
		return;

	camera_out(&o, "state    : %s\r\n", ci.powered ? "powered" : "powered down");
	if (ci.sensor == CAM_SENSOR_OV2640 || ci.sensor == CAM_SENSOR_OV5640)
		camera_out(&o, "sensor   : %s (chip ID 0x%04x, SCCB 0x%02x, %u-bit regs)\r\n",
		           camera_sensor_name(ci.sensor), (unsigned)ci.chip_id,
		           (unsigned)ci.i2c_addr, (unsigned)ci.reg_width);
	else if (ci.i2c_addr != 0u)
		camera_out(&o, "sensor   : %s (unidentified device at SCCB 0x%02x)\r\n",
		           camera_sensor_name(ci.sensor), (unsigned)ci.i2c_addr);
	else
		camera_out(&o, "sensor   : %s\r\n", camera_sensor_name(ci.sensor));
	if (ci.xclk_hz != 0u)
		camera_out(&o, "xclk     : %lu.%02lu MHz = %lu MHz / %lu (PA2, TIM5_CH3)\r\n",
		           (unsigned long)(ci.xclk_hz / 1000000u),
		           (unsigned long)(ci.xclk_hz % 1000000u / 10000u),
		           (unsigned long)(ci.xclk_src_hz / 1000000u),
		           (unsigned long)(ci.xclk_src_hz / ci.xclk_hz));
	else
		camera_out(&o, "xclk     : stopped (PA2 driven low)\r\n");
	/* Everything below the kernel clock is a compile-time constant (phase
	   3c-2 folded away the `tune` knobs), so it is spelled out rather than
	   read back.  The kernel clock is not: it is what the SCCB TIMINGR
	   constants were computed against, and 0.0 MHz here means the mux moved
	   off rcc_pclk4. */
	camera_out(&o, "sccb     : I2C4 PF14/PF15, 100 kHz nominal, kernel %lu.%lu MHz, "
	           "split reads, board pull-ups\r\n",
	           (unsigned long)(ci.i2c_ker_hz / 1000000u),
	           (unsigned long)(ci.i2c_ker_hz % 1000000u / 100000u));
	camera_out(&o, "lines    : PWDN PE7 active high, RESETB PH12 active low\r\n");
	camera_out(&o, "dcmi     : 8-bit, PCK rising, VS/HS low, DMA2_Stream1 -> "
	           "AXI-SRAM bands\r\n");
#if BSP_ENABLE_LCD
	{
		/* Up to 163 bytes with every counter at 10 digits: more than
		   camera_out()'s 128, and still one write. */
		char line[176];
		uint32_t shown, dropped, blit_us;

		cam_preview_stats(&shown, &dropped, &blit_us);
		/* Derived, not switched (issue #142): the preview is what the panel
		   shows while the camera streams AND the display is on. */
		camera_out_n(&o, line, sizeof line,
		             "preview  : %s (%u AXI-SRAM bands of %u rows, rotated to "
		             "320x240 landscape), shown %lu dropped %lu, "
		             "last blit %lu us\r\n",
		             camera_band_streaming()
		                     ? (ltdc_display_on() ? "on"
		                                          : "off (display off -- `lcd on`)")
		                     : "off (camera not streaming)",
		             (unsigned)CAMERA_BANDS_PER_FRAME, (unsigned)CAMERA_BAND_ROWS,
		             (unsigned long)shown, (unsigned long)dropped,
		             (unsigned long)blit_us);
	}
#endif
	camera_out(&o, "sensor cfg : %s, DVP byte swap on, %s\r\n",
	           ci.configured ? "QVGA RGB565 loaded" : "not loaded (lazy)",
	           camera_get_orient());
	if (camera_get_mode(&m) == CAM_OK)
		camera_out(&o, "frame    : %ux%u %s (%lu bytes) -- %s\r\n",
		           (unsigned)m.width, (unsigned)m.height, m.format,
		           (unsigned long)m.frame_bytes,
		           ci.frame_valid ? "captured" : "none yet");
}

/* ---- one-shot capture ----------------------------------------------------- */

/* Frame I/O chunk: exactly one 320-pixel RGB565 row.  The statistics no longer
   need it (svc/camera/cam_rgb565 counts the rows itself); it stays the read
   size the walk below has always used. */
#define CAM_IO_CHUNK (CAMERA_FRAME_WIDTH * 2u)

/*
 * Per-channel min/max/mean over the whole frame -- the instrument phase 2
 * exists to read.  A frame that never arrived is all zeros; a saturated one
 * pins every channel at max; a DCMI latching on the wrong edge or losing sync
 * gives min 0 / max full on all three with a mid-grey mean.  With them, the
 * largest adjacent-row step (the seam): see the header of
 * cmds/cmd_camera_board.c for how to read dR / dG / dB
 * (owhinata/wio-lite-ai#44).  Both are svc/camera/cam_rgb565's (issue #237).
 */

/*
 * [!] THE STATISTICS ARE TAKEN INSIDE THE CAPTURE'S OWN HOLD OF THE OCTOSPI1
 * GUARD, and remembered here with the frame generation they describe.  Taking
 * the guard a second time for the report would leave a gap in which an
 * exclusive holder (`devmem poke`, `psram test`) can rewrite the PSRAM frame
 * by raw stores -- which do not bump the generation -- and the report would
 * describe pixels the sensor never sent.  The old command held one guard
 * across capture, walk and print; this is the same, minus the print, which
 * camera_svc_capture_report() does from this record without touching the
 * PSRAM pixels.
 *
 * Board code, so it may keep state (the shared command may not).  Two
 * consoles may capture at once, so the record is written and read whole,
 * inside a ThreadX critical section, and only a NEWER generation replaces it.
 *
 * [!] THE RECORD NAMES THE THREAD THAT CAPTURED IT, and a report prints it
 * only for that thread AND that frame generation.  The generation alone is not
 * enough: A captures, B captures and stores, and A's report would find the
 * frame and the record agreeing -- on B's capture (B's live statistics under
 * A's `capture test`).  The old command kept its statistics in its own frame,
 * so a command only ever printed its own.  A mismatch is the same failure as a
 * replaced frame.
 *
 * The frame's first bytes are copied in with the statistics, from the same
 * read (issue #237): the report hands them back from here and does not read
 * the PSRAM a second time.
 *
 * DTCM: CPU-only data, kept out of the AXI-SRAM that bus masters need.
 */
struct cam_report_rec {
	uint8_t  valid;
	TX_THREAD *owner;               /* the thread whose capture this is */
	uint32_t gen;
	struct camera_mode m;
	struct cam_rgb565_stats st;     /* the statistics and the seam */
	uint8_t  head[CAMERA_SVC_HEAD_MAX];   /* the frame's first bytes */
};

static struct cam_report_rec cam_last_report DTCM_BSS;

static void cam_report_store(const struct cam_report_rec *r)
{
	TX_INTERRUPT_SAVE_AREA

	TX_DISABLE
	if (!cam_last_report.valid ||
	    (int32_t)(r->gen - cam_last_report.gen) >= 0)
		cam_last_report = *r;
	TX_RESTORE
}

static void cam_report_load(struct cam_report_rec *r)
{
	TX_INTERRUPT_SAVE_AREA

	TX_DISABLE
	*r = cam_last_report;
	TX_RESTORE
}

/* The walk over the frame just captured; the guard is held by the caller.
   The old in-command walk never polled Ctrl+C (240 rows of on-chip
   arithmetic), and this one does not either. */
static void cam_capture_walk(struct camera_op_result *res,
                             struct cam_report_rec *r)
{
	uint8_t buf[CAM_IO_CHUNK];
	struct cam_rgb565_walk walk;
	uint32_t gen, off;
	int rc;

	memset(r, 0, sizeof *r);
	(void)camera_get_mode(&r->m);
	cam_rgb565_begin(&walk, &r->st, r->m.width);
	for (off = 0; off < r->m.frame_bytes; ) {
		uint32_t n = (r->m.frame_bytes - off < CAM_IO_CHUNK)
		             ? r->m.frame_bytes - off : CAM_IO_CHUNK;

		rc = camera_frame_read(off, buf, n, &gen);
		if (rc != CAM_OK) {
			cam_res_rc(res, rc);
			return;
		}
		if (off == 0u) {
			r->gen = gen;
			memcpy(r->head, buf, n < CAMERA_SVC_HEAD_MAX ? n
			                                             : CAMERA_SVC_HEAD_MAX);
		} else if (gen != r->gen) {
			camera_res_fail(res, CAMERA_SVC_ERR_NO_FRAME,
			                "frame changed mid-read (concurrent capture)");
			return;
		}
		cam_rgb565_add(&walk, buf, n);
		off += n;
	}
	cam_rgb565_end(&walk);
	r->valid = 1u;
	r->owner = tx_thread_identify();
	camera_res_ok(res);
}

void camera_svc_capture(int test, struct camera_svc_capture_report *rep,
                        struct camera_op_result *res,
                        camera_svc_write_fn write,
                        int (*cancelled)(void *ctx), void *ctx)
{
	struct camera_out o = { write, ctx, 0 };
	struct cam_report_rec r;
	int rc;

	(void)cancelled;
	if (!cam_psram_take(res))
		return;   /* refused before it is announced, as before */
	/* Every capture pays for the warm-up frames (~1 s at 15).  The FIRST one
	   after a power cycle also pays for the sensor bring-up: 200 ms COM7 reset
	   + 220 ms register table + 100 ms settle, plus 100 ms more if the
	   colorbar state has to change. */
	camera_out(&o, "camera: capturing %s frame (~1 s of warm-up; +0.6 s on the "
	           "first one after a power cycle) ...\r\n",
	           test ? "colorbar test" : "live");
	rc = camera_capture(test ? 1 : 0);
	if (rc != CAM_OK) {
		cam_psram_give();
		cam_res_rc(res, rc);
		return;
	}
	cam_capture_walk(res, &r);   /* still under the capture's guard */
	cam_psram_give();
	if (res->status == CAMERA_SVC_OK) {
		cam_report_store(&r);
		/* Every capture lets the first CAMERA_WARM_FRAMES go (decision D2),
		   inside camera_capture() and -- as before -- not cancellable. */
		rep->warmup = CAMERA_WARM_FRAMES;
		rep->warmup_unit = (uint8_t)CAMERA_SVC_WARMUP_FRAMES;
	}
}

/*
 * The report answers from the record camera_svc_capture() took, after checking
 * -- with a one-byte read under the camera lock (and the guard, the frame being
 * in the PSRAM) -- that the frame is still the one it describes.  The record
 * is never handed back for a frame another capture replaced.  The guard is
 * given back before the numbers are filled in, and the shared command prints
 * them after this returns.
 */
void camera_svc_capture_report(struct camera_svc_capture_report *rep,
                               struct camera_op_result *res,
                               int (*cancelled)(void *ctx), void *ctx)
{
	static const char *const chan[3] = { "R", "G", "B" };
	static const uint8_t bits[3] = { 5u, 6u, 5u };
	struct cam_report_rec r;
	uint8_t probe;
	uint32_t gen;
	int rc;

	(void)cancelled;
	(void)ctx;
	rep->head_len = 0u;   /* the warm-up is the capture's (left alone) */

	if (!cam_psram_take(res))
		return;
	rc = camera_frame_read(0u, &probe, 1u, &gen);
	cam_psram_give();
	if (rc != CAM_OK) {
		cam_res_rc(res, rc);
		return;
	}
	cam_report_load(&r);
	if (!r.valid || r.gen != gen || r.owner != tx_thread_identify()) {
		camera_res_fail(res, CAMERA_SVC_ERR_NO_FRAME,
		                "frame changed by a concurrent capture");
		return;
	}

	rep->format = r.m.format;   /* "rgb565", camera_get_mode()'s (issue #237) */
	rep->width = r.m.width;
	rep->height = r.m.height;
	rep->bytes = r.m.frame_bytes;
	for (unsigned k = 0; k < 3u; k++) {
		rep->chan[k].name = chan[k];
		rep->chan[k].bits = bits[k];
		rep->chan[k].min = (uint16_t)r.st.min[k];
		rep->chan[k].max = (uint16_t)r.st.max[k];
		rep->chan[k].mean_x100 = camera_mean_x100(r.st.sum[k], r.st.npx);
		/* The banding metric: the largest jump between two adjacent rows'
		   channel means (x100).  A settled frame is single digits; a gain
		   change landing mid-readout shows up as hundreds, at the row where
		   it landed. */
		rep->seam[k].name = chan[k];
		rep->seam[k].d_x100 = r.st.seam_d[k];
	}
	rep->nchan = 3u;
	rep->seam_row = r.st.seam_row;
	rep->nseam = 3u;
	memcpy(rep->head, r.head, CAMERA_SVC_HEAD_MAX);
	rep->head_len = CAMERA_SVC_HEAD_MAX;
	camera_res_ok(res);
}

/* This board has no lines of its own: the shared ones say it all. */
int camera_svc_capture_board_lines(const struct camera_svc_capture_report *rep,
                                   camera_svc_write_fn write, void *ctx)
{
	(void)rep;
	(void)write;
	(void)ctx;
	return 0;
}

/* ---- reading the last capture out (save / send) -------------------------- */

/* camera_svc_frame::adapter: this cursor holds the OCTOSPI1 shared guard. */
#define CAM_FRAME_HOLDS_PSRAM 1u

void camera_svc_frame_open(struct camera_svc_frame *f,
                           struct camera_op_result *res)
{
	struct camera_mode m;
	struct camera_info ci;
	uint8_t probe;
	uint32_t gen;
	int rc;

	memset(f, 0, sizeof *f);
	(void)camera_get_mode(&m);
	if (camera_get_info(&ci) != CAM_OK || !ci.frame_valid) {
		cam_res_rc(res, CAM_ERR_NO_FRAME);
		return;
	}
	if (!cam_psram_take(res))
		return;
	/* The generation under the guard, from the read itself. */
	rc = camera_frame_read(0u, &probe, 1u, &gen);
	if (rc != CAM_OK) {
		cam_psram_give();
		cam_res_rc(res, rc);
		return;
	}
	f->gen = gen;
	f->len = m.frame_bytes;
	f->width = m.width;
	f->height = m.height;
	f->format = m.format;
	f->convert = "scripts/rgb565_to_png.py";
	f->adapter = CAM_FRAME_HOLDS_PSRAM;
	camera_res_ok(res);
}

/*
 * camera_frame_read() copies under the camera lock and returns the generation
 * OF THE BYTES IT COPIED; a capture bumps it under the same lock, so the check
 * and the copy are one step.  A changed frame fails the read; the bytes already
 * in @p dst are not to be used.
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
	if (rc != CAM_OK) {
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
	if (f->adapter == CAM_FRAME_HOLDS_PSRAM) {
		f->adapter = 0u;
		cam_psram_give();
	}
}

/* ---- the stream ----------------------------------------------------------- */

/*
 * Since issue #142 a start is the operator TAKING HOLD of the band stream
 * (camera.c): it starts it if nobody held it, and it runs until the last holder
 * lets go.  The OCTOSPI1 guard is held only across the arm -- long enough to be
 * refused while a command reconfigures the bus.
 */
void camera_svc_stream_start(int test, struct camera_op_result *res,
                             int *already,
                             camera_svc_write_fn write, void *ctx)
{
	struct camera_out o = { write, ctx, 0 };
	int rc;

	*already = 0;
	if (!cam_psram_take(res))
		return;
	rc = camera_stream_start(test ? 1 : 0, already);
	cam_psram_give();
	cam_res_rc(res, rc);
	if (rc != CAM_OK)
		return;
#if BSP_ENABLE_LCD
	/* The preview's shown / dropped describe this stream, not every stream
	   since boot (issue #142 review).  Only when this start lit the camera. */
	if (!*already)
		cam_preview_stats_reset();
#endif
	camera_out(&o, "camera: streaming %s%s -- `camera stream stats` / `stop`\r\n",
	           test ? "colorbar" : "live", *already ? " (already running)" : "");
}

/* camera.c's stop waits (<= 1 s) for the stream that was running when it was
   asked to end, so a success is a confirmed stop. */
int camera_svc_stream_stop(struct camera_op_result *res, uint32_t *gen)
{
	const char *users[CAMERA_SVC_USERS_MAX];
	int n = 0;
	int rc = camera_stream_stop_gen(users, (int)CAMERA_SVC_USERS_MAX, &n, gen);

	if (rc == CAM_ERR_IN_USE) {
		camera_res_in_use(res, users, n);   /* D8: nothing changed */
		return CAMERA_SVC_STOP_NONE;
	}
	cam_res_rc(res, rc);
	/* Here CAM_ERR_TIMEOUT is camera_stream_wait_end() running out with the
	   stop request left in place: it may still finish later -- the
	   contract's TIMEOUT, not the HW that a capture's timeout is. */
	if (rc == CAM_ERR_TIMEOUT)
		res->status = CAMERA_SVC_ERR_TIMEOUT;
	return (rc == CAM_OK) ? CAMERA_SVC_STOP_DONE : CAMERA_SVC_STOP_NONE;
}

/* The stop above already waited for its stream; answering by generation all
   the same keeps the contract's one shape (svc/camera/cam_gen.h). */
int camera_svc_stream_ended(uint32_t gen)
{
	return camera_stream_ended(gen);
}

/* The contract's view of one camera_stream_stats() sample, plus the holders --
   a separate snapshot under the camera lock (issue #142), as before. */
static void cam_state_of(struct camera_svc_state *st,
                         const struct camera_stream_stats *s)
{
	int lost = 0;
	int n;

	memset(st, 0, sizeof *st);
	if (s->mode == CAM_STREAM_NONE) {
		st->never_started = 1u;
		return;
	}
	st->streaming = s->active ? 1u : 0u;
	n = camera_stream_holders(st->users, (int)CAMERA_SVC_USERS_MAX, &lost);
	st->nusers = (n > 0) ? (unsigned)n : 0u;
	st->lost = (lost > 0) ? (uint32_t)lost : 0u;
}

void camera_svc_stream_state(struct camera_svc_state *st,
                             struct camera_op_result *res)
{
	struct camera_stream_stats s;
	int rc = camera_stream_stats(&s);

	cam_res_rc(res, rc);
	if (rc != CAM_OK) {
		memset(st, 0, sizeof *st);
		st->streaming = camera_streaming() ? 1u : 0u;
		return;
	}
	cam_state_of(st, &s);
}

void camera_svc_stream_report(struct camera_op_result *res,
                              camera_svc_write_fn write,
                              camera_svc_state_fn head, void *ctx)
{
	struct camera_out o = { write, ctx, 0 };
	struct camera_svc_state st;
	struct camera_stream_stats s;
	int rc = camera_stream_stats(&s);   /* the one sample */

	cam_res_rc(res, rc);
	if (rc != CAM_OK)
		return;
	cam_state_of(&st, &s);
	head(ctx, &st);
	if (st.never_started)
		return;

	camera_out(&o, "frames   : %lu\r\n", (unsigned long)s.frames);
	camera_out(&o, "elapsed  : %lu ms\r\n", (unsigned long)s.elapsed_ms);
	if (s.elapsed_ms != 0u) {
		uint32_t f10 = (uint32_t)((uint64_t)s.frames * 10000u / s.elapsed_ms);

		camera_out(&o, "fps      : %lu.%lu\r\n", (unsigned long)(f10 / 10u),
		           (unsigned long)(f10 % 10u));
	}
	camera_out(&o, "ovr dcmi : %lu\r\n", (unsigned long)s.dcmi_ovr);
	/* The figure of merit for the OCTOSPI1 contention work: DMA FIFO errors
	   are tolerated (they do not stop the stream) but their RATE says how
	   close the bus is to not keeping up.  owhinata/wio-lite-ai#35 expects
	   zero: the DCMI writes AXI-SRAM and never touches OCTOSPI1 at all. */
	camera_out(&o, "dma fe   : %lu\r\n", (unsigned long)s.dma_fe);
	if (s.elapsed_ms != 0u) {
		uint32_t e10 = (uint32_t)((uint64_t)s.dma_fe * 10000u / s.elapsed_ms);

		camera_out(&o, "dma fe/s : %lu.%lu\r\n", (unsigned long)(e10 / 10u),
		           (unsigned long)(e10 % 10u));
	}
	camera_out(&o, "band late : %lu\r\n", (unsigned long)s.band_late);
	camera_out(&o, "band torn : %lu\r\n", (unsigned long)s.band_torn);
	camera_out(&o, "band sync : %lu frame ends off a band boundary\r\n",
	           (unsigned long)s.band_desync);
	camera_out(&o, "bands    : 2 x %lu B in AXI-SRAM (%lu rows each)\r\n",
	           (unsigned long)CAMERA_BAND_BYTES,
	           (unsigned long)CAMERA_BAND_ROWS);
}

/* The shared command calls each of these only under its macro. */
_Static_assert(CAMERA_SVC_HAS_POWER && CAMERA_SVC_HAS_INFO &&
               CAMERA_SVC_HAS_TEST && CAMERA_SVC_HAS_SEND,
               "camera_svc_wio.c implements these camera_svc operations");
