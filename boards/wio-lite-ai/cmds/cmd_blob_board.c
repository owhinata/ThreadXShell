/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    cmd_blob_board.c
 * @brief   This board's three console pieces of the shared `blob` command
 *          (shell/cmds/blob_cmd_board.h, issue #194).
 *
 * The command itself is shell/cmds/cmd_blob.c and the store src/blob.c; what is
 * left here is what only this board's console can do or say: receive over the
 * USB CDC console, refuse a raw transfer on telnet, and say which operations
 * hold the busy flag and where to look when the NOR is down.
 */
#include "cli.h"
#include "blob_cmd_board.h"
#include "blob_err.h"
#include "xfer_console.h"
#include "net_tty.h"       /* refuse a raw transfer on the telnet console */

int blob_board_receive(struct cli_instance *sh, const struct ym_sink *sink,
                       struct ym_recv_diag *diag)
{
	return xfer_recv_sink_locked(sh, sink, diag);
}

/* YMODEM needs the raw USB CDC console; the telnet one says why it refuses. */
int blob_board_raw_console(struct cli_instance *sh)
{
	return net_tty_guard(sh, "ymodem") ? -1 : 0;
}

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
		cli_error(sh, "blob: NOR device down (see `nor info`)\r\n");
		return 1;
	case BLOB_ERR_BUSY:
		/* The busy flag is held by exactly these three. */
		cli_error(sh, "blob: another blob operation is running (a `blob "
		              "write` / `erase`, or `nn model load`)\r\n");
		return 1;
	default:
		break;
	}
	return 0;
}
