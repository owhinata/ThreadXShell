/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Host test: the shared `camera` command (shell/cmds/cmd_camera.c, issue #143)
 * through the REAL dispatcher, over a stub adapter (camera_svc_stub.c), once
 * per capability shape (camera_cfg/camera_svc_config.h).
 *
 * What is pinned:
 *  - the table: shared order, the board's subcommands last, no name twice,
 *    and a subcommand present exactly when its capability is 1 (`help
 *    camera` walks this table in this order; cmd_builtin.c, which prints it,
 *    is firmware-only);
 *  - the usage lines and refusal sentences each shape prints -- a board
 *    without a test pattern still recognises `test` and says why;
 *  - the shared lines around a board's report: the IN_USE list, the
 *    stop-requested / stopped words, the state + holders lines before the
 *    board's stats, the first-bytes dump after its capture lines;
 *  - that a refusal before the adapter reaches nothing, and that every
 *    successful frame open is closed (send, save, a failed read).
 */
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cli_instance.h"
#include "cli_internal.h"
#include "cli_backend_dummy.h"
#include "host_glue.h"
#include "camera_svc.h"
#include "camera_svc_config.h"
#include "camera_svc_stub.h"
#include "camera_table_check.h"

#if defined(CAM_TEST_SHAPE_FULL)
#define SHAPE "full (power, test, save sd|fs, send; board: scan)"
#elif defined(CAM_TEST_SHAPE_BARE)
#define SHAPE "bare (no power / test / save; board: raw, bench)"
#elif defined(CAM_TEST_SHAPE_MIN)
#define SHAPE "min (power, test; no info / save / send; board: scan)"
#else
#define SHAPE "dup (full, and the board also registers `info`)"
#endif

/* The board's subcommand handler (CAMERA_SVC_BOARD_DECLS). */
static int board_calls;
int test_board_cmd(struct cli_instance *sh, int argc, char **argv)
{
	(void)sh; (void)argc; (void)argv;
	board_calls++;
	return 0;
}

/* ---- harness (the same shape as test_nn_cmd_usage.c) --------------------- */

CLI_BACKEND_DUMMY_DEFINE(tr0);
static struct cli_instance sh0;

static void reset(void)
{
	memset(&sh0, 0, sizeof sh0);
	sh0.tr = &tr0;
	tr0.sh = &sh0;
	strcpy(sh0.prompt, "> ");
	cli_dummy_clear_output(&tr0);
	cli_dummy_clear_rx(&tr0);
	cli_dummy_reset_stats(&tr0);
	cli_dummy_set_tx_fail(&tr0, 0);
	cli_dummy_set_tx_cap(&tr0, 0);
	cli_test_set_tx_wait_hook(NULL, NULL);
	cli_test_set_sleep_hook(NULL, NULL);
	camera_stub_reset();
	board_calls = 0;
}

/* Run @p line with whatever the stub was set to after reset(). */
static void run(const char *line)
{
	cli_dummy_clear_output(&tr0);
	cli_dummy_inject(&tr0, line, strlen(line));
	cli_test_pump(&sh0);
}

static int has(const char *needle)
{
	return strstr(cli_dummy_output_str(&tr0), needle) != NULL;
}

/* @p a appears, and before @p b. */
static int before(const char *a, const char *b)
{
	const char *out = cli_dummy_output_str(&tr0);
	const char *pa = strstr(out, a), *pb = strstr(out, b);

	return pa != NULL && pb != NULL && pa < pb;
}

#if defined(CAM_TEST_SHAPE_FULL)
/* A Ctrl+C arriving while a command sleeps. */
static void inject_ctrl_c(struct cli_instance *sh, void *arg)
{
	static const char etx = 0x03;

	(void)sh;
	(void)arg;
	cli_dummy_inject(&tr0, &etx, 1);
}
#endif

static int failures;

static void expect(const char *what, int ok)
{
	printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
	if (!ok) {
		printf("       output was: \"%s\"\n", cli_dummy_output_str(&tr0));
		failures++;
	}
}

/* ---- the table ------------------------------------------------------------ */

static void check_table(void)
{
	const struct cli_cmd *t = camera_table();

	expect("the `camera` root is registered", t != NULL);
	if (t == NULL)
		return;
#if defined(CAM_TEST_SHAPE_DUP)
	/* The check must SEE a shadowing board entry, or a clean answer from
	   the real boards means nothing. */
	expect("a board entry reusing a shared name is caught",
	       camera_table_dups(t) == 1);
	return;
#else
	expect("no subcommand name appears twice", camera_table_dups(t) == 0);
#endif
	expect("probe / capture / stream are always there",
	       camera_table_has(t, "probe") && camera_table_has(t, "capture") &&
	       camera_table_has(t, "stream"));
	expect("on / off exactly with CAMERA_SVC_HAS_POWER",
	       camera_table_has(t, "on") == CAMERA_SVC_HAS_POWER &&
	       camera_table_has(t, "off") == CAMERA_SVC_HAS_POWER);
	expect("info exactly with CAMERA_SVC_HAS_INFO",
	       camera_table_has(t, "info") == CAMERA_SVC_HAS_INFO);
	expect("save exactly with CAMERA_SVC_HAS_SAVE",
	       camera_table_has(t, "save") == CAMERA_SVC_HAS_SAVE);
	expect("send exactly with CAMERA_SVC_HAS_SEND",
	       camera_table_has(t, "send") == CAMERA_SVC_HAS_SEND);
#if defined(CAM_TEST_SHAPE_FULL)
	expect("the shared order, then the board's",
	       camera_table_index(t, "probe") == 0 &&
	       camera_table_index(t, "on") == 1 &&
	       camera_table_index(t, "off") == 2 &&
	       camera_table_index(t, "info") == 3 &&
	       camera_table_index(t, "capture") == 4 &&
	       camera_table_index(t, "save") == 5 &&
	       camera_table_index(t, "send") == 6 &&
	       camera_table_index(t, "stream") == 7 &&
	       camera_table_index(t, "scan") == 8);
#elif defined(CAM_TEST_SHAPE_MIN)
	expect("the shared order, then the board's",
	       camera_table_index(t, "probe") == 0 &&
	       camera_table_index(t, "on") == 1 &&
	       camera_table_index(t, "off") == 2 &&
	       camera_table_index(t, "capture") == 3 &&
	       camera_table_index(t, "stream") == 4 &&
	       camera_table_index(t, "scan") == 5);
#else
	expect("the shared order, then the board's",
	       camera_table_index(t, "probe") == 0 &&
	       camera_table_index(t, "info") == 1 &&
	       camera_table_index(t, "capture") == 2 &&
	       camera_table_index(t, "send") == 3 &&
	       camera_table_index(t, "stream") == 4 &&
	       camera_table_index(t, "raw") == 5 &&
	       camera_table_index(t, "bench") == 6);
#endif
}

/* ---- the lines ------------------------------------------------------------ */

/* ---- capture: the shared lines (issue #237) ------------------------------- */

/* A report with every shared line: three channels, a three-colour seam, the
   board's line, a warm-up and a head. */
static void capture_full_report(void)
{
	struct camera_svc_capture_report *r = &camera_stub.report;

	r->format = "rgb565";
	r->width = 320u;
	r->height = 240u;
	r->bytes = 153600u;
	r->nchan = 3u;
	r->chan[0] = (struct camera_svc_chan_stat){ "R", 1204u, 0u, 31u, 5u };
	r->chan[1] = (struct camera_svc_chan_stat){ "G", 2999u, 3u, 63u, 6u };
	r->chan[2] = (struct camera_svc_chan_stat){ "B", 5u, 1u, 31u, 5u };
	r->nseam = 3u;
	r->seam_row = 7u;
	r->seam[0] = (struct camera_svc_seam_step){ "R", 30u };
	r->seam[1] = (struct camera_svc_seam_step){ "G", 1257u };
	r->seam[2] = (struct camera_svc_seam_step){ "B", 3u };
	camera_stub.warmup = 15u;
	camera_stub.warmup_unit = CAMERA_SVC_WARMUP_FRAMES;
	camera_stub.head_len = 4u;
}

/* Where @p needle starts in the output of `camera capture` run as set up. */
static size_t capture_offset_of(const char *needle)
{
	const char *out, *at;

	run("camera capture\r");
	out = cli_dummy_output_str(&tr0);
	at = strstr(out, needle);
	return at ? (size_t)(at - out) : 0u;
}

/* Run `camera capture` with the console refusing from @p needle on: the
   bytes before it go out, that line is refused. */
static void capture_refused_at_seam(const char *needle, int seam)
{
	size_t off;

	reset();
	capture_full_report();
	if (!seam)
		camera_stub.report.nseam = 0u;
	off = capture_offset_of(needle);
	expect("the refusal point is in the output", off != 0u);
	reset();
	capture_full_report();
	if (!seam)
		camera_stub.report.nseam = 0u;
	cli_dummy_clear_output(&tr0);
	cli_dummy_set_tx_cap(&tr0, off);
	run("camera capture\r");
	cli_dummy_set_tx_cap(&tr0, 0);
}

static void capture_refused_at(const char *needle)
{
	capture_refused_at_seam(needle, 1);
}

static void check_capture(void)
{
	/* every line, in the shared order and shape */
	reset();
	capture_full_report();
	run("camera capture\r");
	expect("capture: the frame line",
	       has("frame    : 320x240 rgb565 (153600 bytes)\r\n"));
	expect("capture: a channel line, its heading padded to 8, two decimals",
	       has("R(5)     : min   0  max  31  mean  12.04\r\n") &&
	       has("G(6)     : min   3  max  63  mean  29.99\r\n"));
	expect("capture: a mean under one keeps its leading zero",
	       has("B(5)     : min   1  max  31  mean   0.05\r\n"));
	expect("capture: the seam line names three colours",
	       has("seam     : row   7  dR 0.30  dG 12.57  dB 0.03\r\n"));
	expect("capture: announcement, frame, channels, seam, board, warm-up, dump",
	       before("stub: capturing live", "frame    :") &&
	       before("frame    :", "R(5)") && before("R(5)", "G(6)") &&
	       before("G(6)", "B(5)") && before("B(5)", "seam     :") &&
	       before("seam     :", "stub: board lines") &&
	       before("stub: board lines", "warm-up  : 15 frames dropped") &&
	       before("warm-up  :", "bytes[0..3]:"));
	expect("capture: the report and the board's lines are each asked once",
	       camera_stub.calls_report == 1 && camera_stub.calls_board_lines == 1);

	/* one channel, two-digit bits, the widest values, a one-colour seam */
	reset();
	camera_stub.report.nchan = 1u;
	camera_stub.report.chan[0] =
		(struct camera_svc_chan_stat){ "Y", 25500u, 255u, 255u, 8u };
	camera_stub.report.nseam = 1u;
	camera_stub.report.seam_row = 120u;
	camera_stub.report.seam[0] = (struct camera_svc_seam_step){ "G", 85u };
	run("camera capture\r");
	expect("capture: one channel at its largest",
	       has("Y(8)     : min 255  max 255  mean 255.00\r\n") &&
	       !has("R(") && !has("(5)"));
	expect("capture: a one-colour seam",
	       has("seam     : row 120  dG 0.85\r\n") && !has("dR") && !has("dB"));
	reset();
	camera_stub.report.nchan = 1u;
	camera_stub.report.chan[0] =
		(struct camera_svc_chan_stat){ "Cb", 12799u, 100u, 160u, 8u };
	run("camera capture\r");
	expect("capture: a two-letter heading, the mean truncated",
	       has("Cb(8)    : min 100  max 160  mean 127.99\r\n"));
	reset();
	camera_stub.report.nchan = 1u;
	camera_stub.report.chan[0] =
		(struct camera_svc_chan_stat){ "Cr", 0u, 0u, 1023u, 10u };
	run("camera capture\r");
	expect("capture: a two-digit bit count keeps the padding to 8",
	       has("Cr(10)   : min   0  max 1023  mean   0.00\r\n"));

	/* no channel and no seam: the frame line, then the board's */
	reset();
	run("camera capture\r");
	expect("capture: no channel lines and no seam line",
	       has("frame    : 4x2 stubfmt (16 bytes)\r\n") && !has(" : min ") &&
	       !has("seam") && before("frame    :", "stub: board lines"));

	/* a failed report prints the failure alone, numbers or no numbers */
	reset();
	capture_full_report();
	camera_stub.report_fills_then_fails = 1;
	run("camera capture\r");
	expect("capture: a report that filled its numbers and then failed "
	       "prints only the failure",
	       has("stub: capturing live\r\n") &&
	       has("camera: stub: frame changed mid-report\r\n") &&
	       !has("frame    :") && !has("R(5)") && !has("seam") &&
	       !has("stub: board lines") && !has("warm-up") && !has("bytes["));
	expect("capture: and does not ask for the board's lines",
	       camera_stub.calls_report == 1 && camera_stub.calls_board_lines == 0);
	expect("capture: the failure comes after the announcement",
	       before("stub: capturing live", "camera: stub: frame changed"));

	/* cancelled in the capture, or in the report: the announcement only */
	reset();
	capture_full_report();
	camera_stub.capture_cancels = 1;
	cli_dummy_inject(&tr0, "camera capture\r\x03", 16);
	cli_test_pump(&sh0);
	expect("capture: a cancel in the capture prints the announcement only",
	       has("stub: capturing live\r\n") && !has("frame    :") &&
	       camera_stub.calls_report == 0 && camera_stub.calls_board_lines == 0);
	reset();
	capture_full_report();
	cli_dummy_inject(&tr0, "camera capture\r\x03", 16);
	cli_test_pump(&sh0);
	expect("capture: a cancel in the report prints the announcement only",
	       has("stub: capturing live\r\n") && !has("frame    :") &&
	       !has("camera: ") && camera_stub.calls_report == 1 &&
	       camera_stub.calls_board_lines == 0);

	/* [!] a refused line ends the output: nothing after it is asked for */
	capture_refused_at("frame    :");
	expect("capture: refused at the frame line, nothing after it",
	       has("stub: capturing live") && !has("frame    :") &&
	       !has("R(5)") && camera_stub.calls_board_lines == 0);
	capture_refused_at("G(6)");
	expect("capture: refused at a channel line, nothing after it",
	       has("R(5)     :") && !has("G(6)") && !has("seam") &&
	       camera_stub.calls_board_lines == 0);
	/* the last channel line refused, with no seam line after it: the
	   board's lines are not asked for (the console would drop them anyway,
	   so only the call shows it) */
	capture_refused_at_seam("B(5)", 0);
	expect("capture: refused at the last channel line, the board's lines "
	       "not asked",
	       has("G(6)     :") && !has("B(5)") &&
	       camera_stub.calls_board_lines == 0);
	capture_refused_at("seam     :");
	expect("capture: refused at the seam line, the board's lines not asked",
	       has("B(5)     :") && !has("seam") &&
	       camera_stub.calls_board_lines == 0);
	capture_refused_at("stub: board lines");
	expect("capture: refused at the board's line, no warm-up and no dump",
	       has("seam     :") && camera_stub.calls_board_lines == 1 &&
	       !has("stub: board lines") && !has("warm-up") && !has("bytes["));
	capture_refused_at("warm-up  :");
	expect("capture: refused at the warm-up, no dump",
	       has("stub: board lines") && !has("warm-up") && !has("bytes["));
	capture_refused_at("bytes[");
	expect("capture: refused at the dump's heading, no dump",
	       has("warm-up  : 15 frames dropped") && !has("bytes[") &&
	       !has("000000"));

	/* a board that says its own writer refused: nothing after it either,
	   though the console would still take it */
	reset();
	capture_full_report();
	camera_stub.board_refuses = 1;
	run("camera capture\r");
	expect("capture: the board's refusal ends the output too",
	       has("stub: board lines") && !has("warm-up") && !has("bytes["));
}

static void check_common(void)
{
	/* an IN_USE stop names the holders and changes nothing */
	reset();
	camera_stub.status = CAMERA_SVC_ERR_IN_USE;
	camera_stub.users[0] = "nn";
	camera_stub.users[1] = "mjpeg";
	camera_stub.nusers = 2;
	run("camera stream stop\r");
	expect("a refused stop names who holds the stream",
	       has("camera: still in use: nn, mjpeg\r\n") &&
	       !has("stop requested"));

	reset();
	run("camera stream stop\r");
	expect("an asynchronous stop says it was requested",
	       has("camera: stream stop requested\r\n"));
	reset();
	camera_stub.stop_what = CAMERA_SVC_STOP_DONE;
	run("camera stream stop\r");
	expect("a confirmed stop says stopped", has("camera: stream stopped\r\n"));

	/* a failure without the board's words falls back to the status */
	reset();
	camera_stub.status = CAMERA_SVC_ERR_BUSY;
	run("camera probe\r");
	expect("a failure with no detail prints the status's words",
	       has("camera: busy -- try again\r\n"));
	reset();
	camera_stub.status = CAMERA_SVC_ERR_HW;
	strcpy(camera_stub.detail, "the board's own words");
	run("camera probe\r");
	expect("and the board's words when it gave them",
	       has("camera: the board's own words\r\n") && !has("stub: probed"));

	/* capture: the board's lines, then the first bytes */
	reset();
	camera_stub.head_len = 4;
	run("camera capture\r");
	expect("capture lets the board announce a live frame",
	       has("stub: capturing live\r\n") && camera_stub.last_test == 0);
	expect("then the board's lines, then the dump of the head",
	       before("stub: board lines", "bytes[0..3]:\r\n"));
	reset();
	camera_stub.head_len = 0;
	run("camera capture\r");
	expect("no head, no dump", has("stub: board lines") && !has("bytes["));

	/* the warm-up the capture paid: none, frames, or time -- one shape */
	reset();
	run("camera capture\r");
	expect("a board that counts no warm-up prints no warm-up line",
	       !has("warm-up"));
	reset();
	camera_stub.warmup = 15;
	camera_stub.warmup_unit = CAMERA_SVC_WARMUP_FRAMES;
	camera_stub.head_len = 4;
	run("camera capture\r");
	expect("frames let go are printed after the board's lines, before the dump",
	       has("warm-up  : 15 frames dropped\r\n") &&
	       before("stub: board lines", "warm-up  :") &&
	       before("warm-up  :", "bytes[0..3]:"));
	reset();
	camera_stub.warmup = 1;
	camera_stub.warmup_unit = CAMERA_SVC_WARMUP_FRAMES;
	run("camera capture\r");
	expect("one frame is one frame", has("warm-up  : 1 frame dropped\r\n"));
	reset();
	camera_stub.warmup = 0;
	camera_stub.warmup_unit = CAMERA_SVC_WARMUP_FRAMES;
	run("camera capture\r");
	expect("zero is printed: the settled answer", has("warm-up  : 0 frames dropped\r\n"));
	reset();
	camera_stub.warmup = 612;
	camera_stub.warmup_unit = CAMERA_SVC_WARMUP_MS;
	run("camera capture\r");
	expect("a settle time is printed in ms", has("warm-up  : 612 ms\r\n"));
	reset();
	camera_stub.warmup = 15;
	camera_stub.warmup_unit = CAMERA_SVC_WARMUP_FRAMES;
	camera_stub.capture_cancels = 1;
	cli_dummy_inject(&tr0, "camera capture\r\x03", 16);
	cli_test_pump(&sh0);
	expect("a Ctrl+C between warm-up frames ends the capture silently",
	       camera_stub.calls_capture == 1 && !has("stub: board lines") &&
	       !has("warm-up") && !has("camera: cancel"));

	/* stats: state and holders first, the board's lines after */
	reset();
	camera_stub.streaming = 1;
	camera_stub.holders[0] = "manual";
	camera_stub.holders[1] = "nn";
	camera_stub.nholders = 2;
	run("camera stream stats\r");
	expect("stats: the state line", has("state    : streaming\r\n"));
	expect("then the holders line in the old spelling",
	       has("holders  : manual, nn\r\n"));
	expect("then the board's lines",
	       before("holders  :", "stub: board stats"));
	reset();
	camera_stub.stop_reason = "overrun";
	camera_stub.lost = 1;
	run("camera stream stats\r");
	expect("a stopped stream gives the board's reason",
	       has("state    : stopped (overrun)\r\n"));
	expect("and nobody holding it, with the lost count",
	       has("holders  : none  (1 lost when the stream ended)\r\n"));

	/* [!] stats: the state and the numbers come from ONE sample */
	reset();
	camera_stub.tag_samples = 1;
	run("camera stream stats\r");
	expect("stats takes one sample and no separate state",
	       camera_stub.samples == 1 && camera_stub.calls_state == 0);
	expect("and the state line and the board's line name the same sample",
	       has("state    : stopped (sample-1)\r\n") &&
	       has("stub: board stats sample-1\r\n") &&
	       before("holders  :", "stub: board stats"));

	/* stats while a start or stop is in flight: no holder list to give */
	reset();
	camera_stub.transition = 1;
	run("camera stream stats\r");
	expect("stats mid-transition says so instead of naming holders",
	       has("holders  : -- (a stream start or stop is in progress)\r\n") &&
	       !has("holders  : none"));
	reset();
	camera_stub.transition = 1;
	camera_stub.lost = 2;
	run("camera stream stats\r");
	expect("and still gives the lost count, as the board heads do",
	       has("holders  : -- (a stream start or stop is in progress)  "
	           "(2 lost when the stream ended)\r\n"));

	/* stats before any stream: the one line, no holders, no board lines */
	reset();
	camera_stub.never_started = 1;
	run("camera stream stats\r");
	expect("stats with no stream since boot says only that",
	       has("state    : never started\r\n") && !has("holders  :") &&
	       !has("stub: board stats"));

	/* stats when the board cannot sample the stream: its error alone */
	reset();
	camera_stub.state_status = CAMERA_SVC_ERR_STATE;
	run("camera stream stats\r");
	expect("stats the board cannot sample prints only the error",
	       has("camera: stub: state unavailable\r\n") && !has("state    :") &&
	       !has("holders  :") && !has("stub: board stats"));

	/* capture cancelled during the board's walk: nothing after the banner */
	reset();
	camera_stub.head_len = 4;
	cli_dummy_inject(&tr0, "camera capture\r\x03", 16);
	cli_test_pump(&sh0);
	expect("a Ctrl+C during the walk is asked about and ends it silently",
	       camera_stub.cancel_asked >= 1 && !has("stub: board lines") &&
	       !has("bytes[") && !has("camera: cancel"));

	/* IN_USE with nobody left to name keeps the old spelling */
	reset();
	camera_stub.status = CAMERA_SVC_ERR_IN_USE;
	camera_stub.nusers = 0;
	run("camera stream stop\r");
	expect("an IN_USE with no names still prints the list's colon",
	       has("camera: still in use: \r\n"));

#if CAMERA_SVC_HAS_SEND
	/* send: opens, hands the source over, closes */
	reset();
	run("camera send\r");
	expect("send names the file frame.raw by default and sizes it",
	       camera_stub.calls_send == 1 && camera_stub.send_size == 8u &&
	       strcmp(camera_stub.send_name, "frame.raw") == 0);
	expect("and closes what it opened", camera_stub.opened == 0);
	reset();
	camera_stub.read_fail_at = 2;
	run("camera send\r");
	expect("a read that fails mid-transfer says why, in the board's words",
	       has("camera: stub: frame changed\r\n") && camera_stub.opened == 0);
	reset();
	run("camera send\r");
	expect("and a transfer whose reads all succeed says nothing more",
	       !has("camera: ") && camera_stub.calls_read == 2);
	reset();
	camera_stub.status = CAMERA_SVC_ERR_NO_FRAME;
	strcpy(camera_stub.detail, "no captured frame");
	run("camera send pic.raw\r");
	expect("send with no frame refuses before the transfer",
	       has("camera: no captured frame\r\n") && camera_stub.calls_send == 0 &&
	       camera_stub.opened == 0);
#else
	reset();
	run("camera send\r");
	expect("`camera send` does not exist here",
	       camera_stub.calls_open == 0 && has("unknown subcommand"));
#endif
#if !CAMERA_SVC_HAS_INFO
	reset();
	run("camera info\r");
	expect("`camera info` does not exist here",
	       camera_stub.calls_info == 0 && has("unknown subcommand"));
#endif

	/* the stream's usage line */
	reset();
	run("camera stream bogus\r");
#if CAMERA_SVC_HAS_TEST
	expect("an unknown stream word gets the usage with [test]",
	       has("camera: unknown stream subcommand 'bogus'\r\n") &&
	       has("usage: camera stream <start [test] | stop | stats>\r\n"));
#else
	expect("an unknown stream word gets the usage without test",
	       has("camera: unknown stream subcommand 'bogus'\r\n") &&
	       has("usage: camera stream <start | stop | stats>\r\n"));
#endif

	/* the board's own subcommands are reached */
	reset();
	run(CAMERA_SVC_HAS_POWER ? "camera scan\r" : "camera bench\r");
	expect("a board subcommand reaches the board's handler", board_calls == 1);
}

#if defined(CAM_TEST_SHAPE_FULL)
static void check_full(void)
{
	reset();
	run("camera capture bogus\r");
	expect("capture: an unknown word names `test`",
	       has("camera: unknown option 'bogus' (try: test)\r\n") &&
	       camera_stub.calls_capture == 0);
	reset();
	run("camera capture test\r");
	expect("capture test asks for the colorbar",
	       has("stub: capturing test\r\n") && camera_stub.last_test == 1);
	reset();
	run("camera stream start bogus\r");
	expect("stream start: an unknown word",
	       has("camera: bad option 'bogus' (test)\r\n") &&
	       camera_stub.calls_start == 0);
	reset();
	run("camera stream start test\r");
	expect("stream start test: the board says the outcome, alone",
	       has("stub: streaming\r\n") && camera_stub.last_test == 1 &&
	       !has("camera: streaming"));
	reset();
	camera_stub.already = 1;
	run("camera stream start\r");
	expect("an already running stream: the board's words",
	       has("stub: already streaming\r\n"));

	/* save */
	reset();
	run("camera save\r");
	expect("save's usage spells the board's media, the medium optional",
	       has("usage: camera save [sd|fs] <path>\r\n"));
	reset();
	run("camera save usb /a.raw\r");
	expect("an unknown medium lists the board's",
	       has("camera: unknown medium 'usb' (sd or fs)\r\n") &&
	       camera_stub.calls_open == 0);
	reset();
	run("camera save fs /a.raw\r");
	expect("a save writes the whole frame and names the medium",
	       camera_stub.written == 8u &&
	       has("wrote 8 bytes (2x2 rgb565) to fs:/a.raw\r\n"));
	expect("with the PC-side conversion hint",
	       has("PC: python3 scripts/rgb565_to_png.py --width 2 --height 2 "
	           "/a.raw out.png\r\n"));
	expect("inside the medium's op slot, the frame closed",
	       camera_stub.calls_op_begin == 1 && camera_stub.calls_op_end == 1 &&
	       camera_stub.opened == 0);
	reset();
	camera_stub.read_fail = 1;
	run("camera save sd /a.raw\r");
	expect("a frame gone by the time the medium is mounted: no file touched",
	       has("camera: stub: frame changed\r\n") && !has("partial") &&
	       camera_stub.created_file == 0 && camera_stub.opened == 0 &&
	       camera_stub.calls_op_end == 1);
	reset();
	camera_stub.read_fail_at = 2;   /* 1 = the check after the mount */
	run("camera save sd /a.raw\r");
	expect("a read that finds the frame changed leaves the file partial",
	       has("camera: stub: frame changed (file left partial)\r\n") &&
	       camera_stub.closed_file == 1 && camera_stub.opened == 0 &&
	       camera_stub.calls_op_end == 1);
	expect("and the partial file is closed and the medium flushed",
	       camera_stub.closed_file == 1 && camera_stub.flushed == 1);
	reset();
	camera_stub.status = CAMERA_SVC_ERR_NO_FRAME;
	strcpy(camera_stub.detail, "no captured frame");
	run("camera save sd /a.raw\r");
	expect("no frame: refused before the medium is touched",
	       has("camera: no captured frame\r\n") &&
	       camera_stub.calls_op_begin == 0);

	/* off */
	reset();
	run("camera off\r");
	expect("off with nothing streaming goes straight to the board",
	       camera_stub.calls_stop == 0 && camera_stub.calls_off == 1 &&
	       has("stub: off\r\n"));
	reset();
	camera_stub.streaming = 1;
	run("camera off\r");
	expect("off while streaming stops the stream first and waits on its "
	       "generation",
	       camera_stub.calls_stop == 1 && camera_stub.ended_gen == 42u &&
	       has("camera: stopping stream before power off\r\n") &&
	       camera_stub.calls_off == 1);
	reset();
	camera_stub.streaming = 1;
	camera_stub.status = CAMERA_SVC_ERR_IN_USE;
	camera_stub.users[0] = "nn";
	camera_stub.nusers = 1;
	run("camera off\r");
	expect("off refused by another holder changes nothing and says who",
	       has("camera: still in use: nn\r\n") && !has("stopping") &&
	       camera_stub.calls_off == 0);

	/* [!] the wait is on THE STOPPED STREAM's generation: another console's
	   stream running after it is not a reason to wait, and its end is */
	reset();
	camera_stub.streaming = 1;
	camera_stub.another_starts = 1;
	run("camera off\r");
	expect("off waits for its own stream's end, not for nothing streaming",
	       camera_stub.calls_off == 1 && !has("not stopped yet"));
	reset();
	camera_stub.streaming = 1;
	camera_stub.stop_never_ends = 1;
	camera_stub.another_starts = 1;
	run("camera off\r");
	expect("a stopped stream that never ends times out, power left on",
	       has("camera: the stream has not stopped yet -- timed out (it may "
	           "still finish); power left on, retry 'camera off'\r\n") &&
	       camera_stub.calls_off == 0 && camera_stub.ended_calls > 1);

	/* Ctrl+C during the wait: the wait ends, nothing more happens */
	reset();
	camera_stub.streaming = 1;
	camera_stub.stop_never_ends = 1;
	camera_stub.another_starts = 1;
	cli_test_set_sleep_hook(inject_ctrl_c, NULL);
	run("camera off\r");
	expect("a cancelled wait leaves the power on and says nothing more",
	       camera_stub.calls_off == 0 && has("stopping stream") &&
	       !has("not stopped yet"));
	cli_test_set_sleep_hook(NULL, NULL);

	/* a stop that failed for any other reason: no power-off */
	reset();
	camera_stub.streaming = 1;
	camera_stub.status = CAMERA_SVC_ERR_BUSY;
	strcpy(camera_stub.detail, "stub: a stop is in flight");
	run("camera off\r");
	expect("a failed stop is printed and the power stays on",
	       has("camera: stub: a stop is in flight\r\n") &&
	       !has("stopping") && camera_stub.calls_off == 0);
	reset();
	camera_stub.streaming = 1;
	camera_stub.status = CAMERA_SVC_ERR_TIMEOUT;
	strcpy(camera_stub.detail, "timeout");
	run("camera off\r");
	expect("a stop that waited and ran out is the same timeout",
	       has("the stream has not stopped yet -- timed out") &&
	       camera_stub.calls_off == 0);

	/* a stop that already waited (DONE): no wait of our own */
	reset();
	camera_stub.streaming = 1;
	camera_stub.stop_what = CAMERA_SVC_STOP_DONE;
	run("camera off\r");
	expect("a confirmed stop goes straight on to the power-off",
	       camera_stub.calls_off == 1 && camera_stub.ended_calls == 0 &&
	       has("camera: stopping stream before power off\r\n"));
	/* ...unless a Ctrl+C came while the board's stop waited (wio) */
	reset();
	camera_stub.streaming = 1;
	camera_stub.stop_what = CAMERA_SVC_STOP_DONE;
	cli_dummy_inject(&tr0, "camera off\r\x03", 12);
	cli_test_pump(&sh0);
	expect("a Ctrl+C during a confirmed stop leaves the power on",
	       camera_stub.calls_stop == 1 && camera_stub.calls_off == 0);

	/* save with the medium left out: the board's first */
	reset();
	run("camera save /b.raw\r");
	expect("save with no medium writes to the first one",
	       has("wrote 8 bytes (2x2 rgb565) to sd:/b.raw\r\n"));
	reset();
	run("camera save fs\r");
	expect("a lone medium name is not taken for a path",
	       has("camera: 'fs' names a medium -- give the path: camera save "
	           "[sd|fs] <path>\r\n") && camera_stub.calls_open == 0);
}
#endif

#if defined(CAM_TEST_SHAPE_MIN)
static void check_min(void)
{
	/* CAMERA_SVC_OFF_STOPS_STREAM 0: a running stream is the board's to
	   refuse, so the shared command neither stops it nor announces a stop. */
	reset();
	camera_stub.streaming = 1;
	camera_stub.status = CAMERA_SVC_ERR_BUSY;
	strcpy(camera_stub.detail, "a capture or stream already owns the DCMI");
	run("camera off\r");
	expect("off while streaming goes straight to the board, which refuses",
	       camera_stub.calls_stop == 0 && camera_stub.calls_off == 1 &&
	       !has("stopping") &&
	       has("camera: a capture or stream already owns the DCMI\r\n"));
}
#endif

#if defined(CAM_TEST_SHAPE_BARE)
static void check_bare(void)
{
	reset();
	run("camera capture test\r");
	expect("capture test on a board without a pattern says why",
	       has("camera: this board has no test pattern to capture\r\n") &&
	       camera_stub.calls_capture == 0);
	reset();
	run("camera capture bogus\r");
	expect("and an unknown word does not offer `test`",
	       has("camera: unknown option 'bogus'\r\n") && !has("try: test"));
	reset();
	run("camera stream start test\r");
	expect("stream start test likewise",
	       has("camera: this board has no test pattern to stream\r\n") &&
	       camera_stub.calls_start == 0);
	reset();
	run("camera on\r");
	expect("`camera on` does not exist here",
	       camera_stub.calls_on == 0 && has("unknown subcommand"));
	reset();
	run("camera save sd /a\r");
	expect("nor does `camera save`", camera_stub.calls_open == 0 &&
	       has("unknown subcommand"));
}
#endif

int main(void)
{
	printf("test_camera_cmd (%s):\n", SHAPE);
	check_table();
#if defined(CAM_TEST_SHAPE_DUP)
	(void)check_common;   /* the duplicate's table is the whole point here */
	(void)check_capture;
#else
	check_common();
	check_capture();
#endif
#if defined(CAM_TEST_SHAPE_FULL)
	check_full();
#endif
#if defined(CAM_TEST_SHAPE_BARE)
	check_bare();
#endif
#if defined(CAM_TEST_SHAPE_MIN)
	check_min();
#endif
	if (failures != 0) {
		printf("test_camera_cmd: %d failure(s)\n", failures);
		return 1;
	}
	printf("test_camera_cmd: all passed\n");
	return 0;
}
