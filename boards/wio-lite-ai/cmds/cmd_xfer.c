/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    cmd_xfer.c
 * @brief   Shared YMODEM-over-console send/receive helpers (see cmd_xfer.h).
 *
 * The protocol core (svc/ymodem.c) is transport-agnostic; here we wire its IO
 * vtable to the raw-console API (cli_console_claim/cli_read_byte/cli_write/
 * cli_rx_flush).  The source/sink vtable is supplied by the caller: `camera send`'s
 * last capture (cmd_camera_board.c), `wifi flash backup`'s RTL8720DN flash reader
 * (owhinata/wio-lite-ai#19 M4) and `wifi flash imgload`'s PSRAM staging buffer
 * (M5, src/rtl8720_img.c), both in cmd_wifi_flash.c, and `blob write`'s slot
 * (cmd_blob_board.c).  Both directions share one io_put() and one set of rules about
 * who may read the console RX ring (see cmd_xfer.h).
 *
 * Ported from ../stm32f746g-disco (its owhinata/stm32f746g-disco#50) without its
 * `xfer` command; no command is registered here.  The same wiring is in the other
 * two boards' cmd_xfer.c (issue #228 made the three say and do the same).
 */
#include "cli.h"
#include "cmd_xfer.h"
#include "net_tty.h"     /* refuse a raw transfer on the telnet console */
#include "log.h"          /* transfer post-mortem into the dmesg ring -- see below */
#include "tx_api.h"       /* tx_time_get(): the drain's longest gap */

#include <string.h>

/* ---- YMODEM IO adapter: cli_read_byte / cli_write ------------------------- */

/*
 * SEND direction only.  Everything the board reads back while SENDING is protocol
 * -- ACK / NAK / 'C' / CAN -- so a 0x03 can only be a human pressing Ctrl+C, and
 * mapping it to an abort is safe.
 *
 * THIS IS WRONG FOR RECEIVING, and using it there was the bug that broke `wifi
 * imgload` on the first try: while receiving, the incoming stream is the FILE, in
 * which 0x03 is just another data byte (~1 in 256 of binary content, and it can
 * equally land in a block's CRC).  Treating those as Ctrl+C aborted the transfer
 * almost immediately.  See io_getc_recv().
 */
static int io_getc(void *ctx, unsigned timeout_ms)
{
	int r = cli_read_byte((struct cli_instance *)ctx, timeout_ms);
	/* -1/-2 already match YM_IO_TIMEOUT/YM_IO_ABORT. */
	return (r == 3) ? YM_IO_ABORT : r;
}

/*
 * RECEIVE direction: pass every byte through untouched, 0x03 included.
 *
 * The cost is that a local Ctrl+C cannot abort a receive -- there is no way to tell
 * it apart from file data, and guessing wrong corrupts the image.  Aborting is done
 * from the PC instead (cancel the sender; lrzsz emits the CAN sequence, which the
 * protocol core honours), and the handshake gives up on its own if no sender ever
 * appears.  cli_read_byte() itself never interprets 0x03, so this is simply the
 * transport's raw byte.
 */
static int io_getc_recv(void *ctx, unsigned timeout_ms)
{
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
		cli_warn(sh, "ymodem: the peer is STILL sending -- stopped discarding after "
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

/* ---- results (one form on the three boards, issue #228) ------------------- *
 *
 * Every outcome is written to the log as a word, so `dmesg` reads the same on
 * every board.  A send also prints its one-line result, while the console is
 * still held (the caller of the "already held" send cannot print for it).  A
 * receive prints nothing: its caller says what was stored and why not, and
 * while a receive runs the PC's terminal still belongs to the sender, so
 * `dmesg` is where its post-mortem is read.
 */
static const char *xfer_result_word(enum ym_result res)
{
	switch (res) {
	case YM_OK:          return "ok";
	case YM_ERR_CANCEL:  return "cancelled";
	case YM_ERR_TIMEOUT: return "timeout";
	case YM_ERR_SOURCE:  return "source read error";
	case YM_ERR_IO:      return "transport error";
	case YM_ERR_SINK:    return "rejected by the destination";
	case YM_ERR_PROTO:   return "protocol error";
	default:             break;
	}
	return "unknown result";
}

static int xfer_send_report(struct cli_instance *sh, const struct ym_source *src,
                            enum ym_result res)
{
	log_write(res == YM_OK ? LOG_LEVEL_INF : LOG_LEVEL_ERR, "xfer",
	          "send %s: '%s', %lu B", xfer_result_word(res), src->name,
	          (unsigned long)src->size);

	switch (res) {
	case YM_OK:
		cli_print(sh, "ymodem: sent %lu bytes OK\r\n",
		          (unsigned long)src->size);
		return 0;
	case YM_ERR_CANCEL:
		cli_warn(sh, "ymodem: cancelled\r\n");
		return 1;
	case YM_ERR_TIMEOUT:
		cli_error(sh, "ymodem: timeout -- no receiver? start `rb -y` on the PC\r\n");
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

static void xfer_recv_log(enum ym_result res)
{
	const struct ym_recv_diag *d = ymodem_recv_diag();

	log_write(res == YM_OK ? LOG_LEVEL_INF : LOG_LEVEL_ERR, "xfer",
	          "recv %s: blocks=%lu crc=%lu seq=%lu short=%lu tmo=%lu",
	          xfer_result_word(res), (unsigned long)d->blocks,
	          (unsigned long)d->bad_crc, (unsigned long)d->bad_seq,
	          (unsigned long)d->short_read, (unsigned long)d->timeouts);
	if (d->first_kind >= 0)
		log_write(LOG_LEVEL_ERR, "xfer",
		          "first bad blk kind=%02X seq=%d nseq=%d body=%lu/%lu "
		          "crc=%04X/%04X",
		          (unsigned)d->first_kind, d->first_seq, d->first_nseq,
		          (unsigned long)d->first_got, (unsigned long)d->first_want,
		          (unsigned)d->first_crc_want, (unsigned)d->first_crc_got);
}

int xfer_send_source_locked(struct cli_instance *sh, const struct ym_source *src)
{
	struct ym_io   io = { sh, io_getc, io_put };
	enum ym_result res;

	/*
	 * Not over telnet.  YMODEM needs a raw, 8-bit clean console, and the telnet one
	 * (shell/cmds/net_tty.c) is not: it drops IAC sequences on the way in and doubles
	 * 0xFF on the way out, does not negotiate BINARY, so the client may rewrite CR,
	 * and its receive ring is 512 B against a 1,029 B block (YMODEM over telnet is
	 * issue #197).  Checked here so every YMODEM user (`camera send`, `blob write`,
	 * `wifi flash imgload` / `flash backup`) is covered by one rule; a caller that
	 * would break something first (erase a slot, invalidate the staged image, take
	 * the WiFi link down) asks the same question itself before it does (issue #228).
	 */
	if (net_tty_guard(sh, "ymodem"))
		return 1;

	cli_rx_flush(sh);                  /* drop type-ahead / the command newline */
	res = ymodem_send(&io, src);
	cli_rx_flush(sh);                  /* drop a trailing 'O'/'C'/garbage tail   */
	if (res != YM_OK)
		xfer_drain_until_quiet(sh);    /* the receiver may still be NAKing */
	return xfer_send_report(sh, src, res);
}

int xfer_send_source(struct cli_instance *sh, const struct ym_source *src)
{
	int rc;

	/* Before the notice (issue #228): a refused transfer says nothing about starting
	 * a receiver.  xfer_send_source_locked() asks again, for its other callers. */
	if (net_tty_guard(sh, "ymodem"))
		return 1;

	cli_print(sh, "ymodem: sending '%s' (%lu bytes) over the console\r\n",
	          src->name, (unsigned long)src->size);
	cli_print(sh, "ymodem: start the receiver now -- `rb -y` (lrzsz YMODEM; -y "
	              "replaces an older file of that name) or Ctrl+A Ctrl+R in "
	              "picocom; Ctrl+C aborts\r\n");

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
	rc = xfer_send_source_locked(sh, src);
	cli_console_release(sh);
	return rc;
}

/* ---- receive direction (owhinata/wio-lite-ai#19 M5) ------------------------------ */

int xfer_recv_sink_locked(struct cli_instance *sh, const struct ym_sink *sink)
{
	struct ym_io   io = { sh, io_getc_recv, io_put };
	enum ym_result res;

	/*
	 * Not over telnet.  YMODEM needs a raw, 8-bit clean console, and the telnet one
	 * (shell/cmds/net_tty.c) is not: it drops IAC sequences on the way in and doubles
	 * 0xFF on the way out, does not negotiate BINARY, so the client may rewrite CR,
	 * and its receive ring is 512 B against a 1,029 B block (YMODEM over telnet is
	 * issue #197).  Checked here so every YMODEM user (`camera send`, `blob write`,
	 * `wifi flash imgload` / `flash backup`) is covered by one rule; a caller that
	 * would break something first (erase a slot, invalidate the staged image, take
	 * the WiFi link down) asks the same question itself before it does (issue #228).
	 */
	if (net_tty_guard(sh, "ymodem"))
		return 1;

	cli_rx_flush(sh);                  /* drop type-ahead / the command newline */
	res = ymodem_recv(&io, sink);
	cli_rx_flush(sh);                  /* drop a trailing 'O'/CAN/garbage tail   */
	if (res != YM_OK)
		xfer_drain_until_quiet(sh);    /* the sender may still be retrying */

	xfer_recv_log(res);               /* the caller prints the outcome */
	return res == YM_OK ? 0 : 1;
}
