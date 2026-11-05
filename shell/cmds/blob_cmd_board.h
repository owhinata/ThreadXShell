/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    blob_cmd_board.h
 * @brief   The shell-layer pieces of the shared `blob` command that a board
 *          provides (issue #194).
 *
 * The store itself is svc/blob/blob_svc.h and takes no console.  These three
 * take one, because what they do is about the console: receiving a file over
 * it, whether this console may carry a raw transfer at all, and a sentence only
 * the board can say.  Each board defines them in boards/<board>/cmds/
 * (cmd_blob_board.c), next to its YMODEM receiver -- the same split as
 * shell/cmds/camera_cmd_board.h.
 */
#ifndef BLOB_CMD_BOARD_H
#define BLOB_CMD_BOARD_H

#include "ymodem.h"   /* struct ym_sink */

#ifdef __cplusplus
extern "C" {
#endif

struct cli_instance;

/** Run one YMODEM receive into @p sink on @p sh, whose console the caller has
 *  claimed.  0 when the batch completed. */
int blob_board_receive(struct cli_instance *sh, const struct ym_sink *sink);

/** May this console carry a raw transfer?  0 yes; -1 no, having told the
 *  operator why (a telnet console, for one). */
int blob_board_raw_console(struct cli_instance *sh);

/**
 * The board's own sentence for @p rc, when it has one: who holds a busy part,
 * why a port is faulted, where to look when the device is down.  Returns 1
 * having printed it (as one cli_error line starting "blob: "), or 0 to let
 * the shared command print its own.
 */
int blob_board_say(struct cli_instance *sh, int rc);

#ifdef __cplusplus
}
#endif

#endif /* BLOB_CMD_BOARD_H */
