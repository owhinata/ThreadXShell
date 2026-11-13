/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    blob_cmd_board.h
 * @brief   The shell-layer pieces of the shared `blob` command that a board
 *          provides (issue #194).
 *
 * The store itself is svc/blob/blob_svc.h and takes no console.  These take
 * one, because what they do is about the console: whether this console may
 * run `blob erase`, a sentence only the board can say, and the RX ring's
 * high-water.  Each board defines them in boards/<board>/cmds/
 * (cmd_blob_board.c).  Receiving a file and whether this console may carry a
 * raw transfer at all are the shared YMODEM wiring's (shell/cmds/
 * xfer_console.h, issue #231), which the shared command calls directly.
 */
#ifndef BLOB_CMD_BOARD_H
#define BLOB_CMD_BOARD_H

#include <stdint.h>   /* uint32_t */

#ifdef __cplusplus
extern "C" {
#endif

struct cli_instance;

/**
 * May @p sh run `blob erase`?  0 yes; -1 no, having told the operator why
 * (as one cli_error line starting "blob: ").  Asked before the reservation.
 * grove-vision-ai-v2 refuses a background job here, because its NOR writer
 * relies on running in the foreground (issue #203); the other boards answer 0.
 */
int blob_board_erase_console(struct cli_instance *sh);

/**
 * The board's own sentence for @p rc, when it has one: who holds a busy part,
 * why a port is faulted, where to look when the device is down.  Returns 1
 * having printed it (as one cli_error line starting "blob: "), or 0 to let
 * the shared command print its own.
 */
int blob_board_say(struct cli_instance *sh, int rc);

/**
 * Only on a board whose svc/blob_svc_config.h sets BLOB_SVC_HAS_RX_LEVEL:
 * start measuring @p sh's RX ring high-water (before a `blob write` erases),
 * and read it with the ring's size in bytes (0, or -1 when @p sh's console has
 * no such ring).
 */
void blob_board_rx_level_clear(struct cli_instance *sh);
int  blob_board_rx_level(struct cli_instance *sh, uint32_t *high,
                         uint32_t *size);

#ifdef __cplusplus
}
#endif

#endif /* BLOB_CMD_BOARD_H */
