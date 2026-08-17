/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    cmd_camera.c
 * @brief   `camera` shell command: OV5647 bring-up, capture and live preview
 *          (Grove Vision AI V2, issue #35 / M-G3b).
 *
 * Registered LOCALLY on this board, like `lcd`: the other two boards have their
 * own camera commands over entirely different hardware, and the only thing the
 * three share is the vocabulary.
 *
 * The subcommands answer questions, in the order you would ask them when
 * nothing works yet:
 *
 *   camera probe    -- is a module there at all?  Powers it, reads the sensor's
 *                      model ID over I2C, and prints the chip revision (which
 *                      decides whether the per-frame MIPI bounce is needed).
 *                      This is the one that separates "nothing is plugged in"
 *                      from every other failure.
 *   camera capture  -- take ONE frame and describe it, per channel, straight
 *                      off the hardware's own planar output.  A dead MIPI lane,
 *                      a sensor that never streamed and a demosaic given the
 *                      wrong Bayer phase all produce a frame of the right size
 *                      at the right time with every layer reporting success;
 *                      the channel balance is what tells them apart.  The row
 *                      seam metric catches the other family -- a gain change
 *                      landing mid-readout, or a stride off by one.
 *   camera preview  -- the milestone: continuous capture onto the panel, Ctrl+C
 *                      to stop.
 *   camera stats    -- the counters, after the fact.
 *
 * WHY `capture` REPORTS THE PLANES AND NOT THE PACKED FRAME.  The planes are
 * what the camera produced; the packed RGB565 is what this firmware made of
 * them.  Reporting the packed version would fold two independent suspects into
 * one number -- and the packer already has a host test
 * (test/test_cam_convert.c), which the camera cannot have.
 */
#include "cli.h"

#include <string.h>   /* strcmp (bayer phase names) */

#include "tx_api.h"

#include "cam_auto.h"
#include "cam_convert.h"
#include "cam_dp.h"
#include "cam_sensor.h"
#include "cam_lcd_sink.h"
#include "camera.h"
#include "lcd_st7789.h"

#define CAM_PREVIEW_DEFAULT 0u   /* 0 = until Ctrl+C */
#define CAM_BENCH_DEFAULT  60u   /* enough to average, ~4 s at 15 fps  */

static void cam_report(struct cli_instance *sh, const char *what, int rc)
{
	struct camera_stats st;

	cli_error(sh, "camera: %s failed: %s\r\n", what, camera_strerror(rc));

	/* The driver latches a specific reason for every failure it can name.
	 * Making the operator know to run `camera stats` to see it is a way of
	 * hiding the one useful sentence behind a second command. */
	camera_stream_stats(&st);
	if (st.fault != NULL)
		cli_error(sh, "        %s\r\n", st.fault);
	if (st.last_dp_status != 0)
		cli_error(sh, "        last datapath status %ld\r\n",
		          (long)st.last_dp_status);
}

/* ---- probe --------------------------------------------------------------- */

static int cmd_camera_probe(struct cli_instance *sh, int argc, char **argv)
{
	struct camera_probe_info info;
	int rc;

	(void)argc; (void)argv;

	rc = camera_probe(&info);
	if (rc != CAM_OK) {
		cam_report(sh, "probe", rc);
		return 1;
	}

	cli_print(sh, "chip     : %08lx%s\r\n",
	          (unsigned long)info.chip_version,
	          info.rev_c ? " (rev C: per-frame MIPI bounce)" : "");
	cli_print(sh, "sensor   : %s (id 0x%04X, on-chip AEC)\r\n",
	          cam_sensor_name(), info.sensor_id);
	cli_print(sh, "frame    : %ux%u planar B/G/R, %lu bytes\r\n",
	          (unsigned)CAM_FRAME_WIDTH, (unsigned)CAM_FRAME_HEIGHT,
	          (unsigned long)CAM_RAW_BYTES);
	return 0;
}

/* ---- capture ------------------------------------------------------------- */

static void cam_print_plane(struct cli_instance *sh, const char *name,
                            const uint8_t *plane)
{
	struct cam_plane_stats st;
	uint32_t mosaic;

	cam_plane_stats(plane, CAM_FRAME_PIXELS, &st);
	mosaic = cam_plane_mosaic_x100(plane, CAM_FRAME_WIDTH, CAM_FRAME_HEIGHT);

	cli_print(sh, "%s : min %3u  max %3u  mean %3lu.%02lu  "
	              "mosaic %3lu.%02lu\r\n", name,
	          st.min, st.max,
	          (unsigned long)(st.mean_x100 / 100u),
	          (unsigned long)(st.mean_x100 % 100u),
	          (unsigned long)(mosaic / 100u),
	          (unsigned long)(mosaic % 100u));
}

static int cmd_camera_capture(struct cli_instance *sh, int argc, char **argv)
{
	const uint8_t *raw;
	uint32_t seam, row;
	int rc;

	(void)argc; (void)argv;

	rc = camera_capture();
	if (rc != CAM_OK) {
		cam_report(sh, "capture", rc);
		return 1;
	}

	raw = camera_raw_frame();
	cli_print(sh, "frame    : %ux%u planar B/G/R\r\n",
	          (unsigned)CAM_FRAME_WIDTH, (unsigned)CAM_FRAME_HEIGHT);

	/* The plane ORDER is the hardware's, not a preference: WDMA3 writes B
	 * first.  Printing them in that order is what makes a swapped pair
	 * visible against a scene somebody can describe. */
	cam_print_plane(sh, "B(plane 0)", raw);
	cam_print_plane(sh, "G(plane 1)", raw + CAM_FRAME_PIXELS);
	cam_print_plane(sh, "R(plane 2)", raw + 2u * CAM_FRAME_PIXELS);

	seam = cam_plane_row_seam_x100(raw + CAM_FRAME_PIXELS,
	                               CAM_FRAME_WIDTH, CAM_FRAME_HEIGHT, &row);
	cli_print(sh, "seam     : row %3lu  dG %lu.%02lu  "
	              "(single digits = settled)\r\n",
	          (unsigned long)row, (unsigned long)(seam / 100u),
	          (unsigned long)(seam % 100u));

	{
		uint32_t colour = cam_frame_colour_x100(raw, CAM_FRAME_PIXELS);

		cli_print(sh, "bayer    : %s   colour separation %lu.%02lu\r\n",
		          cam_dp_bayer_name(cam_dp_bayer()),
		          (unsigned long)(colour / 100u),
		          (unsigned long)(colour % 100u));
	}

	/*
	 * The two figures answer DIFFERENT questions, and conflating them was a
	 * mistake worth not repeating:
	 *
	 *   mosaic  -- did a demosaic run at all?  Single digits yes, tens no.
	 *              It says NOTHING about whether the phase was right: a
	 *              wrongly-phased demosaic is just as phase-flat.
	 *   colour  -- was the phase right?  A wrong phase interpolates red and
	 *              blue from where they are not, dragging both toward green,
	 *              so the frame desaturates.  Point the camera at something
	 *              strongly coloured and try all four `camera bayer` values:
	 *              the right one is the one that maximises this.  On a grey
	 *              scene they all score low and prove nothing.
	 */
	cli_print(sh, "           (mosaic: single digits = a demosaic ran. "
	              "colour: bigger = the phase fits)\r\n");

	cli_hexdump(sh, raw, 32u);
	return 0;
}

/* ---- raw ----------------------------------------------------------------- */

/*
 * Capture the Bayer MOSAIC and name the phase from it.
 *
 * This is the command that settles the phase question with the board instead of
 * with an argument.  The reasoning it automates:  a Bayer tile has two green
 * photosites on a diagonal; green is the most sensitive channel and carries the
 * most light in almost any scene; so the two greens come out highest and nearly
 * equal, and WHICH diagonal they sit on names the family.  Red versus blue --
 * the remaining pair -- cannot be settled from an unknown scene by any amount
 * of arithmetic, so it is reported rather than guessed.
 */
static int cmd_camera_raw(struct cli_instance *sh, int argc, char **argv)
{
	static const char *const pos[4] = { "(0,0)", "(1,0)", "(0,1)", "(1,1)" };
	const uint8_t *raw;
	uint32_t m[4];
	uint32_t i, hi_a, hi_b;
	int rc;

	(void)argc; (void)argv;

	rc = camera_capture_raw();
	if (rc != CAM_OK) {
		cam_report(sh, "raw capture", rc);
		return 1;
	}

	raw = camera_raw_frame();
	cam_bayer_phase_means_x100(raw, CAM_FRAME_WIDTH, CAM_FRAME_HEIGHT, m);

	cli_print(sh, "raw      : %ux%u 8-bit Bayer (no demosaic)\r\n",
	          (unsigned)CAM_FRAME_WIDTH, (unsigned)CAM_FRAME_HEIGHT);
	for (i = 0u; i < 4u; i++)
		cli_print(sh, "  %s mean : %3lu.%02lu\r\n", pos[i],
		          (unsigned long)(m[i] / 100u),
		          (unsigned long)(m[i] % 100u));

	/* The two highest are the greens. */
	hi_a = 0u;
	for (i = 1u; i < 4u; i++)
		if (m[i] > m[hi_a])
			hi_a = i;
	hi_b = (hi_a == 0u) ? 1u : 0u;
	for (i = 0u; i < 4u; i++)
		if (i != hi_a && m[i] > m[hi_b])
			hi_b = i;

	/* Greens on the anti-diagonal (1,0)+(0,1) means the corners are R and B
	 * -- rggb or bggr.  Greens on the main diagonal means grbg or gbrg. */
	if ((hi_a + hi_b) == 3u)
		cli_print(sh, "greens   : %s + %s  -> phase is RGGB or BGGR\r\n",
		          pos[hi_a], pos[hi_b]);
	else if ((hi_a == 0u && hi_b == 3u) || (hi_a == 3u && hi_b == 0u))
		cli_print(sh, "greens   : %s + %s  -> phase is GRBG or GBRG\r\n",
		          pos[hi_a], pos[hi_b]);
	else
		cli_print(sh, "greens   : %s + %s  -> NOT a diagonal pair; this "
		              "is not Bayer data\r\n", pos[hi_a], pos[hi_b]);

	/*
	 * [!] AND SAY HOW MUCH TO TRUST IT.
	 *
	 * The conclusion above rests entirely on the two greens being the two
	 * highest, and that ordering can be one count from flipping: a
	 * red-dominated scene under warm light pushes the red position right up
	 * against the greens.  Measured here, third place came within 0.44 of
	 * second -- close enough that the naming was luck, not evidence.
	 *
	 * The margin is the gap between the LOWER green and the highest
	 * non-green.  A wide one is a real measurement; a narrow one means try
	 * again against a scene with more green in it (foliage, or anything
	 * daylit) before believing the answer.
	 */
	{
		uint32_t lo_green = (m[hi_a] < m[hi_b]) ? m[hi_a] : m[hi_b];
		uint32_t other = 0u;

		for (i = 0u; i < 4u; i++)
			if (i != hi_a && i != hi_b && m[i] > other)
				other = m[i];

		cli_print(sh, "margin   : %lu.%02lu over the highest non-green "
		              "-- %s\r\n",
		          (unsigned long)((lo_green - other) / 100u),
		          (unsigned long)((lo_green - other) % 100u),
		          (lo_green - other) >= 300u ? "trustworthy"
		                                     : "TOO CLOSE TO CALL, "
		                                       "retry on a greener scene");
	}

	cli_print(sh, "           (red vs blue needs a scene of known colour: "
	              "point at something RED)\r\n");

	/* Two full rows, so the 2x2 structure is visible by eye as well. */
	cli_hexdump(sh, raw, 16u);
	cli_hexdump(sh, raw + CAM_FRAME_WIDTH, 16u);
	return 0;
}

/* ---- preview ------------------------------------------------------------- */

static int cmd_camera_preview(struct cli_instance *sh, int argc, char **argv)
{
	uint32_t frames = CAM_PREVIEW_DEFAULT;
	struct camera_stats st;
	ULONG t0, t1, ticks;
	uint32_t before;
	int rc;

	if (argc > 1 && cli_parse_u32(argv[1], &frames) != 0) {
		cli_error(sh, "camera: bad frame count '%s'\r\n", argv[1]);
		return 1;
	}

	/*
	 * No overlay: a plain preview stays a plain preview.  It is an argument
	 * rather than a mode so that `nn preview`'s boxes can never be inherited
	 * by this command (issue #48).
	 *
	 * ONE CALL, because attaching and starting are one operation since issue
	 * #63 -- and therefore one failure to handle: nothing is attached and no
	 * stream is running, whatever went wrong.  This used to be attach-then-
	 * start with a guard here for the case where the start came back BUSY with
	 * the sink already attached, which was a window rather than a case.
	 */
	rc = cam_lcd_sink_attach_and_stream(NULL);
	if (rc == CAM_ERR_BUSY) {
		cli_error(sh, "camera: a preview is already running, or another "
		              "command owns the camera\r\n");
		return 1;
	}
	if (rc != CAM_OK) {
		cam_report(sh, "preview start", rc);
		return 1;
	}

	camera_stream_stats(&st);
	before = st.frames;
	t0 = tx_time_get();

	/*
	 * The shell thread only waits here -- capture, conversion and the blit
	 * all run on the producer.  Polling once per tick is enough to notice
	 * Ctrl+C and cheap enough not to matter next to a 26 ms frame.
	 */
	for (;;) {
		if (cli_cancel_requested(sh))
			break;
		camera_stream_stats(&st);
		if (!st.streaming)
			break;                      /* the producer gave up */
		if (frames != 0u && (st.frames - before) >= frames)
			break;
		if (cli_sleep(sh, 1u) != 0)
			break;
	}

	t1 = tx_time_get();
	ticks = t1 - t0;

	/*
	 * [!] DETACH ONLY ON A CONFIRMED STOP (issue #48).
	 *
	 * This used to detach unconditionally, under a comment asserting that
	 * the stop was synchronous.  The stop's wait is BOUNDED, so a timeout
	 * means the producer is still running -- and unlinking a sink it may be
	 * inside is the one thing the pipeline cannot survive, because publish()
	 * has already pre-pinned this sink and called consume() with the lock
	 * released.  The camera poisons itself on that path and refuses
	 * everything afterwards, so there is nothing to clean up here and
	 * nothing that would be safe to do.
	 *
	 * [!] AND THE DETACH CAN FAIL TOO, since issue #57: it unlinks the sink
	 * and then drains the panel thread, and a drain that does not finish
	 * leaves a thread that may still be blitting.  A confirmed stop is now
	 * only half the answer, so the result is reported rather than dropped.
	 */
	rc = camera_stream_stop();
	if (rc == CAM_OK) {
		int drain_rc = cam_lcd_sink_detach();

		if (drain_rc != CAM_OK) {
			cli_error(sh, "camera: the panel thread did not finish "
			              "(%d); preview is unusable until reboot\r\n",
			          drain_rc);
			return 1;
		}
	}

	if (cli_cancel_requested(sh)) {
		/* Cancelled.  Silent on purpose, as in `lcd loop`: the shared
		 * core discards output produced while cancel_req is still set,
		 * so a summary here would never reach the terminal.  The
		 * dispatcher's "^C" is the feedback. */
		return 0;
	}

	if (rc != CAM_OK) {
		cam_report(sh, "stream stop", rc);
		return 1;
	}

	camera_stream_stats(&st);
	if (st.fault != NULL) {
		cli_error(sh, "camera: stopped: %s\r\n", st.fault);
		return 1;
	}

	{
		uint32_t got = st.frames - before;
		uint32_t ms = (uint32_t)((ticks * 1000u) /
		                         TX_TIMER_TICKS_PER_SECOND);

		cli_print(sh, "%lu frame(s) in %lu ms", (unsigned long)got,
		          (unsigned long)ms);
		if (ms != 0u)
			cli_print(sh, " = %lu.%lu fps",
			          (unsigned long)(got * 1000u / ms),
			          (unsigned long)((got * 10000u / ms) % 10u));
		cli_print(sh, "\r\n");
	}
	return 0;
}

/*
 * One line of the stage profile (issue #38).
 *
 * Microseconds and tenths of a percent, both assembled from integers because
 * svc/fmt.c has no %f -- and the percentages are of prof_total_us, which was
 * measured over the same window as the parts, so the column adds up.
 */
static void cam_prof_line(struct cli_instance *sh, const char *name,
                          uint32_t us, uint32_t total_us, uint32_t iters,
                          const char *what)
{
	uint32_t pct10 = (total_us != 0u)
	               ? (uint32_t)(((uint64_t)us * 1000u) / total_us) : 0u;
	uint32_t per   = (iters != 0u) ? (us / iters) : 0u;

	cli_print(sh, "  %-7s: %6lu us/frame  %2lu.%lu%%   %s\r\n",
	          name, (unsigned long)per,
	          (unsigned long)(pct10 / 10u), (unsigned long)(pct10 % 10u),
	          what);
}

/*
 * The producer's per-stage breakdown, since the last stream start.
 *
 * [!] The point of this is the RATIO, not the fps.  If `wait` dominates, the
 * sensor is not offering frames any faster and pipelining the CPU work would
 * buy nothing; if `pack` and `sink` dominate, it would.  The two cases call for
 * opposite work, which is why the number came before the design (issue #38).
 */
static void cam_print_profile(struct cli_instance *sh,
                              const struct camera_stats *st)
{
	uint32_t fps10;

	if (!st->prof_ok) {
		cli_print(sh, "profile  : -- (%s)\r\n",
		          st->prof_why != NULL ? st->prof_why : "no time source");
		return;
	}
	if (st->prof_iters == 0u || st->prof_total_us == 0u) {
		cli_print(sh, "profile  : -- (no frames since the last stream "
		              "start)\r\n");
		return;
	}

	fps10 = (uint32_t)(((uint64_t)st->prof_iters * 10000000u) /
	                   st->prof_total_us);
	cli_print(sh, "profile  : %lu iterations, %lu us/frame (%lu.%lu fps)"
	              "  [since stream start]\r\n",
	          (unsigned long)st->prof_iters,
	          (unsigned long)(st->prof_total_us / st->prof_iters),
	          (unsigned long)(fps10 / 10u), (unsigned long)(fps10 % 10u));

	cam_prof_line(sh, "wait", st->prof_wait_us, st->prof_total_us,
	              st->prof_iters, "asleep: sensor exposure + readout");
	cam_prof_line(sh, "invald", st->prof_inval_us, st->prof_total_us,
	              st->prof_iters, "D-cache invalidate, 225 KB");
	cam_prof_line(sh, "pack", st->prof_pack_us, st->prof_total_us,
	              st->prof_iters, "planar B/G/R -> RGB565, 76800 px");
	/* NOT the blit -- that left this thread in #57 and has its own row
	 * below.  What is left is whatever the sinks do on the producer: the
	 * inference under `nn preview`, and since #64 the panel thread's staging
	 * copy, which now preempts this thread instead of queueing behind it. */
	cam_prof_line(sh, "sink", st->prof_sink_us, st->prof_total_us,
	              st->prof_iters, "sinks consume: inference + panel staging");
	cam_prof_line(sh, "tune", st->prof_tune_us, st->prof_total_us,
	              st->prof_iters, "means + sensor read-back + wb");
	cam_prof_line(sh, "other", st->prof_other_us, st->prof_total_us,
	              st->prof_iters, "retrigger, wrap reassert, loop");
}

/*
 * The datapath's own frame period, with the display OUT of the path (#38).
 *
 * WHY THIS EXISTS.  The stage profile showed that saving CPU work does not make
 * the preview faster -- 7.8 ms saved in `pack` reappeared as 7.5 ms of `wait`
 * and the total held at ~63 ms.  Something outside the CPU work paces the loop,
 * and two models fit that equally: one frame per 63 ms, or one per ~31.5 ms
 * with this loop taking every other one.  They differ by a factor of two in
 * what is achievable and BOTH predict `wait = 63 ms - work`, so no amount of
 * staring at a preview can separate them.
 *
 * What separates them is making the work small.  This runs the producer with no
 * sink attached, so the LCD blit -- 26.4 ms, the single largest stage -- is not
 * in the loop at all.  What is left is capture, invalidate and pack, about
 * 10 ms with gamma off.  If the total then holds at 63 ms the datapath really
 * does deliver one frame per 63 ms; if it drops, `total` IS the period, read
 * directly.
 *
 * [!] It only reads as a period while `work` is comfortably under it -- which
 * the profile printed below is exactly what to check.  Run it twice, with
 * `camera gamma off` and on, to get two work levels: if the total is the same
 * at both, it is a period and not a multiple of one.
 *
 * No sink is attached and none is detached.
 *
 * [!] AND THAT IS EXACTLY WHY IT NEEDED THE CAMERA'S OWNERSHIP RULE (issue #63).
 * Starting a stream while owning no sink was safe only as long as nobody else's
 * sink was linked -- which the old code did not check, so this command was the
 * one that could take a preview's sink over.  camera_stream_start() now refuses
 * while ANY sink is attached, so a preview that is still tearing down keeps the
 * camera to itself and this command is told to wait.
 */
static int cmd_camera_bench(struct cli_instance *sh, int argc, char **argv)
{
	uint32_t frames = CAM_BENCH_DEFAULT;
	struct camera_stats st;
	uint32_t before;
	int rc;

	if (argc > 1 && cli_parse_u32(argv[1], &frames) != 0) {
		cli_error(sh, "camera: bad frame count '%s'\r\n", argv[1]);
		return 1;
	}
	if (frames == 0u) {
		cli_error(sh, "camera: bench needs a frame count (nothing is on "
		              "the panel to watch)\r\n");
		return 1;
	}

	rc = camera_stream_start(NULL);
	if (rc != CAM_OK) {
		if (rc == CAM_ERR_BUSY)
			cli_error(sh, "camera: a preview owns the camera (a sink "
			              "is attached, or a stream is\r\n"
			              "        running); stop it first\r\n");
		else
			cam_report(sh, "stream start", rc);
		return 1;
	}

	camera_stream_stats(&st);
	before = st.frames;

	for (;;) {
		if (cli_cancel_requested(sh))
			break;
		camera_stream_stats(&st);
		if (!st.streaming)
			break;                      /* the producer gave up */
		if ((st.frames - before) >= frames)
			break;
		if (cli_sleep(sh, 1u) != 0)
			break;
	}

	/* Nothing was attached, so unlike `camera preview` there is nothing to
	 * detach and no reason to condition on a confirmed stop.  A stop that
	 * does not confirm still leaves the camera poisoned, which the profile
	 * below will refuse to describe. */
	rc = camera_stream_stop();

	if (cli_cancel_requested(sh))
		return 0;

	if (rc != CAM_OK) {
		cam_report(sh, "stream stop", rc);
		return 1;
	}

	camera_stream_stats(&st);
	if (st.fault != NULL) {
		cli_error(sh, "camera: stopped: %s\r\n", st.fault);
		return 1;
	}

	cli_print(sh, "no sink attached: the LCD blit is out of this loop\r\n");
	cam_print_profile(sh, &st);
	return 0;
}

/* ---- stats --------------------------------------------------------------- */

static int cmd_camera_stats(struct cli_instance *sh, int argc, char **argv)
{
	struct cam_lcd_sink_stats sk;
	struct camera_stats st;

	(void)argc; (void)argv;

	camera_stream_stats(&st);
	cam_lcd_sink_stats(&sk);

	cli_print(sh, "state    : %s\r\n", st.streaming ? "streaming" : "idle");
	cli_print(sh, "frames   : %lu\r\n", (unsigned long)st.frames);
	cli_print(sh, "timeouts : %lu (restarts %lu)\r\n",
	          (unsigned long)st.timeouts, (unsigned long)st.retries);
	cli_print(sh, "overruns : %lu\r\n", (unsigned long)st.overruns);
	cli_print(sh, "csirx err: %lu (relock fail %lu)\r\n",
	          (unsigned long)st.csirx_errors,
	          (unsigned long)st.relock_fails);
	cli_print(sh, "dp errors: %lu", (unsigned long)st.dp_errors);
	if (st.last_dp_status != 0)
		cli_print(sh, " (last status %ld)", (long)st.last_dp_status);
	cli_print(sh, "\r\n");
	cam_print_profile(sh, &st);
	cli_print(sh, "sink lcd : %lu shown, %lu dropped, %lu busy, %lu err\r\n",
	          (unsigned long)sk.delivered, (unsigned long)sk.dropped,
	          (unsigned long)sk.busy, (unsigned long)sk.errors);
	/*
	 * [!] WHERE THE 26 ms WENT (issue #57).
	 *
	 * The producer's `sink` row above is now just the hand-off (plus the
	 * inference under `nn preview`); the blit itself is timed on the panel
	 * thread and reported here.  Both numbers are needed: "the producer's
	 * went down" is also what a sink that silently stopped drawing looks
	 * like, and only this row distinguishes moved from lost.
	 */
	if (!sk.prof_ok)
		cli_print(sh, "  blit   : -- (%s)\r\n",
		          sk.prof_why != NULL ? sk.prof_why : "no time source");
	else if (sk.blit_frames == 0u)
		cli_print(sh, "  blit   : -- (no frames since the last "
		              "attach)\r\n");
	else
		cli_print(sh, "  blit   : %6lu us/frame over %lu frame(s) "
		              "[panel thread]\r\n",
		          (unsigned long)(sk.blit_us / sk.blit_frames),
		          (unsigned long)sk.blit_frames);
	/*
	 * [!] AND HOW LONG THE SLOT IS HELD (issue #71), which is NOT the row
	 * above.  `blit` is the panel thread's whole interval; this stops when
	 * the pin goes back at the staging seam, before the transfer.  The gap
	 * between the two is exactly what moving the release bought, and this is
	 * the number the frame period is now bounded by -- so it is the one to
	 * read before picking a frame length with `camera vts`.
	 */
	if (sk.prof_ok && sk.hold_frames != 0u)
		cli_print(sh, "  held   : %6lu us/frame over %lu frame(s) "
		              "[to pin returned]\r\n",
		          (unsigned long)(sk.hold_us / sk.hold_frames),
		          (unsigned long)sk.hold_frames);
	/*
	 * A stream in flight has one frame out, so `accepted` leading `puts` by
	 * one is normal and not worth printing.  Any other gap is worth seeing
	 * at once -- detach turns it into a poisoned sink, and this is how to
	 * catch it before then.
	 */
	if (sk.accepted != sk.puts)
		cli_print(sh, "  handed : %lu accepted, %lu released%s\r\n",
		          (unsigned long)sk.accepted, (unsigned long)sk.puts,
		          (sk.accepted - sk.puts == 1u) ? " (one in flight)"
		                                        : " [!]");
	if (sk.fault != NULL)
		cli_print(sh, "sink err : %s\r\n", sk.fault);
	/* Only ever non-zero after an `nn preview`: a frame whose inference was
	 * refused is still SHOWN, just without boxes, so it counts here and not
	 * as a sink error. */
	if (sk.overlay_errors != 0u)
		cli_print(sh, "overlay  : %lu frame(s) shown without boxes\r\n",
		          (unsigned long)sk.overlay_errors);
	if (st.fault != NULL)
		cli_print(sh, "fault    : %s\r\n", st.fault);
	return 0;
}

/* ---- exposure / gain / white balance ------------------------------------- */

/*
 * WHY THESE ARE RUNTIME KNOBS AND NOT CONSTANTS.
 *
 * There is no automatic exposure and no automatic white balance anywhere in
 * this datapath: the donor applications feed the raw output straight to a
 * neural network, which does not care what it looks like.  So the sensor sits
 * at whatever fixed exposure and gain it was programmed with, and the right
 * values depend on the room.
 *
 * Finding them by editing a #define costs one flash cycle per guess, on a part
 * whose external NOR is rated ~100k of them and whose flashing is a manual,
 * press-the-button affair.  Finding them from the console costs nothing.  Once
 * a set is known good it can be baked into the defaults in camera.c.
 *
 * The white balance is separate from the gains and has to be, because the
 * sensor has no per-channel gain registers -- analogue and digital gain both
 * move all three channels together, so neither can correct a cast.  A cast is
 * the normal state of an uncorrected Bayer sensor: twice as many green
 * photosites, and greener filters.
 */
/*
 * The values printed above are what the driver last WROTE to the sensor.  With
 * a stream running a new setting is queued for the producer to apply between
 * frames, so it has not been written yet and the read-back is still the old
 * one -- say so, rather than print a number that looks like it did not take.
 */
static void cam_note_queued(struct cli_instance *sh, int argc)
{
	struct camera_stats st;

	if (argc <= 1)
		return;
	camera_stream_stats(&st);
	if (st.streaming)
		cli_print(sh, "           (queued: applied between frames; the "
		              "value above is the previous one)\r\n");
}

/*
 * Say that a manual exposure or gain has taken `camera auto` off (issue #39).
 *
 * The state change is real and the user did not ask for it in so many words, so
 * it is announced rather than left to be discovered by a later `camera auto`
 * printing "off".  Only when it actually changed: repeating `camera exposure`
 * on an already-manual camera should not keep saying so.
 */
static void cam_note_manual(struct cli_instance *sh, int was_auto)
{
	if (!was_auto || camera_auto())
		return;
	cli_print(sh, "           (auto is now OFF: a manual value takes the "
	              "sensor's own AEC off, and\r\n"
	              "            freezes the white balance with it.  "
	              "`camera auto on` hands both back)\r\n");
}

/*
 * [!] ONE ARGUMENT (issue #54).  This used to take a frame length too, because
 * the IMX219 keeps one at 0x0160/0x0161 and clamps its exposure against it.
 * The OV5647 keeps neither quantity where the IMX219 does -- its frame length
 * is the VTS pair at 0x380E/0x380F -- so with the IMX219 gone there is nothing
 * behind that argument, and it went with the sensor rather than becoming a
 * second address this port would have to be careful about.  (SCCB acknowledges
 * a write to an address that means something else, so "supported" and "lands
 * somewhere unintended" look identical from here.)
 */
static int cmd_camera_exposure(struct cli_instance *sh, int argc, char **argv)
{
	uint16_t lines, dgain;
	uint8_t again;
	uint32_t v;
	int was_auto = camera_auto();

	if (argc > 1) {
		if (cli_parse_u32(argv[1], &v) != 0 || v > 0xFFFFu) {
			cli_error(sh, "camera: exposure must be 0..65535\r\n");
			return 1;
		}
		if (camera_set_exposure((uint16_t)v) != CAM_OK) {
			cli_error(sh, "camera: exposure write failed (is the "
			              "module powered?  try `camera probe`)\r\n");
			return 1;
		}
	}

	cam_sensor_get_exposure_gains(&lines, &again, &dgain);
	cli_print(sh, "exposure : %lu lines\r\n", (unsigned long)lines);
	cam_note_queued(sh, argc);
	cam_note_manual(sh, was_auto);
	return 0;
}

static int cmd_camera_gain(struct cli_instance *sh, int argc, char **argv)
{
	uint16_t lines, dgain;
	uint8_t again;
	uint32_t a, d;
	int was_auto = camera_auto();

	if (argc > 1) {
		cam_sensor_get_exposure_gains(&lines, &again, &dgain);
		a = again;
		d = dgain;

		if (cli_parse_u32(argv[1], &a) != 0 || a > 232u) {
			cli_error(sh, "camera: analogue gain must be 0..232\r\n");
			return 1;
		}
		if (argc > 2 && (cli_parse_u32(argv[2], &d) != 0 ||
		                 d < 0x0100u || d > 0x0FFFu)) {
			cli_error(sh, "camera: digital gain must be 256..4095 "
			              "(256 = 1.0x)\r\n");
			return 1;
		}
		if (camera_set_gains((uint8_t)a, (uint16_t)d) != CAM_OK) {
			cli_error(sh, "camera: gain write failed (is the module "
			              "powered?  try `camera probe`)\r\n");
			return 1;
		}
	}

	cam_sensor_get_exposure_gains(&lines, &again, &dgain);
	cli_print(sh, "again    : %u\r\n", again);
	cli_print(sh, "dgain    : %lu (%lu.%02lux)\r\n", (unsigned long)dgain,
	          (unsigned long)(dgain / 256u),
	          (unsigned long)(((dgain % 256u) * 100u) / 256u));
	cam_note_queued(sh, argc);
	cam_note_manual(sh, was_auto);
	return 0;
}

/*
 * The sensor's frame length -- frame rate and exposure ceiling in one register
 * (issue #38).
 *
 * WHY IT IS A KNOB.  The SDK's mode table never wrote VTS, so the part ran a
 * VGA mode on the 5 MP mode's frame length: 1968 lines, 62.5 ms, 15.9 fps.  The
 * fix is one register, but WHICH value is a trade this board cannot decide for
 * anyone -- lower is faster and caps the exposure, higher is the reverse -- and
 * finding the point by editing a #define costs a flash cycle per guess on a
 * NOR rated ~100k of them.
 *
 * The read-back is from the SENSOR, deliberately.  A getter that reported this
 * port's own last write could not have found the bug it exists to expose.
 */
static int cmd_camera_vts(struct cli_instance *sh, int argc, char **argv)
{
	uint16_t rb = 0u;
	uint32_t v;
	int rc;

	if (argc > 1) {
		if (cli_parse_u32(argv[1], &v) != 0 || v > 0xFFFFu) {
			cli_error(sh, "camera: frame length must be 0..65535 "
			              "lines\r\n");
			return 1;
		}
		if (camera_set_frame_length((uint16_t)v) != CAM_OK) {
			cli_error(sh, "camera: frame length write failed (below "
			              "what the mode needs, or the module is not "
			              "powered)\r\n");
			return 1;
		}
	}

	cli_print(sh, "vts      : %lu lines (programmed)\r\n",
	          (unsigned long)cam_sensor_frame_length());

	rc = camera_read_frame_length(&rb);
	if (rc == CAM_OK)
		cli_print(sh, "  sensor says : %lu\r\n", (unsigned long)rb);
	else if (rc == CAM_ERR_BUSY)
		cli_print(sh, "  (read-back needs an idle camera; stop the "
		              "preview)\r\n");
	else
		cli_print(sh, "  (this sensor's frame length is not readable "
		              "here)\r\n");

	/* [!] NOT vts x 31.8 us.  The datapath is one-shot, so a frame costs the
	 * producer's work plus a whole active frame (15.1 ms) and the result is
	 * rounded UP to a multiple of the sensor's period -- which is why 984 and
	 * 1968 both measure 62 ms today.  Saying the naive formula here would
	 * invite exactly the tuning that falls off the cliff at 1860.
	 *
	 * 31.8 and not the 31.507 this used to print: that figure is HTS 1852
	 * over a 58.8 MHz PCLK, and every N=1 run measures about 0.9% higher --
	 * 450 us at these frame lengths, the same order as the margin somebody
	 * reading this line would be tuning against. */
	cli_print(sh, "           (period = vts x 31.8 us, but ROUNDED UP to a "
	              "whole one that fits\r\n"
	              "            the producer's work + 15.1 ms -- so lowering "
	              "vts often changes\r\n"
	              "            nothing.  `camera bench` measures it.  Lower "
	              "also caps the exposure.)\r\n");
	cam_note_queued(sh, argc);
	return 0;
}

static int cmd_camera_wb(struct cli_instance *sh, int argc, char **argv)
{
	struct cam_wb wb;
	uint32_t r, g, b;

	if (argc > 1) {
		if (argc != 4) {
			cli_error(sh, "camera: wb takes three gains "
			              "(r g b, 256 = unity)\r\n");
			return 1;
		}
		if (cli_parse_u32(argv[1], &r) != 0 ||
		    cli_parse_u32(argv[2], &g) != 0 ||
		    cli_parse_u32(argv[3], &b) != 0 ||
		    r > 4096u || g > 4096u || b > 4096u) {
			cli_error(sh, "camera: gains must be 0..4096 "
			              "(256 = unity)\r\n");
			return 1;
		}
		/* Gains only.  Everything else in the struct belongs to another
		 * subcommand and must survive this one untouched -- which means
		 * READING the live settings first, because camera_set_wb() takes
		 * the whole struct.  Filling in three fields of an automatic and
		 * handing it over posts stack garbage as the black level, the
		 * gamma flag and the saturation. */
		camera_get_wb(&wb);
		wb.r = (uint16_t)r;
		wb.g = (uint16_t)g;
		wb.b = (uint16_t)b;
		camera_set_wb(&wb);
	}

	camera_get_wb(&wb);
	cli_print(sh, "wb       : r %u  g %u  b %u  (256 = unity)\r\n",
	          wb.r, wb.g, wb.b);
	cli_print(sh, "           (applied in software: the sensor has no "
	              "per-channel gain.  `camera black`\r\n"
	              "            for the pedestal, `camera auto` drives these "
	              "while a stream runs)\r\n");
	return 0;
}

static int cmd_camera_auto(struct cli_instance *sh, int argc, char **argv)
{
	uint16_t lines, dgain;
	uint8_t again;
	struct cam_wb wb;

	if (argc > 1) {
		int on;

		if (strcmp(argv[1], "on") == 0)
			on = 1;
		else if (strcmp(argv[1], "off") == 0)
			on = 0;
		else {
			cli_error(sh, "camera: auto takes on or off\r\n");
			return 1;
		}
		/* Reported, not discarded: on a sensor with its own AEC this
		 * carries a register write, and a silent failure there means
		 * `camera exposure` quietly stops holding. */
		if (camera_set_auto(on) != CAM_OK) {
			cli_error(sh, "camera: could not set the sensor's own "
			              "exposure loop (is the module powered?  "
			              "try `camera probe`)\r\n");
			return 1;
		}
	}

	cam_sensor_get_exposure_gains(&lines, &again, &dgain);
	camera_get_wb(&wb);

	cli_print(sh, "auto     : %s  (exposure by the sensor's own AEC, "
	              "wb here)\r\n", camera_auto() ? "on" : "off");
	cli_print(sh, "  now    : exposure %lu  again %u  wb r %u b %u\r\n",
	          (unsigned long)lines, again, wb.r, wb.b);
	if (camera_auto())
		cli_print(sh, "           (the loops steer these while a stream "
		              "runs; turn auto OFF before\r\n"
		              "            taking measurements that assume the "
		              "sensor is holding still)\r\n");
	return 0;
}

/*
 * The black level has its own subcommand.  It is not a white-balance parameter
 * -- it is the sensor's fixed pedestal, it pairs with the gamma curve, and it
 * moves contrast directly -- and burying a control people reach for inside
 * another command's optional argument is how it becomes unfindable.
 */
static int cmd_camera_black(struct cli_instance *sh, int argc, char **argv)
{
	struct cam_wb wb;
	uint32_t v;

	camera_get_wb(&wb);
	if (argc > 1) {
		if (cli_parse_u32(argv[1], &v) != 0 || v > 255u) {
			cli_error(sh, "camera: black level must be 0..255\r\n");
			return 1;
		}
		wb.black = (uint8_t)v;
		camera_set_wb(&wb);
	}

	cli_print(sh, "black    : %u  (subtracted before the gain)\r\n",
	          wb.black);
	cli_print(sh, "           (16 is the sensor's BLC target and the TOP of "
	              "the measured floor,\r\n"
	              "            which spans 8..16 with a mean near 13 -- the "
	              "top is the useful\r\n"
	              "            end, since the gamma curve multiplies whatever "
	              "is left.  Above 16\r\n"
	              "            you trade shadow detail for contrast, which is "
	              "a scene call.\r\n"
	              "            `camera capture` prints each plane's minimum "
	              "-- that is the floor)\r\n");
	return 0;
}

static int cmd_camera_sat(struct cli_instance *sh, int argc, char **argv)
{
	struct cam_wb wb;
	uint32_t v;

	camera_get_wb(&wb);
	if (argc > 1) {
		if (cli_parse_u32(argv[1], &v) != 0 || v > 2048u) {
			cli_error(sh, "camera: saturation must be 0..2048 "
			              "(256 = unchanged)\r\n");
			return 1;
		}
		wb.sat = (uint16_t)v;
		camera_set_wb(&wb);
	}

	cli_print(sh, "sat      : %u  (256 = unchanged)\r\n", wb.sat);
	cli_print(sh, "           (stands in for the colour correction matrix "
	              "this pipeline has not\r\n"
	              "            got: raw sensor RGB is desaturated because "
	              "the colour filters\r\n"
	              "            overlap, and no exposure or white balance "
	              "fixes that)\r\n");
	return 0;
}

static int cmd_camera_gamma(struct cli_instance *sh, int argc, char **argv)
{
	struct cam_wb wb;

	camera_get_wb(&wb);
	if (argc > 1) {
		if (strcmp(argv[1], "on") == 0)
			wb.gamma = 1u;
		else if (strcmp(argv[1], "off") == 0)
			wb.gamma = 0u;
		else {
			cli_error(sh, "camera: gamma takes on or off\r\n");
			return 1;
		}
		camera_set_wb(&wb);
	}

	cli_print(sh, "gamma    : %s   black %u\r\n",
	          wb.gamma ? "sRGB" : "off (linear)", wb.black);
	cli_print(sh, "           (the sensor is linear and the panel expects "
	              "encoded data;\r\n"
	              "            off is for comparison, not a neutral "
	              "setting)\r\n");
	if (wb.gamma && wb.black == 0u)
		cli_print(sh, "           [!] gamma with black 0 lifts the "
		              "pedestal to grey and looks washed out --\r\n"
		              "               set one (`camera black 16`)\r\n");
	return 0;
}

static int cmd_camera_bayer(struct cli_instance *sh, int argc, char **argv)
{
	static const char *const names[4] = { "bggr", "gbrg", "grbg", "rggb" };
	uint8_t i;

	if (argc > 1) {
		for (i = 0u; i < 4u; i++)
			if (strcmp(argv[1], names[i]) == 0)
				break;
		if (i == 4u) {
			cli_error(sh, "camera: bayer must be one of "
			              "bggr gbrg grbg rggb\r\n");
			return 1;
		}
		cam_dp_set_bayer(i);
	}

	cli_print(sh, "bayer    : %s\r\n",
	          cam_dp_bayer_name(cam_dp_bayer()));
	if (argc > 1)
		cli_print(sh, "           (takes effect at the next `camera "
		              "capture` or `camera preview`)\r\n");
	return 0;
}

/* ---- registration -------------------------------------------------------- */

CLI_SUBCMD_SET_CREATE(camera_subcmds,
	CLI_CMD(probe, NULL, "power the module and read its sensor ID",
	        cmd_camera_probe),
	CLI_CMD(capture, NULL, "one frame + per-channel statistics",
	        cmd_camera_capture),
	CLI_CMD_ARG_USAGE(preview, NULL,
	                  "live preview on the panel, Ctrl+C to stop",
	                  "[frames]", cmd_camera_preview, 1, 1),
	CLI_CMD(raw, NULL, "capture the Bayer mosaic and name the phase",
	        cmd_camera_raw),
	CLI_CMD(stats, NULL, "producer and sink counters", cmd_camera_stats),
	CLI_CMD_ARG_USAGE(vts, NULL,
	                  "sensor frame length: frame rate vs exposure ceiling",
	                  "[lines]", cmd_camera_vts, 1, 1),
	CLI_CMD_ARG_USAGE(bench, NULL,
	                  "time the datapath with the panel out of the loop",
	                  "[frames]", cmd_camera_bench, 1, 1),
	CLI_CMD_ARG_USAGE(exposure, NULL,
	                  "read or set the sensor's exposure",
	                  "[lines]", cmd_camera_exposure, 1, 1),
	CLI_CMD_ARG_USAGE(gain, NULL,
	                  "read or set analogue / digital gain",
	                  "[again [dgain]]", cmd_camera_gain, 1, 2),
	CLI_CMD_ARG_USAGE(auto, NULL,
	                  "the sensor's own AEC + this port's white balance",
	                  "[on|off]", cmd_camera_auto, 1, 1),
	CLI_CMD_ARG_USAGE(black, NULL,
	                  "black level subtracted before the gain (default 16)",
	                  "[n]", cmd_camera_black, 1, 1),
	CLI_CMD_ARG_USAGE(sat, NULL,
	                  "saturation, standing in for a colour matrix "
	                  "(256 = unchanged)",
	                  "[n]", cmd_camera_sat, 1, 1),
	CLI_CMD_ARG_USAGE(gamma, NULL,
	                  "sRGB encode the preview (on by default)",
	                  "[on|off]", cmd_camera_gamma, 1, 1),
	CLI_CMD_ARG_USAGE(bayer, NULL,
	                  "demosaic phase: bggr|gbrg|grbg|rggb (try all four)",
	                  "[mode]", cmd_camera_bayer, 1, 1),
	CLI_CMD_ARG_USAGE(wb, NULL,
	                  "software white balance (256 = unity)",
	                  "[r g b]", cmd_camera_wb, 1, 3),
	CLI_SUBCMD_SET_END);

CLI_CMD_REGISTER(camera, camera_subcmds,
                 "OV5647 camera: bring-up, capture, live preview",
                 NULL, 1, 0);
