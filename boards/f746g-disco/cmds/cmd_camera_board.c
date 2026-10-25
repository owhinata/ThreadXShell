/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    cmd_camera_board.c
 * @brief   This board's half of the `camera` command: the OV5640 controls,
 *          and the two shell-layer services the shared command needs
 *          (owhinata/stm32f746g-disco#39/#41/#42, issue #143).
 *
 * The command itself -- probe / on / off / info / capture / save / send /
 * stream -- is shell/cmds/cmd_camera.c, shared with the other boards, over this
 * board's adapter port/camera/camera_svc_f746.c.  What stays here is what no
 * other board has, appended to that table by CAMERA_SVC_BOARD_SUBCMDS
 * (svc/camera_svc_config.h):
 *
 *   camera res <qqvga|qvga|vga|wvga>       resolution (owhinata/stm32f746g-disco#45)
 *   camera format <rgb565|yuv422|y8|jpeg>  pixel format
 *   camera fps <15|30>                     frame rate (owhinata/stm32f746g-disco#67)
 *   camera set [<setting> <value>]         ISP quality (owhinata/stm32f746g-disco#44)
 *
 * and the two functions of shell/cmds/camera_cmd_board.h: the YMODEM send
 * (cmds/cmd_xfer.c, owhinata/stm32f746g-disco#50) and the FileX medium a
 * `camera save` names (sd = microSD FAT32, fs = QSPI NOR, whose fs_device
 * gates the save shares with the fs/sd commands).
 *
 * Clean-room design; no third-party code reused.
 */
#include "cli.h"
#include "camera.h"
#include "camera_names.h"
#include "camera_svc_config.h" /* CAMERA_SVC_BOARD_DECLS: the names declared */
#include "camera_cmd_board.h"
#include "fs_cmd_core.h"
#include "fs_devices.h"       /* fs_sd_device() / fs_qspi_device() (camera save) */
#include "cmd_xfer.h"

#include <string.h>

/* What the shared table expands; declared once, there and here. */
CAMERA_SVC_BOARD_DECLS

/* ---- the two shell-layer services (shell/cmds/camera_cmd_board.h) --------- */

int camera_board_send(struct cli_instance *sh, const struct ym_source *src)
{
	return xfer_send_source(sh, src);
}

const struct fs_device *camera_board_medium(const char *name)
{
	if (strcmp(name, "sd") == 0)
		return fs_sd_device();
	if (strcmp(name, "fs") == 0)
		return fs_qspi_device();
	return NULL;
}

/* ---- printing --------------------------------------------------------------- */

/* The writer camera_settings_write() goes through: one cli_write() per line,
   as the shared command's (shell/cmds/cmd_camera.c). */
static int board_write(void *ctx, const char *s, size_t len)
{
	if (len == 0u)
		return 0;
	return (cli_write((struct cli_instance *)ctx, s, len) < 0) ? -1 : (int)len;
}

static void print_settings(struct cli_instance *sh,
                           const struct camera_settings *s)
{
	struct camera_out o = { board_write, sh, 0 };

	camera_settings_write(&o, s);
}

/* Signed decimal parse for the small bounded levels (brightness/hue/zoom). */
static int cam_parse_int(const char *s, int *out)
{
	const char *p = s;
	int sign = 1, val = 0, any = 0;

	if (p == NULL || *p == '\0')
		return -1;
	if (*p == '-' || *p == '+') {
		sign = (*p == '-') ? -1 : 1;
		p++;
	}
	for (; *p != '\0'; p++) {
		if (*p < '0' || *p > '9')
			return -1;
		val = val * 10 + (*p - '0');
		if (val > 100000)        /* far beyond any camera range */
			return -1;
		any = 1;
	}
	if (!any)
		return -1;
	*out = sign * val;
	return 0;
}

/*
 * `camera set` is a real subcommand tree (one leaf per OV5640 control) so the
 * hierarchical help (owhinata/stm32f746g-disco#37) lists every setting via `help
 * camera set`, and a missing/extra value auto-prints the leaf's usage.  Each leaf
 * validates its value, calls the matching port setter and reports the outcome; the
 * settings are cached and applied immediately when the sensor is live, else at the
 * next capture.  brightness/contrast/saturation/hue coexist (the driver fixes up the
 * OV5640's shared SDE_CTRL0/CTRL8 registers).
 */
static int set_reject(struct cli_instance *sh, const char *name, const char *val)
{
	cli_error(sh, "camera: bad value '%s' for %s\r\n", val, name);
	return 1;
}

static int set_report(struct cli_instance *sh, const char *name,
                      const char *val, int rc)
{
	if (rc != 0) {
		cli_error(sh, "camera: %s '%s' for %s\r\n", camera_strerror(rc),
		          val, name);
		return 1;
	}
	cli_print(sh, "camera: %s = %s\r\n", name, val);
	return 0;
}

/* The framework guarantees argv[1] (each leaf is registered mandatory = 2). */
static int cmd_set_brightness(struct cli_instance *sh, int argc, char **argv)
{
	int n;
	(void)argc;
	if (cam_parse_int(argv[1], &n) != 0)
		return set_reject(sh, argv[0], argv[1]);
	return set_report(sh, argv[0], argv[1], camera_set_brightness(n));
}

static int cmd_set_contrast(struct cli_instance *sh, int argc, char **argv)
{
	int n;
	(void)argc;
	if (cam_parse_int(argv[1], &n) != 0)
		return set_reject(sh, argv[0], argv[1]);
	return set_report(sh, argv[0], argv[1], camera_set_contrast(n));
}

static int cmd_set_saturation(struct cli_instance *sh, int argc, char **argv)
{
	int n;
	(void)argc;
	if (cam_parse_int(argv[1], &n) != 0)
		return set_reject(sh, argv[0], argv[1]);
	return set_report(sh, argv[0], argv[1], camera_set_saturation(n));
}

static int cmd_set_hue(struct cli_instance *sh, int argc, char **argv)
{
	int n;
	(void)argc;
	if (cam_parse_int(argv[1], &n) != 0)
		return set_reject(sh, argv[0], argv[1]);
	return set_report(sh, argv[0], argv[1], camera_set_hue(n));
}

static int cmd_set_awb(struct cli_instance *sh, int argc, char **argv)
{
	int n;
	(void)argc;
	if (camera_named_lookup(camera_awb_names, argv[1], &n) != 0)
		return set_reject(sh, argv[0], argv[1]);
	return set_report(sh, argv[0], argv[1], camera_set_awb((enum camera_awb)n));
}

static int cmd_set_effect(struct cli_instance *sh, int argc, char **argv)
{
	int n;
	(void)argc;
	if (camera_named_lookup(camera_effect_names, argv[1], &n) != 0)
		return set_reject(sh, argv[0], argv[1]);
	return set_report(sh, argv[0], argv[1],
	                  camera_set_effect((enum camera_effect)n));
}

static int cmd_set_flip(struct cli_instance *sh, int argc, char **argv)
{
	int n;
	(void)argc;
	if (camera_named_lookup(camera_flip_names, argv[1], &n) != 0)
		return set_reject(sh, argv[0], argv[1]);
	return set_report(sh, argv[0], argv[1],
	                  camera_set_flip((enum camera_flip)n));
}

static int cmd_set_zoom(struct cli_instance *sh, int argc, char **argv)
{
	int n;
	(void)argc;
	if (cam_parse_int(argv[1], &n) != 0)
		return set_reject(sh, argv[0], argv[1]);
	return set_report(sh, argv[0], argv[1], camera_set_zoom(n));
}

static int cmd_set_night(struct cli_instance *sh, int argc, char **argv)
{
	int on;
	(void)argc;
	if (strcmp(argv[1], "on") == 0)
		on = 1;
	else if (strcmp(argv[1], "off") == 0)
		on = 0;
	else
		return set_reject(sh, argv[0], argv[1]);
	return set_report(sh, argv[0], argv[1], camera_set_night(on));
}

static int cmd_set_default(struct cli_instance *sh, int argc, char **argv)
{
	int rc;
	(void)argc;
	(void)argv;
	rc = camera_set_defaults();
	if (rc != 0) {
		cli_error(sh, "camera: %s\r\n", camera_strerror(rc));
		return 1;
	}
	cli_print(sh, "camera: settings reset to defaults\r\n");
	return 0;
}

/* `camera set` with no subcommand shows the current values; an unrecognized
   token falls here too (the parser passes it as an argument when it matches no
   leaf). */
int camera_board_cmd_set(struct cli_instance *sh, int argc, char **argv)
{
	struct camera_settings s;
	int rc;

	if (argc > 1) {
		cli_error(sh, "camera: unknown setting '%s'\r\n", argv[1]);
		cli_print(sh, "type 'help camera set' for the list of settings\r\n");
		return 1;
	}
	rc = camera_get_settings(&s);
	if (rc != 0) {
		cli_error(sh, "camera: %s\r\n", camera_strerror(rc));
		return 1;
	}
	print_settings(sh, &s);
	cli_print(sh, "type 'help camera set' for the list of settings\r\n");
	return 0;
}

/* Extern (not CLI_SUBCMD_SET_CREATE, which is static): the shared table's
   `set` entry points at it. */
const struct cli_cmd camera_board_set_subcmds[] = {
	CLI_CMD_ARG_USAGE(brightness, NULL, "<-4..4>", "<-4..4>",
	                  cmd_set_brightness, 2, 0),
	CLI_CMD_ARG_USAGE(contrast,   NULL, "<-4..4>", "<-4..4>",
	                  cmd_set_contrast, 2, 0),
	CLI_CMD_ARG_USAGE(saturation, NULL, "<-4..4>", "<-4..4>",
	                  cmd_set_saturation, 2, 0),
	CLI_CMD_ARG_USAGE(hue,    NULL, "<-180..150> in 30 deg steps", "<-180..150>",
	                  cmd_set_hue, 2, 0),
	CLI_CMD_ARG_USAGE(awb,    NULL, "<auto|sunny|office|home|cloudy>",
	                  "auto|sunny|office|home|cloudy", cmd_set_awb, 2, 0),
	CLI_CMD_ARG_USAGE(effect, NULL, "<none|bw|sepia|negative|blue|red|green>",
	                  "none|bw|sepia|negative|blue|red|green",
	                  cmd_set_effect, 2, 0),
	CLI_CMD_ARG_USAGE(flip,   NULL, "<none|mirror|flip|both>",
	                  "none|mirror|flip|both", cmd_set_flip, 2, 0),
	CLI_CMD_ARG_USAGE(zoom,   NULL, "<1|2|4|8>", "1|2|4|8", cmd_set_zoom, 2, 0),
	CLI_CMD_ARG_USAGE(night,  NULL, "<on|off>", "on|off", cmd_set_night, 2, 0),
	/* Why neutral still means flip: owhinata/stm32f746g-disco#68. */
	CLI_CMD(default,    NULL, "reset settings (neutral; flip defaults to flip)",
	        cmd_set_default),
	CLI_SUBCMD_SET_END,
};

/* ---- resolution / pixel format (owhinata/stm32f746g-disco#45) ----------------- */
/* Each leaf changes one axis and keeps the other; camera_set_format re-programs
   the sensor (resolution/format scalers + the per-mode HTS/VTS/PCLK fps table)
   and is refused while a stream or GUIX preview owns the DCMI. */
int camera_board_cmd_res(struct cli_instance *sh, int argc, char **argv)
{
	struct camera_mode m;
	int n;

	(void)argc;
	if (camera_named_lookup(camera_res_names, argv[1], &n) != 0)
		return set_reject(sh, argv[0], argv[1]);
	if (camera_get_mode(&m) != 0)
		return set_report(sh, argv[0], argv[1], CAM_ERR_STATE);
	return set_report(sh, argv[0], argv[1],
	                  camera_set_format((enum camera_res)n,
	                                    (enum camera_format)m.format));
}

int camera_board_cmd_format(struct cli_instance *sh, int argc, char **argv)
{
	struct camera_mode m;
	int n;

	(void)argc;
	if (camera_named_lookup(camera_format_names, argv[1], &n) != 0)
		return set_reject(sh, argv[0], argv[1]);
	if (camera_get_mode(&m) != 0)
		return set_report(sh, argv[0], argv[1], CAM_ERR_STATE);
	return set_report(sh, argv[0], argv[1],
	                  camera_set_format((enum camera_res)m.res,
	                                    (enum camera_format)n));
}

/* ---- frame rate (owhinata/stm32f746g-disco#67) -------------------------------- */
/* `camera fps <15|30>`: 15 = 24 MHz PCLK, 30 = 48 MHz.  30 fps only takes effect
   for a small mode while the LTDC is not scanning out (otherwise it is clamped to
   15 fps so the 48 MHz DCMI burst does not overrun the SDRAM); use `lcd off`. */
int camera_board_cmd_fps(struct cli_instance *sh, int argc, char **argv)
{
	int n;

	(void)argc;
	if (cam_parse_int(argv[1], &n) != 0 || (n != 15 && n != 30))
		return set_reject(sh, argv[0], argv[1]);
	return set_report(sh, argv[0], argv[1], camera_set_fps((unsigned)n));
}
