/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    camera_cmd_board.h
 * @brief   The one thing the shared `camera` command needs from a board that
 *          a port cannot supply (issue #143).
 *
 * A FileX medium is a shell command's device with its own ownership gate.  A
 * port adapter holds no shell instance and must not reach up for it, so the
 * board's cmds/cmd_camera_board.c defines it and the shared command
 * (shell/cmds/cmd_camera.c) calls it -- the same arrangement the `nn`
 * command's file reader for its path source had until issue #196.
 *
 * [!] THE SHARED COMMAND INCLUDES THIS AND NOT A BOARD'S cmds/ HEADERS.  That
 * is why this exists: a board's fs_devices.h stays its own.  Sending over
 * YMODEM, which claims the console the command runs on, needed a board
 * function here too until issue #231 made the wiring shared
 * (shell/cmds/xfer_console.h); the shared command calls that directly.
 *
 * Declared unconditionally; a board defines it only when its
 * camera_svc_config.h says CAMERA_SVC_HAS_SAVE, and the shared command calls
 * it only under the same macro.
 */
#ifndef CAMERA_CMD_BOARD_H
#define CAMERA_CMD_BOARD_H

#ifdef __cplusplus
extern "C" {
#endif

struct cli_instance;
struct fs_device;     /* shell/cmds/fs_cmd_core.h; opaque here so a board
                         without FileX can include this */

/**
 * CAMERA_SVC_HAS_SAVE: the medium `camera save` writes to, by the name the
 * operator typed (one of CAMERA_SVC_SAVE_MEDIA).
 *
 * @return the board's device, or NULL for a name this board does not have.
 */
const struct fs_device *camera_board_medium(const char *name);

#ifdef __cplusplus
}
#endif

#endif /* CAMERA_CMD_BOARD_H */
