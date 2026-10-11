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
 *
 * Not yet read by anything: issue #143 commit 1 adds the table, and the
 * shared command that reads it arrives in the commits that follow.
 */
#ifndef CAMERA_SVC_CONFIG_H
#define CAMERA_SVC_CONFIG_H

/** `camera on` / `camera off`: the module's power can be switched (the OV5640
 *  has no power-only state, so `on` is a bring-up; `off` cuts the rail). */
#define CAMERA_SVC_HAS_POWER  1

/** `camera info`: driver and sensor state. */
#define CAMERA_SVC_HAS_INFO   1

/** `capture --test` / `stream start --test`: the sensor has a colorbar. */
#define CAMERA_SVC_HAS_TEST   1

/** `camera save`: a FileX medium to write the last capture to. */
#define CAMERA_SVC_HAS_SAVE   1
/** The media `camera save` names, the first being the default: the microSD and
 *  the QSPI NOR filesystem.  The board resolves a name to its device. */
#define CAMERA_SVC_SAVE_MEDIA "sd", "fs"

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
 * Empty for now: res, format, fps and set move here in issue #143 commit 2.
 */
#define CAMERA_SVC_BOARD_SUBCMDS

#endif /* CAMERA_SVC_CONFIG_H */
