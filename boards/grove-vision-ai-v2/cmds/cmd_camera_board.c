/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    cmd_camera_board.c
 * @brief   This board's own `camera` subcommands (Grove Vision AI V2, issue
 *          #35 / M-G3b, issue #143).
 *
 * The command itself -- probe / capture / stream start|stop|stats -- is
 * shell/cmds/cmd_camera.c, shared with the other boards, over this board's
 * adapter port/camera/camera_svc_grove.c.  What stays here is appended to that
 * table by CAMERA_SVC_BOARD_SUBCMDS (svc/camera_svc_config.h): `raw`,
 * `profile`, `vts`, `bench`, and the image knobs -- plus camera_board_send(),
 * the YMODEM half of `camera send`.
 *
 * The questions, in the order you would ask them when nothing works yet:
 *
 *   camera probe    -- is a module there at all?  (the adapter)
 *   camera capture  -- take ONE frame and describe it, per channel, straight
 *                      off the hardware's own planar output.  A dead MIPI lane,
 *                      a sensor that never streamed and a demosaic given the
 *                      wrong Bayer phase all produce a frame of the right size
 *                      at the right time with every layer reporting success;
 *                      the channel balance is what tells them apart.  (the
 *                      adapter)
 *   camera stream start / stop / stats
 *                   -- continuous capture onto the panel (issue #142), and the
 *                      counters and who holds it.
 *   camera profile  -- where the producer's time per frame goes.
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
#include "camera_svc.h"
#include "camera_svc_config.h" /* CAMERA_SVC_BOARD_DECLS: the names declared */
#include "camera_svc_grove.h"  /* the profile lines, shared with the adapter */
#include "camera_cmd_board.h"  /* camera_board_send() */
#include "cmd_xfer.h"          /* xfer_send_source(): YMODEM over the console */

/* What the shared table expands; declared once, there and here. */
CAMERA_SVC_BOARD_DECLS

/* The writer the adapter's lines go through here: one cli_write() per line,
   as the shared command's (shell/cmds/cmd_camera.c). */
static int board_write(void *ctx, const char *s, size_t len)
{
	if (len == 0u)
		return 0;
	return (cli_write((struct cli_instance *)ctx, s, len) < 0) ? -1 : (int)len;
}

#define CAM_BENCH_DEFAULT  60u   /* enough to average, ~4 s at 15 fps  */

static void cam_report(struct cli_instance *sh, const char *what, int rc)
{
	struct camera_stats st;

	cli_error(sh, "camera: %s failed: %s\r\n", what, camera_strerror(rc));

	/* The driver latches a specific reason for every failure it can name.
	 * Making the operator know to run `camera stream stats` to see it is a way of
	 * hiding the one useful sentence behind a second command. */
	camera_stream_stats(&st);
	if (st.fault != NULL)
		cli_error(sh, "        %s\r\n", st.fault);
	if (st.last_dp_status != 0)
		cli_error(sh, "        last datapath status %ld\r\n",
		          (long)st.last_dp_status);
}

/*
 * Nothing has read the sensor back yet (issue #55).
 *
 * `--` rather than the shadow's zero, because `exposure 0` is not visibly a
 * placeholder: it reads as an exposure of zero, which is a number an operator
 * can act on.  The window is short -- a bring-up fills these, and `camera
 * probe` is a bring-up -- but it is exactly the window somebody is in when they
 * are working out whether the module is alive at all.
 */
static void cam_print_unread(struct cli_instance *sh, const char *what)
{
	cli_print(sh, "%-9s: -- (nothing has read the sensor back yet; "
	              "`camera probe`)\r\n", what);
}

/*
 * A stop that did not confirm, said accurately (issue #65).
 *
 * The two failures need different sentences because they leave the board in
 * different places, and the old single message described the wrong one for the
 * commoner case: a lock collision was reported as a camera that had poisoned
 * itself.  What they share is the part that is not advice -- nothing that was
 * still in use was torn down.
 *
 * [!] @p retry SINCE ISSUE #142: a locked `camera stream stop` changed nothing
 * and can simply be typed again -- the holder table still has the operator.
 * `camera bench` has no such second chance: its stream has no holder and no
 * sink, so the old advice stands for it.
 */
static void cam_report_stop(struct cli_instance *sh, int rc, int retry)
{
	cam_report(sh, "stream stop", rc);

	if (rc == CAM_ERR_LOCKED && retry)
		cli_error(sh, "        nothing was asked of the producer and "
		              "nothing was torn down -- retry\r\n");
	else if (rc == CAM_ERR_LOCKED)
		cli_error(sh, "        nothing was asked of the producer and "
		              "nothing was torn down;\r\n"
		              "        the sink stays attached, and a reboot "
		              "is what clears it\r\n");
	else if (rc == CAM_ERR_TIMEOUT)
		cli_error(sh, "        the producer is still running; the "
		              "camera is unusable until reboot\r\n");
	else if (rc == CAM_ERR_STATE && retry)
		cli_error(sh, "        the camera is unusable until "
		              "reboot\r\n");
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
int camera_board_cmd_raw(struct cli_instance *sh, int argc, char **argv)
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
int camera_board_cmd_bench(struct cli_instance *sh, int argc, char **argv)
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

	/* Nothing was attached, so unlike the panel's stream there is nothing
	 * to detach and no reason to condition on a confirmed stop.  A stop that
	 * does not confirm still leaves the camera poisoned, which the profile
	 * below will refuse to describe. */
	rc = camera_stream_stop();

	if (cli_cancel_requested(sh))
		return 0;

	if (rc != CAM_OK) {
		cam_report_stop(sh, rc, 0);
		return 1;
	}

	camera_stream_stats(&st);
	if (st.fault != NULL) {
		cli_error(sh, "camera: stopped: %s\r\n", st.fault);
		return 1;
	}

	cli_print(sh, "no sink attached: the LCD blit is out of this loop\r\n");
	{
		struct camera_out o = { board_write, sh, 0 };

		camera_grove_profile_write(&o, &st);
	}
	return 0;
}

/* ---- profile ----------------------------------------------------------- */

/*
 * `camera profile` (issue #143 D3): the producer's per-stage breakdown since the
 * last stream start -- the eight lines `camera stats` used to end with.  The
 * counters themselves are `camera stream stats` now.  One stats sample; the
 * lines are the adapter's, shared with `camera bench`.
 */
int camera_board_cmd_profile(struct cli_instance *sh, int argc, char **argv)
{
	struct camera_out o = { board_write, sh, 0 };
	struct camera_stats st;

	(void)argc; (void)argv;
	camera_stream_stats(&st);
	camera_grove_profile_write(&o, &st);
	return o.failed ? 1 : 0;
}

/* ---- send (shell/cmds/camera_cmd_board.h) ------------------------------- */

/* The last capture to the PC over YMODEM: the console is claimed here, in the
   shell layer, and the frame is read through the adapter (one hold of the
   camera API per block, never across the transfer). */
int camera_board_send(struct cli_instance *sh, const struct ym_source *src)
{
	return xfer_send_source(sh, src);
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
 * A tuning write that was refused rather than failed (issue #77).
 *
 * [!] IT REPLACES THE USUAL MESSAGE, it does not follow it.  The usual one asks
 * whether the module is powered and suggests `camera probe` -- advice that is
 * simply wrong here, and `camera probe` is one of the very commands that can be
 * holding the thing in the way.
 *
 * [!] AND NOT THE SAME TWO CAUSES AS `camera probe`'s own message.  A running
 * stream does not make these refuse -- it makes them QUEUE, and that is a
 * success.  So the only way to reach CAM_ERR_BUSY here is API-mutex
 * contention, and naming a preview would send the operator after the wrong
 * thing.  Naming what CAN hold it matters because the answer is "wait": the
 * producer never takes that mutex, so nothing about a steady stream is holding
 * it.  It is a start, a stop, a capture or a probe, all of them short.
 */
static void cam_note_api_busy(struct cli_instance *sh, const char *what)
{
	cli_error(sh, "camera: %s refused: another camera command holds the "
	              "API\r\n", what);
	cli_error(sh, "        (a stream start or stop, a capture, or a "
	              "probe).  Nothing was\r\n"
	              "        written -- try again\r\n");
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
int camera_board_cmd_exposure(struct cli_instance *sh, int argc, char **argv)
{
	uint16_t lines, dgain;
	uint8_t again;
	uint32_t v;
	int was_auto = camera_auto();
	int rc;

	if (argc > 1) {
		uint16_t vts = cam_sensor_frame_length();

		if (cli_parse_u32(argv[1], &v) != 0 || v > 0xFFFFu) {
			cli_error(sh, "camera: exposure must be 0..65535\r\n");
			return 1;
		}
		/*
		 * [!] SAID HERE AS WELL AS ENFORCED IN THE DRIVER (issue #70).
		 *
		 * The driver refuses this, but during a stream the write is
		 * QUEUED for the producer -- camera_set_exposure() returns
		 * CAM_OK before anything reaches the sensor -- so the refusal
		 * would happen on a thread with no console to report it, and
		 * this command would print success.  Checking here is what makes
		 * the answer arrive where the question was asked.
		 */
		if (vts != 0u && v > vts) {
			cli_error(sh, "camera: %lu lines is longer than the "
			              "frame (vts %u)\r\n", (unsigned long)v,
			          vts);
			cli_error(sh, "        the sensor would stretch the "
			              "frame to fit it, so this would change "
			              "the\r\n"
			              "        frame rate instead.  Raise the "
			              "frame length first: `camera vts %lu`\r\n",
			          (unsigned long)v);
			return 1;
		}
		rc = camera_set_exposure((uint16_t)v);
		if (rc != CAM_OK) {
			if (rc == CAM_ERR_BUSY)
				cam_note_api_busy(sh, "exposure write");
			else
				cli_error(sh, "camera: exposure write failed "
				              "(is the module powered?  try "
				              "`camera probe`)\r\n");
			return 1;
		}
	}

	cam_sensor_get_exposure_gains(&lines, &again, &dgain);
	if (cam_sensor_exposure_gains_valid())
		cli_print(sh, "exposure : %lu lines\r\n", (unsigned long)lines);
	else
		cam_print_unread(sh, "exposure");
	cam_note_queued(sh, argc);
	cam_note_manual(sh, was_auto);
	return 0;
}

int camera_board_cmd_gain(struct cli_instance *sh, int argc, char **argv)
{
	uint16_t lines, dgain;
	uint8_t again;
	uint32_t a, d;
	int was_auto = camera_auto();
	int rc;

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
		rc = camera_set_gains((uint8_t)a, (uint16_t)d);
		if (rc != CAM_OK) {
			if (rc == CAM_ERR_BUSY)
				cam_note_api_busy(sh, "gain write");
			else
				cli_error(sh, "camera: gain write failed (is "
				              "the module powered?  try "
				              "`camera probe`)\r\n");
			return 1;
		}
	}

	cam_sensor_get_exposure_gains(&lines, &again, &dgain);
	if (cam_sensor_exposure_gains_valid())
		cli_print(sh, "again    : %u\r\n", again);
	else
		cam_print_unread(sh, "again");
	/* dgain is not read back from anything -- the OV5647 has no digital gain
	 * and the shadow is a unity this port keeps for the API's shape -- so it
	 * is not part of the question `--` answers. */
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
int camera_board_cmd_vts(struct cli_instance *sh, int argc, char **argv)
{
	uint16_t rb = 0u;
	uint16_t lines, dgain, capped = 0u;
	uint8_t again;
	uint32_t v;
	int rc;

	if (argc > 1) {
		if (cli_parse_u32(argv[1], &v) != 0 || v > 0xFFFFu) {
			cli_error(sh, "camera: frame length must be 0..65535 "
			              "lines\r\n");
			return 1;
		}
		/* What the driver is about to do to a manual exposure that no
		 * longer fits (issue #70), worked out BEFORE the write so the
		 * old value can be named -- and here rather than from the
		 * result, because during a stream the write is queued and the
		 * capping happens on the producer thread. */
		cam_sensor_get_exposure_gains(&lines, &again, &dgain);
		if (!camera_auto() && cam_sensor_exposure_gains_valid() &&
		    lines > (uint16_t)v)
			capped = lines;

		rc = camera_set_frame_length((uint16_t)v);
		if (rc != CAM_OK) {
			if (rc == CAM_ERR_BUSY)
				cam_note_api_busy(sh, "frame length write");
			else
				cli_error(sh, "camera: frame length write "
				              "failed (below what the mode "
				              "needs, or the module is not "
				              "powered)\r\n");
			return 1;
		}
	}

	/*
	 * [!] THE READ-BACK FIRST, THEN BOTH LINES (issue #55).
	 *
	 * `programmed` is a shadow that a bring-up fills -- and the read-back
	 * below is what RUNS that bring-up on a cold board.  Printing it first
	 * answered `camera vts` with "0 lines (programmed)" on the first camera
	 * command after a flash, while the sensor beside it said 940: the
	 * command was reporting a value it was about to go and fetch.
	 */
	rc = camera_read_frame_length(&rb);

	cli_print(sh, "vts      : %lu lines (programmed)\r\n",
	          (unsigned long)cam_sensor_frame_length());
	if (capped != 0u)
		cli_print(sh, "  exposure capped to %lu lines (was %u; it "
		              "cannot exceed the frame)\r\n",
		          (unsigned long)v, capped);

	if (rc == CAM_OK)
		cli_print(sh, "  sensor says : %lu\r\n", (unsigned long)rb);
	else if (rc == CAM_ERR_BUSY)
		/* Both causes (issue #77): the producer owns the bus while a
		   stream runs, and since #77 the read also refuses when another
		   camera command holds the API.  Nothing was read either way. */
		cli_print(sh, "  (read-back needs an idle camera: stop the "
		              "preview, or another camera\r\n"
		              "   command holds the API)\r\n");
	else
		cli_print(sh, "  (this sensor's frame length is not readable "
		              "here)\r\n");

	/* [!] NOT simply vts x 31.8 us.  The period is rounded UP to a whole
	 * sensor frame that fits the producer's work -- since issue #59 the
	 * capture overlaps that work, so the active frame time (15.1 ms) is no
	 * longer added, but the ceil survives: lowering vts below the work
	 * still changes nothing, and tuning it just above the work is the
	 * cliff both #38 and #71 measured.
	 *
	 * 31.8 and not the 31.507 this used to print: that figure is HTS 1852
	 * over a 58.8 MHz PCLK, and every N=1 run measures about 0.9% higher --
	 * 450 us at these frame lengths, the same order as the margin somebody
	 * reading this line would be tuning against. */
	cli_print(sh, "           (period = vts x 31.8 us, but ROUNDED UP to a "
	              "whole one that fits\r\n"
	              "            the producer's work -- so lowering vts often "
	              "changes nothing.\r\n"
	              "            `camera bench` measures it.  Lower also caps "
	              "the exposure.)\r\n");
	cam_note_queued(sh, argc);
	return 0;
}

int camera_board_cmd_wb(struct cli_instance *sh, int argc, char **argv)
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

int camera_board_cmd_auto(struct cli_instance *sh, int argc, char **argv)
{
	uint16_t lines, dgain;
	uint8_t again;
	struct cam_wb wb;
	int rc;

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
		rc = camera_set_auto(on);
		if (rc != CAM_OK) {
			if (rc == CAM_ERR_BUSY)
				cam_note_api_busy(sh, "auto");
			else
				cli_error(sh, "camera: could not set the "
				              "sensor's own exposure loop (is "
				              "the module powered?  try "
				              "`camera probe`)\r\n");
			return 1;
		}
	}

	cam_sensor_get_exposure_gains(&lines, &again, &dgain);
	camera_get_wb(&wb);

	cli_print(sh, "auto     : %s  (exposure by the sensor's own AEC, "
	              "wb here)\r\n", camera_auto() ? "on" : "off");
	if (cam_sensor_exposure_gains_valid())
		cli_print(sh, "  now    : exposure %lu  again %u  "
		              "wb r %u b %u\r\n",
		          (unsigned long)lines, again, wb.r, wb.b);
	else
		/* The white balance is this port's own and is always known; only
		 * the sensor's two halves of this line are unread. */
		cli_print(sh, "  now    : exposure --  again --  "
		              "wb r %u b %u  (sensor not read back yet)\r\n",
		          wb.r, wb.b);
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
int camera_board_cmd_black(struct cli_instance *sh, int argc, char **argv)
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

int camera_board_cmd_sat(struct cli_instance *sh, int argc, char **argv)
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

int camera_board_cmd_gamma(struct cli_instance *sh, int argc, char **argv)
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

int camera_board_cmd_bayer(struct cli_instance *sh, int argc, char **argv)
{
	static const char *const names[4] = { "bggr", "gbrg", "grbg", "rggb" };
	uint8_t i;
	int rc;

	if (argc > 1) {
		for (i = 0u; i < 4u; i++)
			if (strcmp(argv[1], names[i]) == 0)
				break;
		if (i == 4u) {
			cli_error(sh, "camera: bayer must be one of "
			              "bggr gbrg grbg rggb\r\n");
			return 1;
		}
		/*
		 * Through the camera API, not cam_dp_set_bayer() (issue #80).
		 * This command used to write the phase straight into the
		 * datapath from here, and the producer picks that value up on
		 * its own restart -- so the line printed below was a promise
		 * this command could not keep while a stream was running.
		 */
		rc = camera_set_bayer(i);
		if (rc != CAM_OK) {
			if (rc == CAM_ERR_BUSY)
				cli_error(sh, "camera: bayer refused: the "
				              "camera is streaming, or another "
				              "camera\r\n"
				              "        command holds the API.  "
				              "The phase is read when the "
				              "datapath is\r\n"
				              "        configured, so stop the "
				              "stream and set it before "
				              "starting\r\n");
			else
				cam_report(sh, "bayer", rc);
			return 1;
		}
	}

	cli_print(sh, "bayer    : %s\r\n",
	          cam_dp_bayer_name(cam_dp_bayer()));
	if (argc > 1)
		cli_print(sh, "           (takes effect at the next `camera "
		              "capture` or `camera stream start`)\r\n");
	return 0;
}
