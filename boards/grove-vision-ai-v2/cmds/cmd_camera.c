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
	          cam_imx219_sensor_name(), info.sensor_id);
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
		          cam_imx219_bayer_name(cam_imx219_bayer()),
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

	/* No overlay: a plain preview stays a plain preview.  It is an argument
	 * rather than a mode so that `nn preview`'s boxes can never be inherited
	 * by this command (issue #48). */
	rc = cam_lcd_sink_attach(NULL);
	if (rc == CAM_ERR_BUSY) {
		cli_error(sh, "camera: preview already running\r\n");
		return 1;
	}
	if (rc != CAM_OK) {
		cam_report(sh, "panel attach", rc);
		return 1;
	}

	rc = camera_stream_start();
	if (rc != CAM_OK) {
		/* CAM_ERR_BUSY means a stream is already running with this sink
		 * attached, so a producer may be inside consume() -- detaching
		 * there would unlink a sink mid delivery.  See the same guard in
		 * `nn preview` (issue #48). */
		if (rc != CAM_ERR_BUSY)
			(void)cam_lcd_sink_detach();
		cam_report(sh, "stream start", rc);
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
	 */
	rc = camera_stream_stop();
	if (rc == CAM_OK)
		(void)cam_lcd_sink_detach();

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
	cli_print(sh, "sink lcd : %lu shown, %lu dropped, %lu busy, %lu err\r\n",
	          (unsigned long)sk.delivered, (unsigned long)sk.dropped,
	          (unsigned long)sk.busy, (unsigned long)sk.errors);
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

	cam_imx219_get_exposure_gains(&lines, &again, &dgain);
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
		cam_imx219_get_exposure_gains(&lines, &again, &dgain);
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

	cam_imx219_get_exposure_gains(&lines, &again, &dgain);
	cli_print(sh, "again    : %u\r\n", again);
	cli_print(sh, "dgain    : %lu (%lu.%02lux)\r\n", (unsigned long)dgain,
	          (unsigned long)(dgain / 256u),
	          (unsigned long)(((dgain % 256u) * 100u) / 256u));
	cam_note_queued(sh, argc);
	cam_note_manual(sh, was_auto);
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

	cam_imx219_get_exposure_gains(&lines, &again, &dgain);
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
	cli_print(sh, "           (the sensor's own pedestal is 16 and does not "
	              "vary; above that\r\n"
	              "            you are trading shadow detail for contrast, "
	              "which is a scene call.\r\n"
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
		cam_imx219_set_bayer(i);
	}

	cli_print(sh, "bayer    : %s\r\n",
	          cam_imx219_bayer_name(cam_imx219_bayer()));
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
	                  "black level subtracted before the gain (pedestal 16)",
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
