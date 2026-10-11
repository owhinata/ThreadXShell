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
 * implements only what is 1 here.  These are properties, never a board
 * identity test.  Every macro is defined, to 0 or 1, so a reader of this file
 * sees the whole set and a missing one is not mistaken for a decision.
 *
 * Not yet read by anything: issue #143 commit 1 adds the table, and the
 * shared command that reads it arrives in the commits that follow.
 */
#ifndef CAMERA_SVC_CONFIG_H
#define CAMERA_SVC_CONFIG_H

/** `camera on` / `camera off`: XCLK and PWDN / RESETB are the board's to drive
 *  (PWDN is the deepest state; the rail itself stays on). */
#define CAMERA_SVC_HAS_POWER  1

/** `camera info`: driver, sensor, XCLK and SCCB state. */
#define CAMERA_SVC_HAS_INFO   1

/** `capture --test` / `stream start --test`: the OV2640 has a colorbar. */
#define CAMERA_SVC_HAS_TEST   1

/*
 * `camera save` follows the microSD, which is a build option on this board
 * (BSP_ENABLE_SD, always defined to 0 or 1 by board.cmake; the tflm backend
 * defaults it off) -- a property of the build, not of the board.
 */
#if BSP_ENABLE_SD
#define CAMERA_SVC_HAS_SAVE   1
/** The one medium `camera save` names here, so it is also the default. */
#define CAMERA_SVC_SAVE_MEDIA "sd"
#else
#define CAMERA_SVC_HAS_SAVE   0
#endif

/** `camera send`: the last capture to the PC over YMODEM. */
#define CAMERA_SVC_HAS_SEND   1

/*
 * The board's own subcommands, appended to the shared `camera` table.
 *
 * A run of CLI_CMD / CLI_CMD_ARG / CLI_CMD_ARG_USAGE entries (shell/include/
 * cli.h), EACH FOLLOWED BY A COMMA, that the shared command expands as is just
 * before CLI_SUBCMD_SET_END.  Empty means the board adds none.  The handlers
 * are the board's (cmds/cmd_camera_board.c).
 *
 * [!] A NAME HERE THAT IS ALSO A SHARED SUBCOMMAND IS SILENTLY SHADOWED by
 * whichever entry the lookup meets first; the board's host test builds the
 * table and refuses a duplicate.
 *
 * Empty for now: scan and reg move here in issue #143 commit 3.
 */
#define CAMERA_SVC_BOARD_SUBCMDS

#endif /* CAMERA_SVC_CONFIG_H */
