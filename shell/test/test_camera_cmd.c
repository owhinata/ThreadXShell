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
	       before("stub: frame stats", "bytes[0..3]:\r\n"));
	reset();
	camera_stub.head_len = 0;
	run("camera capture\r");
	expect("no head, no dump", has("stub: frame stats") && !has("bytes["));

	/* stats: state and holders first, the board's lines after */
	reset();
	camera_stub.streaming = 1;
	camera_stub.holders[0] = "manual";
	camera_stub.holders[1] = "nn";
	camera_stub.nholders = 2;
	run("camera stream stats\r");
	expect("stats: the state line", has("state:     streaming\r\n"));
	expect("then the holders line in the old spelling",
	       has("holders:   manual, nn\r\n"));
	expect("then the board's lines",
	       before("holders:", "stub: board stats"));
	reset();
	camera_stub.stop_reason = "overrun";
	camera_stub.lost = 1;
	run("camera stream stats\r");
	expect("a stopped stream gives the board's reason",
	       has("state:     stopped (overrun)\r\n"));
	expect("and nobody holding it, with the lost count",
	       has("holders:   none  (1 lost when the stream ended)\r\n"));

	/* [!] stats: the state and the numbers come from ONE sample */
	reset();
	camera_stub.tag_samples = 1;
	run("camera stream stats\r");
	expect("stats takes one sample and no separate state",
	       camera_stub.samples == 1 && camera_stub.calls_state == 0);
	expect("and the state line and the board's line name the same sample",
	       has("state:     stopped (sample-1)\r\n") &&
	       has("stub: board stats sample-1\r\n") &&
	       before("holders:", "stub: board stats"));

	/* stats before any stream: the one line, no holders, no board lines */
	reset();
	camera_stub.never_started = 1;
	run("camera stream stats\r");
	expect("stats with no stream since boot says only that",
	       has("state:     never started\r\n") && !has("holders:") &&
	       !has("stub: board stats"));

	/* stats when the board cannot sample the stream: its error alone */
	reset();
	camera_stub.state_status = CAMERA_SVC_ERR_STATE;
	run("camera stream stats\r");
	expect("stats the board cannot sample prints only the error",
	       has("camera: stub: state unavailable\r\n") && !has("state:") &&
	       !has("holders:") && !has("stub: board stats"));

	/* capture cancelled during the board's walk: nothing after the banner */
	reset();
	camera_stub.head_len = 4;
	cli_dummy_inject(&tr0, "camera capture\r\x03", 16);
	cli_test_pump(&sh0);
	expect("a Ctrl+C during the walk is asked about and ends it silently",
	       camera_stub.cancel_asked >= 1 && !has("stub: frame stats") &&
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
	expect("save's usage spells the board's media",
	       has("usage: camera save sd|fs <path>\r\n"));
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
#else
	check_common();
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
