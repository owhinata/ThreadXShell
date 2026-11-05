/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    cmd_blob_board.c
 * @brief   This board's three console pieces of the shared `blob` command
 *          (shell/cmds/blob_cmd_board.h, issue #194).
 *
 * The command itself is shell/cmds/cmd_blob.c and the store src/blob.c (reads)
 * and src/blob_writer.c (the reservation, the erase and the program); what is
 * left here is what only this board's console can do or say: receive over
 * UART0, and name who holds the NOR or why its port is faulted.
 */
#include "cli.h"
#include "blob_cmd_board.h"
#include "blob_err.h"
#include "cmd_xfer.h"
#include "nor_cmd.h"      /* nor_cmd_say_busy -- the holder, in `nor`'s words */
#include "nor_flash.h"    /* nor_fail_reason */

int blob_board_receive(struct cli_instance *sh, const struct ym_sink *sink)
{
	return xfer_recv_sink_locked(sh, sink);
}

/* One console, the UART: there is nothing a raw transfer could be refused on. */
int blob_board_raw_console(struct cli_instance *sh)
{
	(void)sh;
	return 0;
}

int blob_board_say(struct cli_instance *sh, int rc)
{
	switch (rc) {
	case BLOB_ERR_BUSY:
		/* A lease or the writer reservation is out: name the holder, as
		 * `nor erase` does, rather than send the operator to look. */
		nor_cmd_say_busy(sh, "blob");
		return 1;
	case BLOB_ERR_FAULT:
		/* Not busy, and not worth retrying: the port latched a failure
		 * and only a reset clears it.  Sending an operator to look for
		 * a holder that does not exist is the wrong instruction. */
		cli_error(sh, "blob: the NOR port is faulted -- %s (reset "
		              "required)\r\n",
		          nor_fail_reason() ? nor_fail_reason() : "no reason "
		          "recorded");
		return 1;
	case BLOB_ERR_IO:
		/* The writer's own status went to the log (src/blob_writer.c). */
		cli_error(sh, "blob: NOR erase/program failed (`dmesg` has the "
		              "writer's status)\r\n");
		return 1;
	default:
		break;
	}
	return 0;
}
