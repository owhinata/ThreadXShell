/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Host test: the `camera` subcommand table Grove Vision AI V2 actually builds
 * -- the shared command (shell/cmds/cmd_camera.c) against this board's REAL
 * svc/camera_svc_config.h, over the stub adapter (shell/test/camera_svc_stub.c)
 * and stub handlers for the board's own subcommands (issue #143).
 *
 *  - [!] NO NAME TWICE: CAMERA_SVC_BOARD_SUBCMDS is appended to the shared
 *    entries and the parser takes the first match, so a board name that
 *    reuses a shared one would be silently unreachable.
 *  - each shared subcommand is there exactly when its capability is 1 (info
 *    and send since issue #143's B step; never on / off / save), and the
 *    board's twelve come after the shared ones -- `stats` gone (it is
 *    `camera stream stats`), `profile` in its place.
 *  - no test pattern: `test` is refused in this board's old words.
 */
#include <stdio.h>
#include <string.h>

#include "cli_instance.h"
#include "cli_internal.h"
#include "cli_backend_dummy.h"
#include "host_glue.h"
#include "camera_svc_config.h"
#include "camera_svc_stub.h"
#include "camera_table_check.h"

/* The board's handlers, as CAMERA_SVC_BOARD_DECLS names them. */
static int board_calls;
static const char *board_last;

static int stub_board(struct cli_instance *sh, int argc, char **argv)
{
	(void)sh;
	(void)argc;
	board_calls++;
	board_last = argv[0];
	return 0;
}

int camera_board_cmd_raw(struct cli_instance *sh, int argc, char **argv)
{ return stub_board(sh, argc, argv); }
int camera_board_cmd_profile(struct cli_instance *sh, int argc, char **argv)
{ return stub_board(sh, argc, argv); }
int camera_board_cmd_vts(struct cli_instance *sh, int argc, char **argv)
{ return stub_board(sh, argc, argv); }
int camera_board_cmd_bench(struct cli_instance *sh, int argc, char **argv)
{ return stub_board(sh, argc, argv); }
int camera_board_cmd_exposure(struct cli_instance *sh, int argc, char **argv)
{ return stub_board(sh, argc, argv); }
int camera_board_cmd_gain(struct cli_instance *sh, int argc, char **argv)
{ return stub_board(sh, argc, argv); }
int camera_board_cmd_auto(struct cli_instance *sh, int argc, char **argv)
{ return stub_board(sh, argc, argv); }
int camera_board_cmd_black(struct cli_instance *sh, int argc, char **argv)
{ return stub_board(sh, argc, argv); }
int camera_board_cmd_sat(struct cli_instance *sh, int argc, char **argv)
{ return stub_board(sh, argc, argv); }
int camera_board_cmd_gamma(struct cli_instance *sh, int argc, char **argv)
{ return stub_board(sh, argc, argv); }
int camera_board_cmd_bayer(struct cli_instance *sh, int argc, char **argv)
{ return stub_board(sh, argc, argv); }
int camera_board_cmd_wb(struct cli_instance *sh, int argc, char **argv)
{ return stub_board(sh, argc, argv); }

CLI_BACKEND_DUMMY_DEFINE(tr0);
static struct cli_instance sh0;

static void run(const char *line)
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
	board_last = NULL;
	cli_dummy_inject(&tr0, line, strlen(line));
	cli_test_pump(&sh0);
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

int main(void)
{
	const struct cli_cmd *t = camera_table();
	static const char *const board[] = {
		"raw", "profile", "vts", "bench", "exposure", "gain", "auto", "black",
		"sat", "gamma", "bayer", "wb"
	};
	int last_shared;

	printf("test_camera_table (grove-vision-ai-v2):\n");
	expect("the `camera` root is registered", t != NULL);
	if (t == NULL)
		return 1;
	expect("no subcommand name appears twice", camera_table_dups(t) == 0);
	expect("on / off exactly with CAMERA_SVC_HAS_POWER",
	       camera_table_has(t, "on") == CAMERA_SVC_HAS_POWER &&
	       camera_table_has(t, "off") == CAMERA_SVC_HAS_POWER);
	expect("info exactly with CAMERA_SVC_HAS_INFO",
	       camera_table_has(t, "info") == CAMERA_SVC_HAS_INFO);
	expect("save exactly with CAMERA_SVC_HAS_SAVE",
	       camera_table_has(t, "save") == CAMERA_SVC_HAS_SAVE);
	expect("send exactly with CAMERA_SVC_HAS_SEND",
	       camera_table_has(t, "send") == CAMERA_SVC_HAS_SEND);
	last_shared = camera_table_index(t, "stream");
	for (unsigned i = 0; i < sizeof board / sizeof board[0]; i++) {
		int at = camera_table_index(t, board[i]);

		expect(board[i], at > last_shared);
		last_shared = at;
	}

	expect("the shared order: probe, info, capture, send, stream, then the "
	       "board's",
	       camera_table_index(t, "probe") == 0 &&
	       camera_table_index(t, "info") == 1 &&
	       camera_table_index(t, "capture") == 2 &&
	       camera_table_index(t, "send") == 3 &&
	       camera_table_index(t, "stream") == 4 &&
	       camera_table_index(t, "raw") == 5);
	expect("`camera stats` is gone: the counters are `camera stream stats`",
	       !camera_table_has(t, "stats"));
	run("camera profile\r");
	expect("`camera profile` reaches the board", board_calls == 1 &&
	       strcmp(board_last, "profile") == 0);
	run("camera bench 30\r");
	expect("`camera bench` reaches the board", board_calls == 1 &&
	       strcmp(board_last, "bench") == 0);
	run("camera wb 1 2 3\r");
	expect("`camera wb` takes its three values", board_calls == 1 &&
	       strcmp(board_last, "wb") == 0);
	run("camera stream start test\r");
	expect("`stream start test` is refused in this board's words",
	       strstr(cli_dummy_output_str(&tr0),
	              "camera: this board has no test pattern to stream\r\n") != NULL &&
	       camera_stub.calls_start == 0);
	run("camera capture test\r");
	expect("so is `capture test`",
	       strstr(cli_dummy_output_str(&tr0),
	              "camera: this board has no test pattern to capture\r\n") != NULL &&
	       camera_stub.calls_capture == 0);

	if (failures != 0) {
		printf("test_camera_table: %d failure(s)\n", failures);
		return 1;
	}
	printf("test_camera_table: all passed\n");
	return 0;
}
