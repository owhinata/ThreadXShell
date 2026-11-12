/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    cli_printf.c
 * @brief   Shell output API: staging buffer + colour + hexdump over svc/fmt.
 *
 * The public cli_print/error/warn/info/write/hexdump are implemented here.  Each
 * call is bracketed by cli_lock/cli_unlock (per-instance TX mutex) so formatting,
 * 32 B staging and flush are atomic, then reaches the transport only through
 * cli_tx_send_blocking() -- the ThreadX-specific lock + flow control live in
 * cli_core.c.  This file calls no tx_* function, so it builds and unit-tests on
 * the host against the tx_api.h shim.
 *
 * The actual number formatting is the clean-room formatter in svc/fmt.c (issue
 * owhinata/stm32f746g-disco#28); this file only supplies the staging-buffer putc
 * sink (cli_out_putc, ctx = the instance) and the lock/colour brackets.
 * Behaviour is unchanged from the earlier inline formatter.
 */
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "fmt.h"
#include "cli_instance.h"
#include "cli_internal.h"
#include "cli_vt100.h"

/* ---- staging ----------------------------------------------------------- */

void cli_out_flush(struct cli_instance *sh)
{
	if (sh->out_len == 0)
		return;
	/* Once a send has failed this command, discard the rest (req §11). */
	if (!sh->tx_failed) {
		if (cli_tx_send_blocking(sh, (const uint8_t *)sh->out_buf,
		                         sh->out_len) < 0)
			sh->tx_failed = 1;
	}
	sh->out_len = 0;
}

void cli_out_discard(struct cli_instance *sh)
{
	sh->out_len = 0;
}

void cli_out_putc(struct cli_instance *sh, char c)
{
	if (sh->tx_failed)
		return;                         /* drop further output this command */
	sh->out_buf[sh->out_len++] = c;
	if (sh->out_len >= CLI_PRINTF_BUFFER_SIZE)
		cli_out_flush(sh);
}

/* fmt putc sink: ctx is the owning instance, output goes to its staging. */
static void inst_putc(void *ctx, char c)
{
	cli_out_putc((struct cli_instance *)ctx, c);
}

static void out_str(struct cli_instance *sh, const char *s)
{
	while (*s)
		cli_out_putc(sh, *s++);
}

/* ---- output bracket (lock + bg line-break, owhinata/stm32f746g-disco#5/#25) --- */

int cli_out_begin(struct cli_instance *sh)
{
	if (cli_lock(sh) != 0) {
		sh->tx_failed = 1;      /* so the command result is forced non-zero */
		return -1;
	}
	/* Background job (owhinata/stm32f746g-disco#25): on the FIRST output since the
    foreground prompt
	 * was last (re)drawn, break to a fresh line so this output does not splice
	 * into the user's half-typed line, and invalidate the fg's render bookkeeping
	 * (old_rows/draw_row = 0 -> "draw at the cursor, do not erase") so the fg
	 * repaints prompt+line below this output on its next keystroke.  Best-effort:
	 * a dropped break just leaves the staged output to set tx_failed as usual. */
	if (sh->fg != NULL && sh->fg->render_dirty == 0) {
		cli_tx_send_blocking(sh, (const uint8_t *)"\r\n", 2);
		sh->fg->old_rows     = 0;
		sh->fg->draw_row     = 0;
		sh->fg->render_dirty = 1;
	}
	return 0;
}

void cli_out_end(struct cli_instance *sh)
{
	cli_unlock(sh);
}

/* ---- console hand-over for a binary transfer -------------------------- *
 * Kept here, not in cli_core.c: it reaches ThreadX only through cli_lock /
 * cli_unlock, so the host tests run these and not a stand-in (issue #229). */

/*
 * Console hand-over for a binary transfer (owhinata/stm32f746g-disco#50).  Take the
 * output lock for the WHOLE transfer (so a background job's output cannot interleave
 * into the YMODEM byte stream -- it blocks behind the transfer; with a
 * CLI_TX_MUTEX_WAIT deadline configured it drops at the deadline instead,
 * owhinata/stm32f746g-disco#25) and count one claim on this console, so
 * cli_tx_send_blocking stops draining its RX and _write drops printf bound for it.
 * Returns 0 on success, -2 if called from a background job, -1 if the lock could
 * not be acquired, -3 if the count is at its limit.
 *
 * Counted, not a flag (issue #229): each console has its own count, so one
 * console's release never ends another's transfer, and a nested claim on the
 * same console is ended only by its matching release.  The count moves together
 * with the lock: +1 only with a lock taken here, -1 just before the lock is given
 * back, and a refusal after the lock gives that one take back.  It does not wrap:
 * at its limit the claim is refused, and cli_console_claimable() says so before
 * anything is taken.
 *
 * A bg-job worker (sh->fg != NULL) is REFUSED: the RX ring is a strict SPSC pipe
 * owned by the foreground thread, and the USART RX ISR posts CLI_EVT_RX to the
 * FOREGROUND's event group (u->sh == fg), not the worker's -- so a worker's
 * cli_read_byte() would never wake on an ACK while the foreground line editor
 * drains the bytes.  A binary transfer must run in the foreground
 * (owhinata/stm32f746g-disco#50/#25).
 */
int cli_console_claimable(struct cli_instance *sh)
{
	if (sh->fg != NULL)
		return -2;
	if (sh->console_claims == UINT16_MAX)
		return -3;
	return 0;
}

int cli_console_claim(struct cli_instance *sh)
{
	int rc;

	if (sh->fg != NULL)
		return -2;              /* before the lock, as it always was */
	if (cli_lock(sh) != 0)
		return -1;
	rc = cli_console_claimable(sh);
	if (rc != 0) {
		cli_unlock(sh);
		return rc;
	}
	sh->console_claims++;
	return 0;
}

/* A release with no claim counted does nothing: the lock it would give back is
 * not one a claim took. */
void cli_console_release(struct cli_instance *sh)
{
	if (sh->console_claims == 0u)
		return;
	sh->console_claims--;
	cli_unlock(sh);
}

/* ---- public API -------------------------------------------------------- */

/* Bracket a formatted call with lock + autoflush; @p color is "" for cli_print
 * or a VT100 SGR for the level helpers (and "" when CLI_USE_COLOR=0). */
static int vemit(struct cli_instance *sh, const char *color,
                 const char *fmt, va_list ap)
{
	if (cli_out_begin(sh) != 0)
		return -1;
	if (color[0]) out_str(sh, color);
	fmt_vformat(inst_putc, sh, fmt, ap);
	if (color[0]) out_str(sh, CLI_VT100_RESET);
	cli_out_flush(sh);
	int r = sh->tx_failed ? -1 : 0;
	cli_out_end(sh);
	return r;
}

int cli_print(struct cli_instance *sh, const char *fmt, ...)
{
	va_list ap; va_start(ap, fmt);
	int r = vemit(sh, "", fmt, ap);
	va_end(ap);
	return r;
}

int cli_error(struct cli_instance *sh, const char *fmt, ...)
{
	va_list ap; va_start(ap, fmt);
	int r = vemit(sh, CLI_VT100_RED, fmt, ap);
	va_end(ap);
	return r;
}

int cli_warn(struct cli_instance *sh, const char *fmt, ...)
{
	va_list ap; va_start(ap, fmt);
	int r = vemit(sh, CLI_VT100_YELLOW, fmt, ap);
	va_end(ap);
	return r;
}

int cli_info(struct cli_instance *sh, const char *fmt, ...)
{
	va_list ap; va_start(ap, fmt);
	int r = vemit(sh, CLI_VT100_GREEN, fmt, ap);
	va_end(ap);
	return r;
}

int cli_write(struct cli_instance *sh, const void *data, size_t len)
{
	const uint8_t *p = (const uint8_t *)data;
	if (cli_out_begin(sh) != 0)
		return -1;
	for (size_t i = 0; i < len; i++)
		cli_out_putc(sh, (char)p[i]);
	cli_out_flush(sh);
	int r = sh->tx_failed ? -1 : 0;
	cli_out_end(sh);
	return r;
}

int cli_hexdump_base(struct cli_instance *sh, const void *data, size_t len,
                     unsigned long long base)
{
	const uint8_t *p = (const uint8_t *)data;
	char body[20];

	if (cli_out_begin(sh) != 0)
		return -1;

	for (size_t off = 0; off < len; off += 16) {
		/* Ctrl+C between rows: stop before emitting the next 16-byte line
		 * (issue #16).  Drop the staged tail BEFORE releasing the lock, then
		 * let the dispatcher detect cancel_req and print "^C".  The discard is
		 * not cosmetic (issue #17): staging auto-flushes every 32 B but a row
		 * is 77 B, so up to 31 B of the last row are still staged here, and a
		 * caller that ignores this -1 and keeps printing would emit them as
		 * the head of its next line. */
		if (cli_cancel_requested(sh)) {
			cli_out_discard(sh);
			cli_out_end(sh);
			return -1;
		}

		int n = fmt_utoa(base + (unsigned long long)off, 16, 0, body);
		fmt_padded(inst_putc, sh, NULL, body, n, 8, 1, 0);   /* %08x offset */
		out_str(sh, "  ");

		for (int j = 0; j < 16; j++) {
			if (off + (size_t)j < len) {
				int h = fmt_utoa(p[off + j], 16, 0, body);
				fmt_padded(inst_putc, sh, NULL, body, h, 2, 1, 0);  /* %02x */
				cli_out_putc(sh, ' ');
			} else {
				out_str(sh, "   ");
			}
		}
		cli_out_putc(sh, ' ');

		for (int j = 0; j < 16 && off + (size_t)j < len; j++) {
			uint8_t b = p[off + j];
			cli_out_putc(sh, (b >= 0x20 && b <= 0x7E) ? (char)b : '.');
		}
		out_str(sh, "\r\n");
	}

	cli_out_flush(sh);
	int r = sh->tx_failed ? -1 : 0;
	cli_out_end(sh);
	return r;
}

int cli_hexdump(struct cli_instance *sh, const void *data, size_t len)
{
	return cli_hexdump_base(sh, data, len, 0);
}
