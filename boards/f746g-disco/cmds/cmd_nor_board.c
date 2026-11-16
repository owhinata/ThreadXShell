/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    cmd_nor_board.c
 * @brief   The shared `nor`'s table on STM32F746G-DISCO (issue #250): the
 *          N25Q128A on QUADSPI, and `nor probe`.
 *
 * The command is shell/cmds/cmd_nor.c; this file answers svc/nor/nor_svc.h.
 * Until issue #250 it was this board's `qspi` (cmds/cmd_qspi.c,
 * owhinata/stm32f746g-disco#29), whose guards stay here:
 *   - the range: an erase that touches the key-value store's partition, the
 *     first megabyte, by even one byte is refused (issue #214) -- FlashDB's
 *     sectors, which a raw erase would turn into a corrupt store; `kv format
 *     yes` is the way to erase it;
 *   - the hold: the driver up, then the blob store's busy flag (src/blob.h)
 *     for as long as the erase or test runs, given back at the one exit
 *     below -- so it is refused while a blob operation, a `kv` command, an
 *     `nn model load` or another erase or test has it (issue #194).
 *
 * [!] AN ERASE IS 4 KB SUBSECTORS ONLY.  The 64 KB sector erase is never used
 * here: the range an operator names is what goes, and nothing outside it.
 * The progress function runs after each subsector, so Ctrl+C stops it there.
 *
 * `nor info` reads the controller's registers before it sends anything of
 * its own, so what it prints is what the controller looked like when it was
 * typed, then the driver's failure record (count, first and last; issue
 * #194).  The driver recovers by itself; there is no manual recover.
 *
 * `nor read` and `nor info` take no flag; the driver's mutex orders each
 * transfer.  There is no `nor write` on this board (nothing here needs one).
 *
 * Clean-room design; no third-party code reused.
 */
#include "cli.h"
#include "nor_svc.h"
#include "nor_svc_config.h"   /* the declaration of nor_board_probe */

#include "blob.h"            /* the busy flag and BLOB_BUSY_HOLDERS */
#include "fal_cfg.h"         /* KV_PART_OFFSET / KV_PART_LEN (issue #214) */
#include "fmt.h"
#include "qspi_flash.h"

#include "stm32f7xx_hal.h"   /* HAL_GetTick for the register lines */

#include <stdarg.h>
#include <stdint.h>

_Static_assert(QSPI_FLASH_SUBSECTOR_SIZE == NOR_SVC_ERASE_UNIT,
               "the shared `nor` erases whole 4 KB subsectors");
_Static_assert(QSPI_FLASH_PAGE_SIZE == NOR_SVC_WRITE_MAX,
               "the test programs one page at a time");

#define NOR_KV_END (KV_PART_OFFSET + KV_PART_LEN)

static const char *qspi_strerror(int rc)
{
	switch (rc) {
	case QSPI_FLASH_ERR_PARAM:   return "out of range";
	case QSPI_FLASH_ERR_HAL:     return "QUADSPI transfer failed";
	case QSPI_FLASH_ERR_TIMEOUT: return "busy timeout";
	case QSPI_FLASH_ERR_FLASH:   return "flash reported an error";
	case QSPI_FLASH_ERR_STATE:   return "driver down";
	default:                     return "unknown error";
	}
}

static void qspi_answer(int rc, uint32_t len, struct nor_svc_result *r)
{
	if (rc == QSPI_FLASH_OK) {
		r->status = NOR_SVC_OK;
		r->done = len;
	} else {
		r->status = NOR_SVC_IO;
		r->why = qspi_strerror(rc);
	}
}

/* ---- `nor probe` ---------------------------------------------------------- */

int nor_board_probe(struct cli_instance *sh, int argc, char **argv)
{
	uint8_t id[3];
	int rc;

	(void)argc;
	(void)argv;
	rc = qspi_flash_read_id(id);
	if (rc != 0) {
		cli_error(sh, "nor: read id failed: %s\r\n", qspi_strerror(rc));
		return 1;
	}
	cli_print(sh, "JEDEC %02X %02X %02X %s\r\n", id[0], id[1], id[2],
	          (id[0] == 0x20 && id[1] == 0xBA && id[2] == 0x18)
	                  ? "(N25Q128A/MT25QL128)" : "(unexpected: wiring/AF?)");
	return 0;
}

/* ---- `nor info` ----------------------------------------------------------- */

struct qspi_live {
	struct qspi_flash_regs r;
	uint32_t state, error, tick;
};

static void qspi_live_now(struct qspi_live *l)
{
	qspi_flash_regs_now(&l->r, &l->state, &l->error);
	l->tick = HAL_GetTick();
}

static int qspi_line(nor_svc_write_fn write, void *ctx, const char *fmt, ...)
{
	char line[128];
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

/* SR: BUSY bit 5, FLEVEL bits 13:8, TCF bit 1, FTF bit 2, TEF bit 0.
 * CR: ABORT bit 1.  CCR: FMODE bits 27:26 (0 ind-write, 1 ind-read,
 * 2 polling, 3 mapped), DMODE bits 25:24 (0 = no data phase). */
static int qspi_regs_lines(nor_svc_write_fn write, void *ctx, const char *label,
                           const struct qspi_flash_regs *r)
{
	return qspi_line(write, ctx, "  %s: CR %08lx (abort %lu) SR %08lx (busy %lu "
	                 "flevel %lu tcf %lu ftf %lu tef %lu)\r\n", label,
	                 (unsigned long)r->cr, (unsigned long)((r->cr >> 1) & 1u),
	                 (unsigned long)r->sr, (unsigned long)((r->sr >> 5) & 1u),
	                 (unsigned long)((r->sr >> 8) & 0x3Fu),
	                 (unsigned long)((r->sr >> 1) & 1u),
	                 (unsigned long)((r->sr >> 2) & 1u),
	                 (unsigned long)(r->sr & 1u)) ||
	       qspi_line(write, ctx, "             CCR %08lx (fmode %lu dmode %lu) "
	                 "DLR %08lx AR %08lx\r\n", (unsigned long)r->ccr,
	                 (unsigned long)((r->ccr >> 26) & 3u),
	                 (unsigned long)((r->ccr >> 24) & 3u),
	                 (unsigned long)r->dlr, (unsigned long)r->ar);
}

static int qspi_live_lines(nor_svc_write_fn write, void *ctx, const char *label,
                           const struct qspi_live *l)
{
	return qspi_regs_lines(write, ctx, label, &l->r) ||
	       qspi_line(write, ctx, "             handle state 0x%02lx error 0x%lx, "
	                 "t %lu ms\r\n", (unsigned long)l->state,
	                 (unsigned long)l->error, (unsigned long)l->tick);
}

static int qspi_fail_lines(nor_svc_write_fn write, void *ctx, const char *label,
                           const struct qspi_flash_fail *f)
{
	return qspi_line(write, ctx, "  %s: t %lu ms, %s-%s, hal %d, state 0x%02lx "
	                 "error 0x%lx\r\n", label, (unsigned long)f->tick, f->op,
	                 f->phase, f->hal_rc, (unsigned long)f->state,
	                 (unsigned long)f->error) ||
	       qspi_regs_lines(write, ctx, "  regs   ", &f->regs) ||
	       qspi_line(write, ctx, "             recovery %s, handle state "
	                 "0x%02lx\r\n", f->recovered ? "ok"
	                                             : "FAILED (handle set to error)",
	                 (unsigned long)f->state_after) ||
	       qspi_regs_lines(write, ctx, "  after  ", &f->after);
}

static int nor_board_info(nor_svc_common_fn common, nor_svc_write_fn write,
                          void *ctx, struct nor_svc_result *res)
{
	const struct qspi_flash_info *fi = qspi_flash_get_info();
	struct nor_svc_info in = { 0 };
	struct qspi_flash_record d;
	struct qspi_live before, after;
	int id_rc, sr_rc, ready;
	uint8_t sr = 0u;

	(void)res;
	/* [!] Before any command of our own, so what is printed is what the
	 * controller looked like when the operator typed this. */
	qspi_live_now(&before);
	ready = qspi_flash_ready();
	id_rc = qspi_flash_read_id(in.jedec);
	sr_rc = qspi_flash_read_status(&sr);
	qspi_live_now(&after);
	qspi_flash_record_get(&d);

	in.state = ready ? "ready" : "driver down";
	in.state_note = "";
	in.jedec_valid = (id_rc == 0);
	in.jedec_note = (id_rc == 0) ? "" : "read failed";
	if (common(ctx, &in) != 0)
		return -1;

	if (qspi_line(write, ctx, "  sclk    : %lu MHz, read %s\r\n",
	              (unsigned long)(fi->sclk_hz / 1000000u),
	              qspi_flash_quad_enabled() ? "0x6B 1-1-4" : "0x0B 1-1-1"))
		return -1;
	if (sr_rc != 0) {
		if (qspi_line(write, ctx, "  status  : %s\r\n",
		              qspi_strerror(sr_rc)))
			return -1;
	} else if (qspi_line(write, ctx, "  status  : 0x%02x%s\r\n", sr,
	                     (sr & 0x01u) ? " (WIP)" : "")) {
		return -1;
	}
	if (qspi_live_lines(write, ctx, "before ", &before) ||
	    qspi_live_lines(write, ctx, "after  ", &after) ||
	    qspi_line(write, ctx, "  failures: %lu\r\n", (unsigned long)d.fail_count))
		return -1;
	if (d.fail_count > 0u &&
	    (qspi_fail_lines(write, ctx, "first  ", &d.first) ||
	     qspi_fail_lines(write, ctx, "last   ", &d.last)))
		return -1;
	return 0;
}

void nor_svc_board_read(uint32_t addr, uint8_t *buf, uint32_t len,
                        struct nor_svc_result *r)
{
	qspi_answer(qspi_flash_read(addr, buf, len), len, r);
}

/* ---- the range ------------------------------------------------------------ */

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
	uint32_t done;

	(void)dev;
	for (done = 0u; done < len; done += QSPI_FLASH_SUBSECTOR_SIZE) {
		int rc = qspi_flash_erase_subsector(addr + done);

		if (rc != QSPI_FLASH_OK) {
			r->status = NOR_SVC_IO;
			r->why = qspi_strerror(rc);
			r->done = done;
			return;
		}
		if (p != NULL && p->tick(p->ctx, done + QSPI_FLASH_SUBSECTOR_SIZE, len) &&
		    done + QSPI_FLASH_SUBSECTOR_SIZE < len) {
			r->status = NOR_SVC_CANCELLED;
			r->done = done + QSPI_FLASH_SUBSECTOR_SIZE;
			return;
		}
	}
	r->status = NOR_SVC_OK;
	r->done = len;
}

static void nor_op_program(void *dev, uint32_t addr, const uint8_t *data,
                           uint32_t len, struct nor_svc_result *r)
{
	(void)dev;
	qspi_answer(qspi_flash_write(addr, data, len), len, r);
}

static const struct nor_svc_ops nor_ops = {
	.dev = NULL,
	.erase = nor_op_erase,
	.program = nor_op_program,
};

/* The driver up, then the busy flag for the whole of @p body, given back here
 * and only here; on a refusal nothing is taken and body is not called. */
static void nor_board_hold(nor_svc_body_fn body, void *ctx,
                           struct nor_svc_result *r)
{
	if (!qspi_flash_ready()) {
		r->status = NOR_SVC_DOWN;
		r->why = "driver down";
		return;
	}
	if (blob_busy_acquire() != BLOB_OK) {
		r->status = NOR_SVC_BUSY;
		r->why = BLOB_BUSY_HOLDERS " is running";
		return;
	}
	body(ctx, &nor_ops);
	blob_busy_release();
	r->status = NOR_SVC_OK;
}

const struct nor_svc_board nor_svc_board = {
	.part          = "N25Q128A on QUADSPI",
	.size          = QSPI_FLASH_SIZE,
	.erase_unit    = QSPI_FLASH_SUBSECTOR_SIZE,
	.page          = QSPI_FLASH_PAGE_SIZE,
	.erase_cancels = 1u,
	.info          = nor_board_info,
	.range         = nor_board_range,
	.hold          = nor_board_hold,
};
