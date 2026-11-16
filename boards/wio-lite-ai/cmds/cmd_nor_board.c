/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    cmd_nor_board.c
 * @brief   The shared `nor`'s table on Wio Lite AI (issue #250): the external
 *          W25Q128JV on OCTOSPI2 (owhinata/wio-lite-ai#37).
 *
 * The command is shell/cmds/cmd_nor.c; this file answers svc/nor/nor_svc.h.
 * Until issue #250 it was this board's cmds/cmd_nor.c, whose guards (issue
 * #249) stay here:
 *   - the range: an erase or write that touches the kv partition
 *     (port/flashdb/fal_cfg.h) by even one byte is refused -- `kv format yes`
 *     erases it.  The kv store does not take the busy flag on this board, so
 *     the range is what keeps a raw erase off FlashDB's sectors;
 *   - the hold: the NOR must be up, then the blob store's busy flag
 *     (src/blob.h), held for the whole erase, write or test and given back at
 *     the one exit below.  It orders a raw erase against a `blob write` /
 *     `blob erase` / `nn model load` and against another erase or test.  The
 *     blob slots themselves are NOT refused: under the flag a slot can be
 *     erased here, as on f746g-disco.
 *
 * `nor info` and `nor read` take no flag: nothing on this board orders a read
 * against the flag (neither the blob reads nor kv take it), and `blob write`
 * holds it for a whole transfer, during which a read is exactly what is
 * wanted.  A read interleaves with an erase or a program one driver call at a
 * time under the device mutex and disturbs neither.
 *
 * An erase is ONE driver call (nor_erase(), 64 KB blocks inside the range and
 * 4 KB sectors at its edges), so it has no point to stop at between sectors:
 * erase_cancels is 0 and Ctrl+C is not offered for it.
 *
 * Addresses are device offsets (0 .. 16 MB), not 0x70000000 pointers: the
 * window is never memory-mapped and stays fenced off in the MPU (port/nor).
 *
 * Clean-room design; no third-party code reused.
 */
#include "nor_svc.h"

#include "blob.h"          /* the busy flag and BLOB_BUSY_HOLDERS */
#include "fal_cfg.h"       /* KV_PART_OFFSET / KV_PART_LEN (issue #249) */
#include "fmt.h"
#include "nor_flash.h"

#include <stdarg.h>
#include <stdint.h>

_Static_assert(NOR_SECTOR_SIZE == NOR_SVC_ERASE_UNIT,
               "the shared `nor` erases whole 4 KB sectors");
_Static_assert(NOR_PAGE_SIZE == NOR_SVC_WRITE_MAX,
               "a `nor write` is at most one page");

/* The end of the kv partition, one past its last byte. */
#define NOR_KV_END (KV_PART_OFFSET + KV_PART_LEN)

static const char *nor_strerror(int rc)
{
	switch (rc) {
	case NOR_ERR_PARAM:   return "bad argument";
	case NOR_ERR_IO:      return "OCTOSPI2 transaction did not complete";
	case NOR_ERR_TIMEOUT: return "device stayed busy";
	case NOR_ERR_STATE:   return "driver not initialized / device down";
	case NOR_ERR_BUSY:    return "device busy (another command holds it)";
	default:              return "unknown error";
	}
}

static void nor_answer(int rc, uint32_t len, struct nor_svc_result *r)
{
	if (rc == NOR_OK) {
		r->status = NOR_SVC_OK;
		r->done = len;
	} else {
		r->status = NOR_SVC_IO;
		r->why = nor_strerror(rc);
	}
}

/* ---- info and read -------------------------------------------------------- */

static int nor_line(nor_svc_write_fn write, void *ctx, const char *fmt, ...)
{
	char line[96];
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = fmt_vsnformat(line, sizeof line, fmt, ap);
	va_end(ap);
	if (n < 0)
		return -1;
	if ((size_t)n >= sizeof line)
		n = (int)sizeof line - 1;
	return write(ctx, line, (size_t)n) < 0 ? -1 : 0;
}

static int nor_board_info(nor_svc_common_fn common, nor_svc_write_fn write,
                          void *ctx, struct nor_svc_result *r)
{
	struct nor_svc_info in = { 0 };
	uint32_t regs[4], hz;
	int ready;

	(void)r;
	ready = nor_flash_ready();
	nor_flash_get_id(in.jedec);
	nor_flash_get_regs(regs);
	hz = nor_flash_clock_hz();

	in.state = ready ? "ready (indirect)" : "NOT ready";
	in.state_note = "";
	in.jedec_valid = 1u;
	in.jedec_note = "";
	if (common(ctx, &in) != 0)
		return -1;
	/* A wrong controller configuration and a dead device look the same
	 * from the id alone (the first bring-up here read 00 00 00 with a clean
	 * CR.FSEL=0 -- transaction completed, wrong pins), so the registers. */
	if (nor_line(write, ctx, "  block   : %lu B (inside a range an erase uses "
	             "these)\r\n", (unsigned long)NOR_BLOCK_SIZE) ||
	    nor_line(write, ctx, "  clock   : %lu Hz (1-line SPI, Fast Read 0x0B)\r\n",
	             (unsigned long)hz) ||
	    nor_line(write, ctx, "  mapping : none -- indirect only, window fenced "
	             "off in the MPU\r\n") ||
	    nor_line(write, ctx, "  regs    : CR %08lX DCR1 %08lX DCR2 %08lX SR "
	             "%08lX\r\n", (unsigned long)regs[0], (unsigned long)regs[1],
	             (unsigned long)regs[2], (unsigned long)regs[3]))
		return -1;
	return 0;
}

static void nor_board_read(uint32_t addr, uint8_t *buf, uint32_t len,
                           struct nor_svc_result *r)
{
	if (!nor_flash_ready()) {
		r->status = NOR_SVC_DOWN;
		r->why = "device down (see `nor info`)";
		return;
	}
	nor_answer(nor_read(addr, buf, len), len, r);
}

/* ---- the range ------------------------------------------------------------ */

/* Refuse a range that touches the kv partition by even one byte.  len is not
 * 0 and addr + len is on the device (the shared command), so it cannot wrap. */
static void nor_board_range(enum nor_svc_op op, uint32_t addr, uint32_t len,
                            struct nor_svc_range *out)
{
	(void)op;
	if (addr >= NOR_KV_END || addr + len <= KV_PART_OFFSET) {
		out->kind = NOR_SVC_RANGE_OK;
		return;
	}
	out->kind = NOR_SVC_RANGE_INSIDE;
	out->lo = KV_PART_OFFSET;
	out->hi = NOR_KV_END;
	out->name = "kv store";
	out->hint = " -- `kv format yes` erases it";
}

/* ---- the hold and what is used inside it ---------------------------------- */

static void nor_op_erase(void *dev, uint32_t addr, uint32_t len,
                         const struct nor_svc_progress *p, struct nor_svc_result *r)
{
	(void)dev;
	(void)p;      /* one driver call: nowhere to stop (file comment) */
	nor_answer(nor_erase(addr, len), len, r);
}

static void nor_op_program(void *dev, uint32_t addr, const uint8_t *data,
                           uint32_t len, struct nor_svc_result *r)
{
	(void)dev;
	nor_answer(nor_write(addr, data, len), len, r);
}

static void nor_op_read(void *dev, uint32_t addr, uint8_t *buf, uint32_t len,
                        struct nor_svc_result *r)
{
	(void)dev;
	nor_answer(nor_read(addr, buf, len), len, r);
}

static const struct nor_svc_ops nor_ops = {
	.dev = NULL,
	.erase = nor_op_erase,
	.program = nor_op_program,
	.read = nor_op_read,
};

/* The NOR up, then the busy flag for the whole of @p body, given back here
 * and only here; on a refusal nothing is taken and body is not called. */
static void nor_board_hold(nor_svc_body_fn body, void *ctx,
                           struct nor_svc_result *r)
{
	if (!nor_flash_ready()) {
		r->status = NOR_SVC_DOWN;
		r->why = "device down (see `nor info`)";
		return;
	}
	if (blob_busy_acquire() != BLOB_OK) {
		r->status = NOR_SVC_BUSY;
		r->why = BLOB_BUSY_HOLDERS " is running; try again";
		return;
	}
	body(ctx, &nor_ops);
	blob_busy_release();
	r->status = NOR_SVC_OK;
}

const struct nor_svc_board nor_svc_board = {
	.part          = "W25Q128JV on OCTOSPI2",
	.size          = NOR_SIZE_BYTES,
	.erase_unit    = NOR_SECTOR_SIZE,
	.page          = NOR_PAGE_SIZE,
	.erase_cancels = 0u,
	.no_read       = NULL,
	.no_write      = NULL,
	.no_test       = NULL,
	.info          = nor_board_info,
	.read          = nor_board_read,
	.range         = nor_board_range,
	.hold          = nor_board_hold,
};
