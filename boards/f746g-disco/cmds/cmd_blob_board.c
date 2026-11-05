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
 * do or say: receive over the VCP, refuse a raw transfer on the telnet console
 * (decision D7; the telnet case is issue #197), say who can hold the busy flag
 * and where to look when the QSPI driver is down, and report the VCP's RX ring
 * high-water (decision D6).
 */
#include "cli.h"
#include "cli_instance.h"      /* sh->tr: which console this is */
#include "blob_cmd_board.h"
#include "blob.h"             /* BLOB_BUSY_HOLDERS */
#include "blob_err.h"
#include "cli_backend_uart.h"  /* the VCP's RX ring high-water */
#include "cmd_xfer.h"
#include "nx_shell.h"          /* nx_shell_transport: the telnet console */

int blob_board_receive(struct cli_instance *sh, const struct ym_sink *sink)
{
	return xfer_recv_sink_locked(sh, sink);
}

/* YMODEM needs the raw VCP.  The telnet console is told why it is refused, in
 * wio-lite-ai's words (its net_shell_guard()).  A background job's worker has
 * the foreground's transport, and the shared command refuses it before this
 * matters (op_console_ready). */
int blob_board_raw_console(struct cli_instance *sh)
{
	if (sh->tr != &nx_shell_transport)
		return 0;
	cli_error(sh, "ymodem: refused on the telnet console -- it would destroy "
	              "this session. Run it from the VCP console.\r\n");
	return -1;
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
