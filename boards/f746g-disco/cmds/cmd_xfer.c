/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    cmd_xfer.c
 * @brief   YMODEM over the VCP, for this board's commands
 * (owhinata/stm32f746g-disco#50).
 *
 * Send for `camera send` (cmd_camera_board.c, the last capture from RAM) and,
 * since issue #194, receive for `blob write` (cmd_blob_board.c), the same shape
 * as grove-vision-ai-v2's cmd_xfer.c.  There is no `xfer` command here: the
 * `xfer send sd <path>` command that streamed a microSD file to the PC was
 * removed in issue #227 (a file on the card is copied by taking the card out).
 *
 * The protocol core (svc/ymodem.c) is transport-agnostic; here we wire its IO
 * vtable to the raw-console API (cli_console_claim/cli_read_byte/cli_write/
 * cli_rx_flush).  The caller supplies the source or sink.  Clean-room.
 */
#include "cli.h"
#include "cmd_xfer.h"
#include "log.h"
#include "net_tty.h"     /* refuse a raw transfer on the telnet console (#228) */

/* ---- YMODEM IO adapter: cli_read_byte / cli_write ------------------------- */

static int io_getc(void *ctx, unsigned timeout_ms)
{
	int r = cli_read_byte((struct cli_instance *)ctx, timeout_ms);
	/* Map a local Ctrl+C to a transfer abort; -1/-2 already match
	 * YM_IO_TIMEOUT/YM_IO_ABORT. */
	return (r == 3) ? YM_IO_ABORT : r;
}

static int io_put(void *ctx, const uint8_t *buf, size_t len)
{
	return cli_write((struct cli_instance *)ctx, buf, len) < 0 ? -1 : 0;
}

/* ---- receive (issue #194) ------------------------------------------------- */

/* How long the line must stay quiet after a failed receive, and how much a
 * drain may swallow before it gives up -- grove-vision-ai-v2's numbers. */
#define XFER_DRAIN_QUIET_MS   400u
#define XFER_DRAIN_MAX_BYTES  8192u

static int io_getc_recv(void *ctx, unsigned timeout_ms)
{
	/* [!] 0x03 IS NOT AN ABORT HERE.  See cmd_xfer.h: while receiving, the
	 * stream is the file, and about one byte in 256 of binary content is
	 * 0x03.  The send direction maps it (io_getc above); this must not. */
	return cli_read_byte((struct cli_instance *)ctx, timeout_ms);
}

static void drain_until_quiet(struct cli_instance *sh)
{
	uint32_t dropped = 0u;

	while (dropped < XFER_DRAIN_MAX_BYTES) {
		if (cli_read_byte(sh, XFER_DRAIN_QUIET_MS) < 0)
			return;             /* quiet for the whole window */
		dropped++;
	}
}

int xfer_recv_sink_locked(struct cli_instance *sh, const struct ym_sink *sink)
{
	struct ym_io io = { sh, io_getc_recv, io_put };
	const struct ym_recv_diag *d;
	enum ym_result res;

	/* Not over telnet (issue #228): YMODEM needs the raw VCP.  The first
	 * statement, so a caller that forgot to ask is still refused; one that
	 * would break something before it gets here (`blob write` erases the
	 * slot) asks first as well. */
	if (net_tty_guard(sh, "ymodem"))
		return 1;

	cli_rx_flush(sh);                /* type-ahead and the command's newline */
	res = ymodem_recv(&io, sink);
	cli_rx_flush(sh);                /* a trailing 'O' / CAN / garbage tail  */
	if (res != YM_OK)
		drain_until_quiet(sh);

	/* The post-mortem goes to the log, not the console: while a transfer
	 * runs the PC's terminal belongs to the sender. */
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

int xfer_send_source(struct cli_instance *sh, const struct ym_source *src)
{
	struct ym_io   io = { sh, io_getc, io_put };
	enum ym_result res;

	/* Not over telnet (issue #228), and before the notice: a refused transfer
	 * says nothing about starting a receiver. */
	if (net_tty_guard(sh, "ymodem"))
		return 1;

	cli_print(sh, "ymodem: sending '%s' (%lu bytes) over the VCP\r\n",
	          src->name, (unsigned long)src->size);
	cli_print(sh, "ymodem: start the receiver now -- e.g. `rb` (lrzsz YMODEM); "
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
	cli_rx_flush(sh);                  /* drop type-ahead / the command newline */
	res = ymodem_send(&io, src);
	cli_rx_flush(sh);                  /* drop a trailing 'O'/'C'/garbage tail   */
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
