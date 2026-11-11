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
#include "tx_api.h"   /* tx_time_get(): the drain's longest gap */

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

/* ---- post-failure drain (one rule on the three boards, issue #228) -------- *
 *
 * After a FAILED transfer, keep discarding until the line goes quiet.  One
 * cli_rx_flush() is not enough (owhinata/wio-lite-ai#10 caught it on hardware):
 * a peer that has just been sent CAN retries the block it was on a couple of
 * times before it accepts the cancel, and bytes still arriving when this
 * returns reach the line editor and are RUN AS A COMMAND -- a rejected
 * sender's block 0 put its filename on the prompt.  Both directions drain: a
 * receiver that never saw our CAN goes on NAKing exactly like a sender does.
 *
 * The numbers are wio-lite-ai's, the only measured ones (board #2: lrzsz
 * retried twice well within a second, then quit; 286 B drained, nothing
 * leaked); until issue #228 f746g-disco and grove-vision-ai-v2 waited 400 ms
 * and stopped at 8 KB, and f746g-disco did not drain after a send at all.
 * The byte cap only matters against a peer that never stops: at 115200 baud
 * 256 KB takes about 23 s, at 921600 about 3 s, over USB CDC a moment.
 * Hitting it is "stopped discarding while bytes were still coming", which is
 * said on the console and in the log rather than read as a clean drain.  Nor
 * is the quiet window a proof that the peer stopped; it is sized against what
 * was seen, and the longest gap (from the start of the drain or the previous
 * late byte to the next one) goes to the log so the window can be checked
 * again.  What keeps that acceptable is that the sender and the console
 * operator are the same party: this is robustness, not a privilege boundary.
 *
 * Only on the failure path: after YM_OK the batch is closed and the peer has
 * stopped, so the success path keeps its timing.
 */
#define XFER_DRAIN_QUIET_MS    1000u
#define XFER_DRAIN_MAX_BYTES   262144u

/* The gap below is measured in ticks and reported in ms, and cli_read_byte()
 * takes its timeout in ticks as ms: both need the 1 kHz tick. */
_Static_assert(TX_TIMER_TICKS_PER_SECOND == 1000,
               "the drain's quiet window and gap assume 1 tick = 1 ms");

static void xfer_drain_until_quiet(struct cli_instance *sh)
{
	uint32_t dropped = 0u;
	ULONG    last    = tx_time_get();
	ULONG    gap_max = 0u;

	while (dropped < XFER_DRAIN_MAX_BYTES) {
		ULONG now;

		/* <0 is the quiet window expiring or the session going away; both
		 * mean stop.  cli_read_byte() waits on an event flag rather than
		 * spinning, so the other threads keep running throughout. */
		if (cli_read_byte(sh, XFER_DRAIN_QUIET_MS) < 0)
			break;
		now = tx_time_get();
		if ((ULONG)(now - last) > gap_max)       /* wrap-safe */
			gap_max = (ULONG)(now - last);
		last = now;
		dropped++;
	}
	cli_rx_flush(sh);

	if (dropped >= XFER_DRAIN_MAX_BYTES) {
		cli_warn(sh, "xfer: the peer is STILL sending -- stopped discarding after "
		         "%lu B, so stray bytes may reach the prompt and be run as a "
		         "command. Stop the program on the PC.\r\n",
		         (unsigned long)dropped);
		log_write(LOG_LEVEL_WRN, "xfer",
		          "drain cap hit at %lu B, longest gap %lu ms -- peer did not stop",
		          (unsigned long)dropped, (unsigned long)gap_max);
	} else if (dropped != 0u) {
		log_write(LOG_LEVEL_INF, "xfer",
		          "drained %lu late byte(s) from the peer after a failed "
		          "transfer, longest gap %lu ms",
		          (unsigned long)dropped, (unsigned long)gap_max);
	}
}

int xfer_send_source(struct cli_instance *sh, const struct ym_source *src)
{
	struct ym_io io = { sh, io_getc_send, io_put };
	enum ym_result res;

	/* No telnet question here: this board has no telnet console (issue #228). */
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
		xfer_drain_until_quiet(sh);  /* the receiver may still be NAKing */
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

	/* No telnet question here: this board has no telnet console (issue #228). */
	cli_rx_flush(sh);                /* type-ahead and the command's newline */
	res = ymodem_recv(&io, sink);
	cli_rx_flush(sh);                /* a trailing 'O' / CAN / garbage tail  */
	if (res != YM_OK)
		xfer_drain_until_quiet(sh);  /* the sender may still be retrying */

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
