/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Host test: `camera save sd <path>` (shell/cmds/cmd_camera.c) through the
 * REAL `sd` medium (shell/cmds/cmd_sd.c's fs_sd_device()), the REAL filesystem
 * bodies' mount (fs_cmd_core.c), the REAL mount glue and FileX media driver
 * (sd_fs_glue.c, fx_sd_driver.c), over the stub camera adapter
 * (camera_svc_stub.c, which writes the file) and sd_fake.c (the card,
 * FileX's open, the mutexes), through the REAL dispatcher (issue #233).
 *
 * Built twice: with the full card table, where the save mounts and writes,
 * and with a field of it empty (SD_FAKE_SHAPE_NOWRITE), where the save says
 * so in one line without the mount hint, creates no file, touches neither
 * FileX nor the card, gives the operation slot back and closes the frame.
 */
#include <stdio.h>
#include <string.h>

#include "cli_instance.h"
#include "cli_internal.h"
#include "cli_backend_dummy.h"
#include "host_glue.h"
#include "camera_svc.h"
#include "camera_svc_config.h"
#include "camera_svc_stub.h"
#include "sd_cmd_board.h"
#include "sd_fake.h"

/* The board's subcommand handler (CAMERA_SVC_BOARD_DECLS). */
int test_board_cmd(struct cli_instance *sh, int argc, char **argv)
{
	(void)sh; (void)argc; (void)argv;
	return 0;
}

static int failures;

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
	sd_fake_reset();
	sd_fake_put_vbr(0, 62333952u);
}

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

__attribute__((unused)) static int out_lines(void)
{
	int n = 0;
	const char *p = cli_dummy_output_str(&tr0);

	while ((p = strstr(p, "\r\n")) != NULL) {
		n++;
		p += 2;
	}
	return n;
}

static void expect(const char *what, int ok)
{
	printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
	if (!ok) {
		printf("       output was: \"%s\"\n", cli_dummy_output_str(&tr0));
		printf("       calls were: \"%s\"\n", sd_fake_calls());
		failures++;
	}
}

int main(void)
{
	setvbuf(stdout, NULL, _IONBF, 0);
#if defined(SD_FAKE_SHAPE_NOWRITE)
	printf("test_camera_save_sd (a field of the card table empty)\n");
	reset();
	run("camera save sd /a.raw\r");
	expect("one line, no mount hint",
	       has("sd: mount failed: card driver table has an empty field "
	           "(0xf2)\r\n") &&
	       !has("no FAT filesystem?") && out_lines() == 2 /* echo + it */);
	expect("no file created, nothing written, FileX and the card untouched",
	       camera_stub.created_file == 0 && camera_stub.written == 0u &&
	       sd_fake_calls()[0] == '\0');
	expect("the operation slot given back, the frame closed",
	       sd_fs_board_state.active_ops == 0 && !sd_fake.lock_held &&
	       sd_fake.lock_gets == sd_fake.lock_puts && camera_stub.opened == 0);
#else
	printf("test_camera_save_sd (full card table)\n");
	reset();
	run("camera save sd /a.raw\r");
	expect("mounted on the way and written",
	       has("wrote 8 bytes (2x2 rgb565) to sd:/a.raw\r\n") &&
	       sd_fake_count("media_open") == 1 && camera_stub.created_file == 1 &&
	       camera_stub.written == 8u);
	expect("the operation slot given back, the frame closed",
	       sd_fs_board_state.active_ops == 0 && !sd_fake.lock_held &&
	       camera_stub.opened == 0);
	reset();
	sd_fake.present = 0;
	run("camera save sd /a.raw\r");
	expect("an empty slot: no card, one line, no file",
	       has("sd: mount failed: no card (0xf1)\r\n") &&
	       !has("no FAT filesystem?") && camera_stub.created_file == 0);
#endif
	if (failures) {
		printf("test_camera_save_sd: %d FAILED\n", failures);
		return 1;
	}
	printf("test_camera_save_sd: all passed\n");
	return 0;
}
