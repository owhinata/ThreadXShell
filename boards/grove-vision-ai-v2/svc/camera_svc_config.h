/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    camera_svc_config.h
 * @brief   What the shared `camera` command has on Grove Vision AI V2 (issue #143).
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

/** No `camera on` / `camera off`: this board has no sensor power state for
 *  them to set, so they are not registered (issue #143 decision 2). */
#define CAMERA_SVC_HAS_POWER  0

/** `camera info`: identity, state and the last capture (new in issue #143;
 *  it reads no sensor register). */
#define CAMERA_SVC_HAS_INFO   1

/** No test pattern on this data path: `--test` is refused, in the same words
 *  `nn stream start --test` uses. */
#define CAMERA_SVC_HAS_TEST   0

/** No `camera save`: this board has no filesystem. */
#define CAMERA_SVC_HAS_SAVE   0

/** `camera send`: the last capture's planar B/G/R to the PC over YMODEM (new
 *  in issue #143). */
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
 * Empty for now: raw, vts, bench, profile and the tuning subcommands move here in issue #143 commit 4.
 */
#define CAMERA_SVC_BOARD_SUBCMDS

#endif /* CAMERA_SVC_CONFIG_H */
