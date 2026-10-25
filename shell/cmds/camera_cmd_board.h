/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    camera_cmd_board.h
 * @brief   The two things the shared `camera` command needs from a board that a
 *          port cannot supply (issue #143).
 *
 * Both are shell-layer things: sending over YMODEM claims the console the
 * command runs on, and a FileX medium is a shell command's device with its own
 * ownership gate.  A port adapter holds no shell instance and must not reach up
 * for either, so the board's cmds/cmd_camera_board.c defines these and the
 * shared command (shell/cmds/cmd_camera.c) calls them -- the same arrangement
 * as nn_board_read_file() for `nn model load --path`.
 *
 * [!] THE SHARED COMMAND INCLUDES THIS AND NOT A BOARD'S cmds/ HEADERS.  That
 * is why these two exist: a board's cmd_xfer.h and fs_devices.h stay its own.
 *
 * Declared unconditionally; a board defines only what its camera_svc_config.h
 * says it has (CAMERA_SVC_HAS_SEND / CAMERA_SVC_HAS_SAVE), and the shared
 * command calls only under the same macros.
 */
#ifndef CAMERA_CMD_BOARD_H
#define CAMERA_CMD_BOARD_H

#ifdef __cplusplus
extern "C" {
#endif

struct cli_instance;
struct fs_device;     /* shell/cmds/fs_cmd_core.h; opaque here so a board
                         without FileX can include this */
struct ym_source;     /* svc/ymodem.h */

/**
 * CAMERA_SVC_HAS_SEND: send @p src to the PC over YMODEM on @p sh's console --
 * the board's own xfer_send_source(), which claims the console, runs the
 * transfer and prints the one-line result.  The caller owns @p src (and the
 * open frame behind it) for the duration.
 *
 * @return 0 on a completed transfer, 1 on any failure or cancel.
 */
int camera_board_send(struct cli_instance *sh, const struct ym_source *src);

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
