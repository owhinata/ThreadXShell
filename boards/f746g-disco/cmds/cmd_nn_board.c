/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    cmd_nn_board.c
 * @brief   `nn stream` -- this board's background inference worker (issue #50).
 *
 * The rest of `nn` is one shared command (shell/cmds/cmd_nn.c) above a neutral
 * contract.  This subcommand is not, and issue #50 says so deliberately: the
 * lifecycle of a worker that another console can stop, whose teardown can come
 * back INCOMPLETE (issue #72), is not a formatting difference, and reconciling
 * it with the Grove board's blocking panel preview belongs in its own change.
 *
 * [!] TEMPORARY.  The destination is `nn stream start/stop/stats` on all three
 * boards over neutral start/poll/request-stop hooks -- the same grammar, one
 * implementation.  This file is the interim step, not a second permanent one.
 *
 * It lives in cmds/ rather than in the port because it needs a shell instance;
 * cmds/ is shell-layer code and may hold one, the port may not.
 */
#include "cli.h"

#include <stdint.h>
#include <string.h>

#include "camera.h"
#include "nn_camera.h"
#include "fs_cmd_core.h"   /* fs_core_read_file() */
#include "fs_devices.h"    /* fs_sd_device() */
#include "nn_cmd_core.h"

/*
 * The whole-file read behind `nn model load --path`.
 *
 * [!] IT IS HERE RATHER THAN IN THE ADAPTER because fs_core_read_file() takes a
 * shell instance and prints its own errors, and both the filesystem helper and
 * this board's device table are shell-layer things a port must not reach up for.
 * The shared command passes this function down, so the port sees a pointer and
 * names nothing above itself.
 */
int nn_board_read_file(void *ctx, const char *path, void *buf, uint32_t cap,
                       uint32_t *len)
{
	return fs_core_read_file(fs_sd_device(), (struct cli_instance *)ctx, path,
	                         buf, cap, len);
}

static enum camera_res nn_parse_res(const char *s, int *ok)
{
	*ok = 1;
	if (strcmp(s, "qqvga") == 0)
		return CAM_RES_QQVGA;
	if (strcmp(s, "qvga") == 0)
		return CAM_RES_QVGA;
	*ok = 0;
	return CAM_RES_QVGA;
}

static int cmd_nn_stream_start(struct cli_instance *sh, int argc, char **argv)
{
	enum camera_res res = CAM_RES_QVGA;
	int rc;

	if (argc > 1) {
		int ok;

		res = nn_parse_res(argv[1], &ok);
		if (!ok) {
			cli_error(sh, "nn: usage: nn stream start [qqvga|qvga]\r\n");
			return 1;
		}
	}
	rc = nn_camera_start(res);
	if (rc != 0) {
		cli_error(sh, "nn: start failed (%d): NN busy (bench or another "
		              "stream), SDRAM down, or no model loaded?\r\n", rc);
		return 1;
	}
	/* The inference path is a SUBSCRIBER of the base capture: it attaches if the
	   base is already streaming RGB565, otherwise it stays enabled and idle
	   until `camera stream start`.  The intent is independent of the base. */
	if (!camera_streaming())
		cli_print(sh, "nn: inference enabled -- start the base "
		              "(`camera stream start`) to run\r\n");
	else
		cli_print(sh, "nn: inference stream started\r\n");
	return 0;
}

static int cmd_nn_stream_stop(struct cli_instance *sh, int argc, char **argv)
{
	int rc;

	(void)argc; (void)argv;

	rc = nn_camera_stop();
	if (rc == -1) {
		cli_warn(sh, "nn: not running\r\n");
		return 0;
	}
	/*
	 * [!] THE INCOMPLETE TEARDOWNS ARE RETRYABLE, NOT FAILURES (issue #72).
	 * -7 means the sink is detached but still pinned, so a producer callback
	 * may still be writing the staging buffers; -8 means another start or stop
	 * owns the transition.  In both the claim is still out and the release is
	 * idempotent, so repeating this command is what settles it -- which is the
	 * disposition the shared command reports for the same condition after
	 * `nn run`.
	 */
	if (rc == -8) {
		cli_error(sh, "nn: another `nn stream` start/stop is in progress -- "
		              "retry\r\n");
		return 1;
	}
	if (rc == -7) {
		cli_error(sh, "nn: the camera has not released the inference frame; "
		              "the stream stays reserved -- retry `nn stream stop`\r\n");
		return 1;
	}
	if (rc != 0) {
		cli_error(sh, "nn: stop timed out (%d) -- retry `nn stream stop`\r\n",
		          rc);
		return 1;
	}
	cli_print(sh, "nn: stopped\r\n");
	return 0;
}

static int cmd_nn_stream_stats(struct cli_instance *sh, int argc, char **argv)
{
	struct nn_camera_stats s;

	(void)argc; (void)argv;

	nn_camera_stats_get(&s);
	cli_print(sh, "state   : %s\r\n", s.running ? "running" : "stopped");
	cli_print(sh, "frames  : %lu delivered, %lu dropped\r\n",
	          (unsigned long)s.frames, (unsigned long)s.drops);
	cli_print(sh, "infers  : %lu, %lu error(s)\r\n",
	          (unsigned long)s.infers, (unsigned long)s.errors);
	if (s.infers) {
		cli_print(sh, "latency : %lu us (last)\r\n",
		          (unsigned long)s.last_us);
		cli_print(sh, "rate    : %lu.%02lu inferences/s\r\n",
		          (unsigned long)(s.fps_x100 / 100u),
		          (unsigned long)(s.fps_x100 % 100u));
	}
	return 0;
}

/*
 * Exported, not static: the shared command's table names this set under the
 * board's NN_SVC_HAS_STREAM capability.  It is the one symbol that crosses.
 */
const struct cli_cmd nn_board_stream_subcmds[] = {
	CLI_CMD_ARG_USAGE(start, NULL, "enable live inference",
	                  "usage: nn stream start [qqvga|qvga]\r\n",
	                  cmd_nn_stream_start, 1, 1),
	CLI_CMD(stop,  NULL, "stop live inference", cmd_nn_stream_stop),
	CLI_CMD(stats, NULL, "inference rate / latency / drops",
	        cmd_nn_stream_stats),
	CLI_SUBCMD_SET_END,
};
