/* shared-command: required */
/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    xfer_console.c
 * @brief   YMODEM over the console, the one wiring for every board (issue #231;
 *          see xfer_console.h).
 *
 * The protocol core (svc/ymodem.c) is transport-agnostic; here its IO vtable is
 * wired to the raw-console API (cli_console_claim / cli_read_byte / cli_write;
 * every discard is its own counted read, see the drain below).  The caller supplies the source or sink: `camera send`'s last
 * capture and `blob write`'s slot on every board, and on wio-lite-ai `wifi
 * flash backup`'s RTL8720DN flash reader and `wifi flash imgload`'s PSRAM
 * staging buffer.  Ported from ../stm32f746g-disco (its
 * owhinata/stm32f746g-disco#50); until issue #231 each board had a copy of
 * this in its cmds/cmd_xfer.c.  Clean-room.
 *
 * [!] A SHARED TRANSLATION UNIT OWNS NO MUTABLE STORAGE
 * (cmake/check_no_mutable_storage.py, per board).  The transfer owner and its
 * two working areas are the board's (xfer_console_board_facts, in its
 * cmds/cmd_xfer_board.c), and so is the answer to the telnet question.
 */
#include "xfer_console.h"

#include "cli.h"
#include "log.h"
#include "tx_api.h"      /* tx_time_get(): the drain's longest gap */

/* ---- the board's table ---------------------------------------------------- *
 *
 * Read on every entry.  A field left NULL refuses before the console is
 * touched (xfer_console.h): the alternative directions are a transfer with no
 * owner (a NULL dereference in the protocol core) and a board whose telnet
 * refusal vanished because a field was forgotten.
 */
static const struct xfer_console_facts *xfer_facts(void)
{
	const struct xfer_console_facts *f = &xfer_console_board_facts;

	return (f->owner != NULL && f->telnet != NULL) ? f : NULL;
}

static void xfer_say_unwired(struct cli_instance *sh)
{
	cli_error(sh, "ymodem: refused -- this firmware's transfer table "
	              "(xfer_console_board_facts) has an empty field\r\n");
}

int xfer_console_no_telnet(struct cli_instance *sh, const char *what)
{
	(void)sh;
	(void)what;
	return 0;
}

/*
 * Not over telnet (issue #228).  YMODEM needs a raw, 8-bit clean console, and
 * the telnet one (shell/cmds/net_tty.c) is not: it drops IAC sequences on the
 * way in and doubles 0xFF on the way out, does not negotiate BINARY, so the
 * client may rewrite CR, and its receive ring is 512 B against a 1,029 B
 * block (YMODEM over telnet is issue #197).  Asked as the first statement of
 * every entry below, so a caller that forgot to ask is still refused; a caller
 * that would break something first (erase a slot, invalidate a staged image,
 * take the WiFi link down) asks it itself before it does.
 */
int xfer_refuse_if_telnet(struct cli_instance *sh)
{
	const struct xfer_console_facts *f = xfer_facts();

	if (f == NULL) {
		xfer_say_unwired(sh);
		return 1;
	}
	return f->telnet(sh, "ymodem") != 0;
}

/* ---- YMODEM IO adapter: cli_read_byte / cli_write ------------------------- */

/*
 * SEND direction only.  Everything read back while SENDING is protocol -- ACK /
 * NAK / 'C' / CAN -- so a 0x03 can only be a person pressing Ctrl+C, and
 * mapping it to an abort is safe.  -1 / -2 already match YM_IO_TIMEOUT /
 * YM_IO_ABORT.
 */
static int io_getc_send(void *ctx, unsigned timeout_ms)
{
	int r = cli_read_byte((struct cli_instance *)ctx, timeout_ms);

	return (r == 3) ? YM_IO_ABORT : r;
}

/*
 * RECEIVE direction: every byte passes untouched, 0x03 included.
 *
 * [!] 0x03 IS NOT AN ABORT HERE.  While receiving, the incoming stream is the
 * FILE, in which 0x03 is just another data byte (about 1 in 256 of binary
 * content, and it can equally land in a block's CRC).  Mapping it, as the
 * send direction does, was the bug that broke wio-lite-ai's `wifi flash
 * imgload` on the first try.  cli_read_byte() itself never interprets 0x03,
 * so this is simply the transport's raw byte.
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
 *
 * [!] EVERY DISCARD HERE IS COUNTED AND STOPS AT THE SAME CAP (issue #231,
 * D7): the flush before a transfer and after it as well as this drain.  The
 * shell core's cli_rx_flush() reads until the ring is empty, which against a
 * peer that never stops is never, and the command would not return; so the
 * wiring does not use it.  Hitting the cap is said the same way in all three
 * places, and nothing else changes: the transfer still runs, the result is
 * still returned and printed.
 */
#define XFER_DRAIN_QUIET_MS    1000u
#define XFER_DRAIN_MAX_BYTES   262144u

static void xfer_say_still_sending(struct cli_instance *sh, uint32_t dropped)
{
	cli_warn(sh, "ymodem: the peer is STILL sending -- stopped discarding after "
	         "%lu B, so stray bytes may reach the prompt and be run as a "
	         "command. Stop the program on the PC.\r\n",
	         (unsigned long)dropped);
}

/*
 * What the ring holds now, discarded without waiting -- what cli_rx_flush()
 * does, but at most XFER_DRAIN_MAX_BYTES.  @p where is "before" or "after"
 * (the transfer), for the log.
 */
static void xfer_flush(struct cli_instance *sh, const char *where)
{
	uint32_t dropped = 0u;

	/* cli_read_byte(sh, 0) polls the ring once: a byte, or -1 when empty. */
	while (dropped < XFER_DRAIN_MAX_BYTES && cli_read_byte(sh, 0u) >= 0)
		dropped++;
	if (dropped >= XFER_DRAIN_MAX_BYTES) {
		xfer_say_still_sending(sh, dropped);
		log_write(LOG_LEVEL_WRN, "xfer",
		          "drain cap hit at %lu B %s the transfer -- peer did not stop",
		          (unsigned long)dropped, where);
	}
}

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
	/* What arrived as the window closed, within the same cap. */
	while (dropped < XFER_DRAIN_MAX_BYTES && cli_read_byte(sh, 0u) >= 0)
		dropped++;

	if (dropped >= XFER_DRAIN_MAX_BYTES) {
		xfer_say_still_sending(sh, dropped);
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

/* ---- one transfer at a time (issue #230) --------------------------------- *
 *
 * svc/ymodem.c refuses a transfer while another runs on the board's owner
 * (YM_ERR_BUSY, having touched nothing).  These say so in one sentence on
 * every path: asked before a notice that tells the operator to start the PC
 * side, and again when the protocol core itself refused.
 */
static void xfer_say_busy(struct cli_instance *sh)
{
	cli_error(sh, "ymodem: another transfer is running -- run it again when "
	              "that one ends\r\n");
}

int xfer_refuse_if_busy(struct cli_instance *sh)
{
	const struct xfer_console_facts *f = xfer_facts();

	if (f == NULL) {
		xfer_say_unwired(sh);
		return 1;
	}
	if (!ymodem_busy(f->owner))
		return 0;
	xfer_say_busy(sh);
	return 1;
}

/* ---- results (one form on the three boards, issue #228) ------------------- *
 *
 * Every outcome is written to the log as a word, so `dmesg` reads the same on
 * every board.  A send also prints its one-line result, while the console is
 * still held.  A receive prints nothing: its caller says what was stored and
 * why not, and
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
	case YM_ERR_BUSY:    return "refused, another transfer is running";
	default:             break;
	}
	return "unknown result";
}

static int xfer_send_report(struct cli_instance *sh, const struct ym_source *src,
                            enum ym_result res)
{
	log_write(res == YM_OK ? LOG_LEVEL_INF : LOG_LEVEL_ERR, "xfer",
	          "send %s: %lu B, '%s'", xfer_result_word(res),
	          (unsigned long)src->size, src->name);

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
	case YM_ERR_BUSY:
		/* Only when another console started one after xfer_refuse_if_busy()
		 * said no (issue #230). */
		xfer_say_busy(sh);
		return 1;
	case YM_ERR_IO:
	default:
		cli_error(sh, "ymodem: transport error\r\n");
		return 1;
	}
}

static void xfer_recv_log(enum ym_result res, const struct ym_recv_diag *d)
{
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

/* ---- send ----------------------------------------------------------------- */

/*
 * The one send entry.  A caller that already holds the console (wio-lite-ai's
 * `wifi flash backup`, which keeps the RTL8720 download session open around
 * the transfer) calls it too: claims are counted per console (issue #229), so
 * the claim here is a second one and its release gives back only that one.
 */
int xfer_send_source(struct cli_instance *sh, const struct ym_source *src)
{
	struct ym_io   io = { sh, io_getc_send, io_put };
	enum ym_result res;
	int            rc;

	/* Before the notice: a refused transfer says nothing about starting a
	 * receiver. */
	if (xfer_refuse_if_telnet(sh))
		return 1;

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
	/* Nor one that another transfer would refuse (issue #230): asked after
	 * the claim and before the notice. */
	if (xfer_refuse_if_busy(sh)) {
		cli_console_release(sh);
		return 1;
	}
	/* The notice comes after the claim (issue #228): a transfer that cannot have
	 * the console says nothing about starting a receiver.  It asks for `rb -y`
	 * on every board: without -y lrzsz does not overwrite an existing file --
	 * block 0 carries a name and a size but no mtime -- and you quietly keep
	 * looking at the PREVIOUS capture, which reads like a camera that stopped
	 * producing frames (it happened during wio-lite-ai's bring-up). */
	cli_print(sh, "ymodem: sending '%s' (%lu bytes) over the console\r\n",
	          src->name, (unsigned long)src->size);
	cli_print(sh, "ymodem: start the receiver now -- `rb -y` (lrzsz YMODEM; -y "
	              "replaces an older file of that name) or Ctrl+A Ctrl+R in "
	              "picocom; Ctrl+C aborts\r\n");
	xfer_flush(sh, "before");          /* drop type-ahead / the command newline */
	res = ymodem_send(xfer_console_board_facts.owner, &io, src);
	xfer_flush(sh, "after");           /* drop a trailing 'O'/'C'/garbage tail   */
	if (res != YM_OK)
		xfer_drain_until_quiet(sh);    /* the receiver may still be NAKing */
	rc = xfer_send_report(sh, src, res);   /* printed while still held */
	cli_console_release(sh);
	return rc;
}

/* ---- receive (owhinata/wio-lite-ai#19 M5, issue #194) --------------------- */

int xfer_recv_sink_locked(struct cli_instance *sh, const struct ym_sink *sink,
                          struct ym_recv_diag *diag)
{
	struct ym_io   io = { sh, io_getc_recv, io_put };
	enum ym_result res;

	if (xfer_refuse_if_telnet(sh))
		return 1;

	xfer_flush(sh, "before");          /* drop type-ahead / the command newline */
	res = ymodem_recv(xfer_console_board_facts.owner, &io, sink, diag);
	/* Refused (issue #230): said here, since this prints nothing else, and
	 * then handled as any failure -- the drain below empties THIS console,
	 * where a sender the operator already started may be talking; the
	 * transfer that is running is on another one and is not touched. */
	if (res == YM_ERR_BUSY)
		xfer_say_busy(sh);
	xfer_flush(sh, "after");           /* drop a trailing 'O'/CAN/garbage tail   */
	if (res != YM_OK)
		xfer_drain_until_quiet(sh);    /* the sender may still be retrying */

	xfer_recv_log(res, diag);          /* the caller prints the outcome */
	return res == YM_OK ? 0 : 1;
}
