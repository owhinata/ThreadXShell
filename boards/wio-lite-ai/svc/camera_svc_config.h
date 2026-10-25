/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    camera_svc_config.h
 * @brief   What the shared `camera` command has on Wio Lite AI (issue #143).
 *
 * The board says what it HAS; the shared command (shell/cmds/cmd_camera.c)
 * registers a subcommand only under its macro, and the board adapter
 * (port/camera/camera_svc_wio.c) implements only what is 1 here.  These are
 * properties, never a board identity test.  Every macro is defined, to 0 or 1,
 * so a reader of this file sees the whole set and a missing one is not
 * mistaken for a decision.
 */
#ifndef CAMERA_SVC_CONFIG_H
#define CAMERA_SVC_CONFIG_H

/** The `camera` line of the top-level `help`: what the module is. */
#define CAMERA_SVC_HELP "FPC-24 DVP camera (OV2640 over DCMI, QVGA RGB565 snapshot)"

/** `camera on` / `camera off`: XCLK and PWDN / RESETB are the board's to drive
 *  (PWDN is the deepest state; the rail itself stays on). */
#define CAMERA_SVC_HAS_POWER  1
/** `camera off` with the operator's stream running stops that stream (wio's
 *  stop waits for it, up to 1 s) and then powers down; a holder other than the
 *  operator still refuses (issue #143 decision 2). */
#define CAMERA_SVC_OFF_STOPS_STREAM 1

/** `camera info`: driver, sensor, XCLK and SCCB state. */
#define CAMERA_SVC_HAS_INFO   1

/** `capture test` / `stream start test`: the OV2640's COM7 colour bars. */
#define CAMERA_SVC_HAS_TEST   1

/** `camera save`: only with the microSD built in.  The one medium is "sd";
 *  see the f746 config for the X-macro's shape. */
#if BSP_ENABLE_SD
#define CAMERA_SVC_HAS_SAVE   1
#define CAMERA_SVC_SAVE_MEDIA(first, more) first("sd")
#else
#define CAMERA_SVC_HAS_SAVE   0
#endif

/** `camera send`: the last capture to the PC over YMODEM. */
#define CAMERA_SVC_HAS_SEND   1

/*
 * The board's own subcommands, appended to the shared `camera` table (see the
 * f746 config for the rules): the raw SCCB tools that are how anything is
 * learnt about a sensor probe could not identify.  Handlers in
 * cmds/cmd_camera_board.c.
 */
#define CAMERA_SVC_BOARD_DECLS                                                 \
	int camera_board_cmd_scan(struct cli_instance *sh, int argc, char **argv); \
	int camera_board_cmd_reg(struct cli_instance *sh, int argc, char **argv);

#define CAMERA_SVC_BOARD_SUBCMDS                                               \
	CLI_CMD(scan, NULL, "list SCCB addresses that ACK (0x08..0x77)",       \
	        camera_board_cmd_scan),                                        \
	CLI_CMD_ARG_USAGE(reg, NULL, "raw SCCB access <addr> <reg> [value] [-16]", \
	                  "<addr> <reg> [value] [-16]", camera_board_cmd_reg, 3, 2),

#endif /* CAMERA_SVC_CONFIG_H */
