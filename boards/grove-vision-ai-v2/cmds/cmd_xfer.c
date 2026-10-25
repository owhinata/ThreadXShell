/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    cmd_xfer.c
 * @brief   YMODEM over the console: receive (#92) and send (#143).
 *
 * Twenty lines of wiring between svc/ymodem.c, which is transport-agnostic, and
 * the shell's raw-console API.  Every rule this file follows is in cmd_xfer.h;
 * the code is the easy part.
 */
#include "cmd_xfer.h"

#include "cli.h"
#include "log.h"

/* The quiet window a failed transfer waits out.  Long enough for a sender that
 * was just cancelled to stop talking, short enough that a failure still reports
 * promptly.  A byte cap as well as a time one, because a peer that keeps
 * talking must not keep this here for ever. */
#define XFER_DRAIN_QUIET_MS   400u
#define XFER_DRAIN_MAX_BYTES  8192u

/*
 * SEND direction: everything read back while sending is protocol -- ACK / NAK /
 * 'C' / CAN -- so a 0x03 can only be a person pressing Ctrl+C, and mapping it to
 * an abort is safe (the same rule as the other two boards' cmd_xfer.c).  The
 * receive direction below must not.
 */
static int io_getc_send(void *ctx, unsigned timeout_ms)
{
	int r = cli_read_byte((struct cli_instance *)ctx, timeout_ms);

	return (r == 3) ? YM_IO_ABORT : r;
}

static int io_getc_recv(void *ctx, unsigned timeout_ms)
{
	/* [!] 0x03 IS NOT AN ABORT HERE.  See cmd_xfer.h: while receiving, the
	 * stream is the file.  The send direction on the other boards maps it;
	 * this must not. */
	return cli_read_byte((struct cli_instance *)ctx, timeout_ms);
}

static int io_put(void *ctx, const uint8_t *buf, size_t len)
{
	return cli_write((struct cli_instance *)ctx, buf, len) < 0 ? -1 : 0;
}

static void drain_until_quiet(struct cli_instance *sh)
{
	uint32_t dropped = 0u;

	while (dropped < XFER_DRAIN_MAX_BYTES) {
		if (cli_read_byte(sh, XFER_DRAIN_QUIET_MS) < 0)
			return;             /* quiet for the whole window */
		dropped++;
	}
	log_write(LOG_LEVEL_WRN, "xfer",
	          "peer still talking after %lu B; the rest reaches the prompt",
	          (unsigned long)dropped);
}

int xfer_send_source(struct cli_instance *sh, const struct ym_source *src)
{
	struct ym_io io = { sh, io_getc_send, io_put };
	enum ym_result res;

	cli_print(sh, "ymodem: sending '%s' (%lu bytes) over the console\r\n",
	          src->name, (unsigned long)src->size);
	cli_print(sh, "ymodem: start the receiver now -- e.g. `rz` (lrzsz YMODEM); "
	              "Ctrl+C aborts\r\n");
	switch (cli_console_claim(sh)) {
	case 0:
		break;
	case -2:
		cli_error(sh, "ymodem: cannot run in the background -- "
		              "drop the trailing '&'\r\n");
		return 1;
	default:
		cli_error(sh, "ymodem: console busy\r\n");
		return 1;
	}
	cli_rx_flush(sh);                /* type-ahead and the command's newline */
	res = ymodem_send(&io, src);
	cli_rx_flush(sh);                /* a trailing 'O' / 'C' / garbage tail  */
	if (res != YM_OK)
		drain_until_quiet(sh);       /* the receiver may still be NAKing */
	cli_console_release(sh);

	switch (res) {
	case YM_OK:
		cli_print(sh, "ymodem: sent %lu bytes OK\r\n",
		          (unsigned long)src->size);
		return 0;
	case YM_ERR_CANCEL:
		cli_warn(sh, "ymodem: cancelled\r\n");
		return 1;
	case YM_ERR_TIMEOUT:
		cli_error(sh, "ymodem: timeout -- no receiver? start `rz` on the PC\r\n");
		return 1;
	case YM_ERR_SOURCE:
		cli_error(sh, "ymodem: source read error\r\n");
		return 1;
	case YM_ERR_IO:
	default:
		cli_error(sh, "ymodem: transport error\r\n");
		return 1;
	}
}

int xfer_recv_sink_locked(struct cli_instance *sh, const struct ym_sink *sink)
{
	struct ym_io io = { sh, io_getc_recv, io_put };
	const struct ym_recv_diag *d;
	enum ym_result res;

	cli_rx_flush(sh);                /* type-ahead and the command's newline */
	res = ymodem_recv(&io, sink);
	cli_rx_flush(sh);                /* a trailing 'O' / CAN / garbage tail  */
	if (res != YM_OK)
		drain_until_quiet(sh);

	/* [!] THE POST-MORTEM GOES TO THE LOG, NOT THE CONSOLE.  While a transfer
	 * runs the PC's terminal belongs to `sb`, so anything printed here is
	 * swallowed by that program rather than shown; `dmesg` afterwards is
	 * where an operator can actually read it. */
	d = ymodem_recv_diag();
	log_write(res == YM_OK ? LOG_LEVEL_INF : LOG_LEVEL_ERR, "xfer",
	          "recv rc=%d blk=%lu crc=%lu seq=%lu shrt=%lu tmo=%lu",
	          (int)res, (unsigned long)d->blocks, (unsigned long)d->bad_crc,
	          (unsigned long)d->bad_seq, (unsigned long)d->short_read,
	          (unsigned long)d->timeouts);
	if (d->first_kind >= 0)
		log_write(LOG_LEVEL_ERR, "xfer",
		          "first bad blk kind=%02X seq=%d body=%lu/%lu",
		          (unsigned)d->first_kind, d->first_seq,
		          (unsigned long)d->first_got, (unsigned long)d->first_want);
	return res == YM_OK ? 0 : 1;
}
