/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    camera_svc_config.h
 * @brief   What the shared `camera` command has on STM32F746G-DISCO (issue #143).
 *
 * The board says what it HAS; the shared command (shell/cmds/cmd_camera.c)
 * registers a subcommand only under its macro, and the board adapter
 * implements only what is 1 here.  These are properties, never a board
 * identity test.  Every macro is defined, to 0 or 1, so a reader of this file
 * sees the whole set and a missing one is not mistaken for a decision.
 */
#ifndef CAMERA_SVC_CONFIG_H
#define CAMERA_SVC_CONFIG_H

/** The `camera` line of the top-level `help`: what the module is. */
#define CAMERA_SVC_HELP "B-CAMS-OMV camera (OV5640 on DCMI)"

/** `camera on` / `camera off`: the module's power can be switched (the OV5640
 *  has no power-only state, so `on` is a bring-up; `off` cuts the rail). */
#define CAMERA_SVC_HAS_POWER  1
/** `camera off` with the operator's stream running stops that stream, waits
 *  for it to end, then cuts the rail (a holder other than the operator still
 *  refuses).  0 would refuse a running stream instead. */
#define CAMERA_SVC_OFF_STOPS_STREAM 1

/** `camera info`: driver and sensor state. */
#define CAMERA_SVC_HAS_INFO   1

/** `capture test` / `stream start test`: the sensor has a colorbar. */
#define CAMERA_SVC_HAS_TEST   1

/** `camera save`: a FileX medium to write the last capture to. */
#define CAMERA_SVC_HAS_SAVE   1
/**
 * The media `camera save` names, the first being the default: the microSD and
 * the QSPI NOR filesystem.  The board resolves a name to its device
 * (camera_board_medium()).
 *
 * An X-macro, so the one list spells the usage ("sd|fs") and the refusal
 * ("sd or fs") as string literals at compile time: @p first is applied to the
 * first name and @p more to each one after it.
 */
#define CAMERA_SVC_SAVE_MEDIA(first, more) first("sd") more("fs")

/** `camera send`: the last capture to the PC over YMODEM. */
#define CAMERA_SVC_HAS_SEND   1

/*
 * The board's own subcommands, appended to the shared `camera` table.
 *
 * A run of CLI_CMD / CLI_CMD_ARG / CLI_CMD_ARG_USAGE entries (shell/include/
 * cli.h), EACH FOLLOWED BY A COMMA, that the shared command expands as is just
 * before CLI_SUBCMD_SET_END.  Empty means the board adds none.  The handlers
 * are the board's (cmds/cmd_camera_board.c), declared by
 * CAMERA_SVC_BOARD_DECLS, which the shared command expands at file scope after
 * cli.h (so struct cli_cmd is complete there and nowhere else needs it).
 *
 * [!] A NAME HERE THAT IS ALSO A SHARED SUBCOMMAND IS SILENTLY SHADOWED by
 * whichever entry the lookup meets first; the board's host test builds the
 * table and refuses a duplicate.
 *
 * The OV5640's own controls: resolution, pixel format, frame rate and the ISP
 * settings.
 */
#define CAMERA_SVC_BOARD_DECLS                                                 \
	int camera_board_cmd_res(struct cli_instance *sh, int argc, char **argv); \
	int camera_board_cmd_format(struct cli_instance *sh, int argc,         \
	                            char **argv);                              \
	int camera_board_cmd_fps(struct cli_instance *sh, int argc, char **argv); \
	int camera_board_cmd_set(struct cli_instance *sh, int argc, char **argv); \
	extern const struct cli_cmd camera_board_set_subcmds[];

#define CAMERA_SVC_BOARD_SUBCMDS                                               \
	CLI_CMD_ARG_USAGE(res, NULL, "set resolution <qqvga|qvga|vga|wvga>",   \
	                  "qqvga|qvga|vga|wvga", camera_board_cmd_res, 2, 0),  \
	CLI_CMD_ARG_USAGE(format, NULL,                                        \
	                  "set pixel format <rgb565|yuv422|y8|jpeg> (jpeg: snapshot <=VGA)", \
	                  "rgb565|yuv422|y8|jpeg", camera_board_cmd_format, 2, 0), \
	CLI_CMD_ARG_USAGE(fps, NULL, "set frame rate <15|30> (30 needs `lcd off`)", \
	                  "15|30", camera_board_cmd_fps, 2, 0),                \
	CLI_CMD_ARG_USAGE(set, camera_board_set_subcmds,                       \
	                  "OV5640 image quality (no arg = show current)",      \
	                  "[<setting> <value>]", camera_board_cmd_set, 1, 1),

#endif /* CAMERA_SVC_CONFIG_H */
