/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Host test: the `camera` subcommand table this board actually builds -- the
 * shared command (shell/cmds/cmd_camera.c) against this board's REAL
 * svc/camera_svc_config.h, over the stub adapter (shell/test/camera_svc_stub.c)
 * and stub handlers for the board's own subcommands (issue #143).
 *
 *  - [!] NO NAME TWICE: CAMERA_SVC_BOARD_SUBCMDS is appended to the shared
 *    entries and the parser takes the first match, so a board name that
 *    reuses a shared one would be silently unreachable.
 *  - each shared subcommand is there exactly when its capability is 1, and
 *    the board's res / format / fps / set come after the shared ones.
 *  - `set` still carries its leaf table (`camera set brightness 1` reaches
 *    the leaf through the dispatcher).
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

int camera_board_cmd_res(struct cli_instance *sh, int argc, char **argv)
{ return stub_board(sh, argc, argv); }
int camera_board_cmd_format(struct cli_instance *sh, int argc, char **argv)
{ return stub_board(sh, argc, argv); }
int camera_board_cmd_fps(struct cli_instance *sh, int argc, char **argv)
{ return stub_board(sh, argc, argv); }
int camera_board_cmd_set(struct cli_instance *sh, int argc, char **argv)
{ return stub_board(sh, argc, argv); }

const struct cli_cmd camera_board_set_subcmds[] = {
	CLI_CMD_ARG(brightness, NULL, "stub leaf", stub_board, 2, 0),
	CLI_SUBCMD_SET_END,
};

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
	static const char *const board[] = { "res", "format", "fps", "set" };
	int last_shared;

	printf("test_camera_table (f746g-disco):\n");
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

	run("camera res vga\r");
	expect("`camera res` reaches the board", board_calls == 1 &&
	       strcmp(board_last, "res") == 0);
	run("camera set brightness 1\r");
	expect("`camera set brightness` reaches the board's leaf",
	       board_calls == 1 && strcmp(board_last, "brightness") == 0);
	run("camera save usb /a.raw\r");
	expect("save refuses a medium this board does not have, in its words",
	       strstr(cli_dummy_output_str(&tr0),
	              "camera: unknown medium 'usb' (sd or fs)\r\n") != NULL);

	if (failures != 0) {
		printf("test_camera_table: %d failure(s)\n", failures);
		return 1;
	}
	printf("test_camera_table: all passed\n");
	return 0;
}
