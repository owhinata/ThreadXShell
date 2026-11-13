/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    cmd_blob_board.c
 * @brief   This board's console pieces of the shared `blob` command
 *          (shell/cmds/blob_cmd_board.h, issue #194).
 *
 * The command itself is shell/cmds/cmd_blob.c and the store src/blob.c and
 * src/blob_writer.c; what is left here is what only this board's consoles can
 * do or say: who can hold the busy flag and where to look when the QSPI
 * driver is down, and the VCP's RX ring high-water (decision D6).  The receive
 * and the telnet refusal (decision D7; the telnet case is issue #197) are the
 * shared YMODEM wiring's (shell/cmds/xfer_console.c, issue #231).
 */
#include "cli.h"
#include "cli_instance.h"      /* sh->tr: which console this is */
#include "blob_cmd_board.h"
#include "blob.h"             /* BLOB_BUSY_HOLDERS */
#include "blob_err.h"
#include "cli_backend_uart.h"  /* the VCP's RX ring high-water */

/* Any console, foreground or background: the busy flag is what orders an
 * erase against everything else on this board. */
int blob_board_erase_console(struct cli_instance *sh)
{
	(void)sh;
	return 0;
}

int blob_board_say(struct cli_instance *sh, int rc)
{
	switch (rc) {
	case BLOB_ERR_STATE:
		cli_error(sh, "blob: QSPI driver down (`qspi info` says why)\r\n");
		return 1;
	case BLOB_ERR_BUSY:
		/* The busy flag is held by exactly these (src/blob.h). */
		cli_error(sh, "blob: another QSPI operation is running ("
		              BLOB_BUSY_HOLDERS ")\r\n");
		return 1;
	default:
		break;
	}
	return 0;
}

void blob_board_rx_level_clear(struct cli_instance *sh)
{
	cli_uart_rx_high_clear(sh);
}

int blob_board_rx_level(struct cli_instance *sh, uint32_t *high,
                        uint32_t *size)
{
	return cli_uart_rx_high(sh, high, size);
}
