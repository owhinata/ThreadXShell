/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    cmd_qspi.c
 * @brief   `qspi` shell command (owhinata/stm32f746g-disco#29): QSPI NOR flash
 * bring-up/debug.
 *
 *   qspi probe             read the JEDEC ID (expect 20 BA 18)
 *   qspi info              geometry, clock, status register, controller
 *                          registers and the driver's failure record
 *   qspi read <addr> [len] hexdump flash content (indirect read, no 0x90000000)
 *   qspi erase <addr>      erase the 4 KB subsector containing <addr>   (danger)
 *   qspi test <addr>       destructive self-test on one 4 KB subsector  (danger)
 *
 * `qspi info` also prints the controller's registers as they were when it was
 * typed and after its own status read, and the driver's failure record (count,
 * first and last; issue #194).  The driver recovers by itself; there is no
 * manual recover.
 *
 * `qspi test` is the Phase A hardware acceptance: erase -> blank check ->
 * program a counting pattern page by page -> read back and verify.  It polls
 * cli_cancel_requested() between pages so Ctrl+C aborts promptly (the subsector
 * is left partially programmed -- it is a scratch area by contract).
 *
 * erase/test rewrite flash content, so they compile in only with
 * CLI_ENABLE_DANGEROUS_CMDS, like devmem.  They used to be refused while the
 * QSPI FileX/LevelX volume was mounted; since issue #194 the QSPI NOR is the
 * blob store, and they take the store's busy flag (src/blob.h) for as long as
 * they run -- so they are refused while a blob operation, or another erase/test
 * from the other console or a background job, has it.
 *
 * Clean-room design; no third-party code reused.
 */
#include "blob.h"            /* blob_busy_acquire / _release */
#include "cli.h"
#include "qspi_flash.h"

#include "stm32f7xx_hal.h"   /* HAL_GetTick for the self-test timing */

#include <stdint.h>
#include <string.h>

/* Upper bound on one `qspi read` hexdump, mirroring devmem's dump cap. */
#define QSPI_READ_MAX_LEN 256u

static const char *qspi_strerror(int rc)
{
	switch (rc) {
	case QSPI_FLASH_ERR_PARAM:   return "address/length out of range";
	case QSPI_FLASH_ERR_HAL:     return "QUADSPI transfer failed";
	case QSPI_FLASH_ERR_TIMEOUT: return "flash busy timeout";
	case QSPI_FLASH_ERR_FLASH:   return "flash reported program/erase error";
	case QSPI_FLASH_ERR_STATE:   return "driver not initialized";
	default:                     return "unknown error";
	}
}

/* Strict 32-bit parse: 0x-prefixed hex or decimal (same contract as devmem). */
static int parse_u32(const char *s, uint32_t *out)
{
	uint32_t base = 10;
	uint32_t val = 0;
	const char *p = s;

	if (p == NULL || *p == '\0')
		return -1;
	if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
		base = 16;
		p += 2;
		if (*p == '\0')
			return -1;
	}
	for (; *p != '\0'; p++) {
		uint32_t digit;
		char c = *p;

		if (c >= '0' && c <= '9')
			digit = (uint32_t)(c - '0');
		else if (base == 16 && c >= 'a' && c <= 'f')
			digit = (uint32_t)(c - 'a' + 10);
		else if (base == 16 && c >= 'A' && c <= 'F')
			digit = (uint32_t)(c - 'A' + 10);
		else
			return -1;
		if (val > (0xFFFFFFFFu - digit) / base)
			return -1;
		val = val * base + digit;
	}
	*out = val;
	return 0;
}

static int cmd_qspi_probe(struct cli_instance *sh, int argc, char **argv)
{
	uint8_t id[3];
	int rc;

	(void)argc; (void)argv;

	rc = qspi_flash_read_id(id);
	if (rc != 0) {
		cli_error(sh, "qspi: read id failed: %s\r\n", qspi_strerror(rc));
		return 1;
	}
	cli_print(sh, "JEDEC ID: %02X %02X %02X", id[0], id[1], id[2]);
	if (id[0] == 0x20 && id[1] == 0xBA && id[2] == 0x18)
		cli_print(sh, "  (Micron N25Q128A/MT25QL128, 16 MiB)\r\n");
	else
		cli_print(sh, "  (unexpected -- check wiring/AF)\r\n");
	return 0;
}

/* ---- controller registers and the failure record (issue #194) ---------- */

/* SR: BUSY bit 5, FLEVEL bits 13:8, TCF bit 1, FTF bit 2, TEF bit 0.
 * CR: ABORT bit 1.
 * CCR: FMODE bits 27:26 (0 ind-write, 1 ind-read, 2 polling, 3 mapped),
 * DMODE bits 25:24 (0 = no data phase). */
static void print_regs(struct cli_instance *sh, const char *label,
                       const struct qspi_flash_regs *r)
{
	cli_print(sh, "%s: CR %08lx (abort %lu) SR %08lx (busy %lu flevel %lu "
	              "tcf %lu ftf %lu tef %lu)\r\n",
	          label, (unsigned long)r->cr,
	          (unsigned long)((r->cr >> 1) & 1u),
	          (unsigned long)r->sr,
	          (unsigned long)((r->sr >> 5) & 1u),
	          (unsigned long)((r->sr >> 8) & 0x3Fu),
	          (unsigned long)((r->sr >> 1) & 1u),
	          (unsigned long)((r->sr >> 2) & 1u),
	          (unsigned long)(r->sr & 1u));
	cli_print(sh, "            CCR %08lx (fmode %lu dmode %lu) DLR %08lx "
	              "AR %08lx\r\n",
	          (unsigned long)r->ccr,
	          (unsigned long)((r->ccr >> 26) & 3u),
	          (unsigned long)((r->ccr >> 24) & 3u),
	          (unsigned long)r->dlr, (unsigned long)r->ar);
}

static void print_live(struct cli_instance *sh, const char *label)
{
	struct qspi_flash_regs r;
	uint32_t state, error;

	qspi_flash_regs_now(&r, &state, &error);
	print_regs(sh, label, &r);
	cli_print(sh, "            handle state 0x%02lx error 0x%lx, t %lu ms\r\n",
	          (unsigned long)state,
	          (unsigned long)error, (unsigned long)HAL_GetTick());
}

static void print_fail(struct cli_instance *sh, const char *label,
                       const struct qspi_flash_fail *f)
{
	cli_print(sh, "%s: t %lu ms, %s-%s, hal %d, state 0x%02lx error 0x%lx\r\n",
	          label, (unsigned long)f->tick, f->op, f->phase, f->hal_rc,
	          (unsigned long)f->state, (unsigned long)f->error);
	print_regs(sh, "  regs    ", &f->regs);
	cli_print(sh, "            recovery %s, handle state 0x%02lx\r\n",
	          f->recovered ? "ok" : "FAILED (handle set to error)",
	          (unsigned long)f->state_after);
	print_regs(sh, "  after   ", &f->after);
}

static void print_record(struct cli_instance *sh)
{
	struct qspi_flash_record d;

	qspi_flash_record_get(&d);
	cli_print(sh, "failures  : %lu\r\n", (unsigned long)d.fail_count);
	if (d.fail_count > 0) {
		print_fail(sh, "first fail", &d.first);
		print_fail(sh, "last fail ", &d.last);
	}
}

static int cmd_qspi_info(struct cli_instance *sh, int argc, char **argv)
{
	const struct qspi_flash_info *fi = qspi_flash_get_info();
	uint8_t sr;
	int rc;

	(void)argc; (void)argv;

	/* [!] Before any command of our own, so what is printed is what the
	 * controller looked like when the operator typed this. */
	print_live(sh, "before    ");

	cli_print(sh, "size      : %lu KiB\r\n", (unsigned long)(fi->size / 1024u));
	cli_print(sh, "sector    : %lu KiB (erase 0xD8)\r\n",
	          (unsigned long)(fi->sector_size / 1024u));
	cli_print(sh, "subsector : %lu KiB (erase 0x20)\r\n",
	          (unsigned long)(fi->subsector_size / 1024u));
	cli_print(sh, "page      : %lu B (program 0x02)\r\n",
	          (unsigned long)fi->page_size);
	cli_print(sh, "sclk      : %lu MHz, indirect mode\r\n",
	          (unsigned long)(fi->sclk_hz / 1000000u));
	cli_print(sh, "read      : %s\r\n",
	          qspi_flash_quad_enabled()
	                  ? "quad output 0x6B (1-1-4, dummy 10)"
	                  : "fast read 0x0B (1-1-1, dummy 8)");

	rc = qspi_flash_read_status(&sr);
	if (rc != 0)
		cli_error(sh, "qspi: read status failed: %s\r\n", qspi_strerror(rc));
	else
		cli_print(sh, "status    : 0x%02x%s\r\n", sr,
		          (sr & 0x01u) ? " (write in progress)" : "");
	print_live(sh, "after     ");
	print_record(sh);
	return rc != 0 ? 1 : 0;
}

static int cmd_qspi_read(struct cli_instance *sh, int argc, char **argv)
{
	uint8_t buf[QSPI_READ_MAX_LEN];
	uint32_t addr, len = 64;
	int rc;

	if (parse_u32(argv[1], &addr) != 0) {
		cli_error(sh, "qspi: bad address '%s'\r\n", argv[1]);
		return 1;
	}
	if (argc >= 3 && parse_u32(argv[2], &len) != 0) {
		cli_error(sh, "qspi: bad length '%s'\r\n", argv[2]);
		return 1;
	}
	if (len == 0)
		return 0;
	if (len > QSPI_READ_MAX_LEN) {
		cli_error(sh, "qspi: length %lu exceeds max %u\r\n",
		          (unsigned long)len, (unsigned)QSPI_READ_MAX_LEN);
		return 1;
	}

	rc = qspi_flash_read(addr, buf, len);
	if (rc != 0) {
		cli_error(sh, "qspi: read failed: %s\r\n", qspi_strerror(rc));
		return 1;
	}
	return cli_hexdump_base(sh, buf, len, addr) == 0 ? 0 : 1;
}

#if CLI_ENABLE_DANGEROUS_CMDS

/* Parse + gate a destructive target: must be subsector-aligned and in range. */
static int parse_subsector(struct cli_instance *sh, const char *s, uint32_t *addr)
{
	if (parse_u32(s, addr) != 0) {
		cli_error(sh, "qspi: bad address '%s'\r\n", s);
		return -1;
	}
	if (*addr >= QSPI_FLASH_SIZE) {
		cli_error(sh, "qspi: 0x%08lx beyond flash end\r\n", (unsigned long)*addr);
		return -1;
	}
	if (*addr % QSPI_FLASH_SUBSECTOR_SIZE != 0) {
		cli_error(sh, "qspi: 0x%08lx not 4 KB aligned\r\n", (unsigned long)*addr);
		return -1;
	}
	return 0;
}

/* The store's busy flag, for the whole of one erase or test.  Without it two
 * consoles could each run one on the same subsector, and a raw erase could land
 * between a blob operation's header read and its use of it. */
static int qspi_busy_gate(struct cli_instance *sh)
{
	if (blob_busy_acquire() != BLOB_OK) {
		cli_error(sh, "qspi: busy -- " BLOB_BUSY_HOLDERS " is running; "
		              "try again\r\n");
		return -1;
	}
	return 0;
}

static int cmd_qspi_erase(struct cli_instance *sh, int argc, char **argv)
{
	uint32_t addr;
	int rc;

	(void)argc;

	if (parse_subsector(sh, argv[1], &addr) != 0)
		return 1;
	if (qspi_busy_gate(sh) != 0)
		return 1;
	rc = qspi_flash_erase_subsector(addr);
	blob_busy_release();
	if (rc != 0) {
		cli_error(sh, "qspi: erase failed: %s\r\n", qspi_strerror(rc));
		return 1;
	}
	cli_print(sh, "erased 4 KB at 0x%08lx\r\n", (unsigned long)addr);
	return 0;
}

/*
 * Destructive self-test on one 4 KB subsector: erase -> blank check -> program
 * a counting pattern (page-indexed so a misdirected page is caught) -> read
 * back -> verify.  One 256 B working buffer; cancel polled between pages.
 */
static int qspi_test_run(struct cli_instance *sh, uint32_t addr)
{
	uint8_t buf[QSPI_FLASH_PAGE_SIZE];
	uint32_t off, i, t0;
	int rc;

	cli_print(sh, "erase 4 KB at 0x%08lx...\r\n", (unsigned long)addr);
	rc = qspi_flash_erase_subsector(addr);
	if (rc != 0) {
		cli_error(sh, "qspi: erase failed: %s\r\n", qspi_strerror(rc));
		return 1;
	}

	for (off = 0; off < QSPI_FLASH_SUBSECTOR_SIZE; off += sizeof buf) {
		if (cli_cancel_requested(sh))
			return 1;
		rc = qspi_flash_read(addr + off, buf, sizeof buf);
		if (rc != 0) {
			cli_error(sh, "qspi: read failed: %s\r\n", qspi_strerror(rc));
			return 1;
		}
		for (i = 0; i < sizeof buf; i++) {
			if (buf[i] != 0xFF) {
				cli_error(sh, "FAIL: not blank at 0x%08lx (0x%02x)\r\n",
				          (unsigned long)(addr + off + i), buf[i]);
				return 1;
			}
		}
	}
	cli_print(sh, "blank check ok, programming pattern...\r\n");

	for (off = 0; off < QSPI_FLASH_SUBSECTOR_SIZE; off += sizeof buf) {
		if (cli_cancel_requested(sh))
			return 1;
		for (i = 0; i < sizeof buf; i++)
			buf[i] = (uint8_t)(i ^ (off >> 8));
		rc = qspi_flash_write(addr + off, buf, sizeof buf);
		if (rc != 0) {
			cli_error(sh, "qspi: program failed at 0x%08lx: %s\r\n",
			          (unsigned long)(addr + off), qspi_strerror(rc));
			return 1;
		}
	}

	t0 = HAL_GetTick();
	for (off = 0; off < QSPI_FLASH_SUBSECTOR_SIZE; off += sizeof buf) {
		if (cli_cancel_requested(sh))
			return 1;
		rc = qspi_flash_read(addr + off, buf, sizeof buf);
		if (rc != 0) {
			cli_error(sh, "qspi: read failed: %s\r\n", qspi_strerror(rc));
			return 1;
		}
		for (i = 0; i < sizeof buf; i++) {
			uint8_t want = (uint8_t)(i ^ (off >> 8));

			if (buf[i] != want) {
				cli_error(sh, "FAIL: 0x%08lx read 0x%02x want 0x%02x\r\n",
				          (unsigned long)(addr + off + i), buf[i], want);
				return 1;
			}
		}
	}
	cli_print(sh, "read+verify 4 KB in %lu ms (%s)\r\n",
	          (unsigned long)(HAL_GetTick() - t0),
	          qspi_flash_quad_enabled() ? "quad 0x6B" : "1-line 0x0B");

	cli_print(sh, "PASS: erase/program/verify 4 KB at 0x%08lx\r\n",
	          (unsigned long)addr);
	return 0;
}

static int cmd_qspi_test(struct cli_instance *sh, int argc, char **argv)
{
	uint32_t addr;
	int rc;

	(void)argc;

	if (parse_subsector(sh, argv[1], &addr) != 0)
		return 1;
	if (qspi_busy_gate(sh) != 0)
		return 1;
	rc = qspi_test_run(sh, addr);
	blob_busy_release();
	return rc;
}

#endif /* CLI_ENABLE_DANGEROUS_CMDS */

CLI_SUBCMD_SET_CREATE(qspi_subcmds,
	CLI_CMD_ARG(probe, NULL, "read JEDEC ID",                cmd_qspi_probe, 1, 0),
	CLI_CMD_ARG(info,  NULL, "geometry/clock/status",        cmd_qspi_info,  1, 0),
	CLI_CMD_ARG_USAGE(read, NULL, "hexdump <addr> [len]",
	                  "<addr> [len]", cmd_qspi_read, 2, 1),
#if CLI_ENABLE_DANGEROUS_CMDS
	CLI_CMD_ARG_USAGE(erase, NULL, "erase 4KB subsector <addr>",
	                  "<addr>", cmd_qspi_erase, 2, 0),
	CLI_CMD_ARG_USAGE(test, NULL, "destructive self-test <addr>",
	                  "<addr>", cmd_qspi_test, 2, 0),
#endif
	CLI_SUBCMD_SET_END);

CLI_CMD_REGISTER(qspi, qspi_subcmds,
                 "QSPI NOR flash (N25Q128A, 16 MiB)", NULL, 1, 0);
