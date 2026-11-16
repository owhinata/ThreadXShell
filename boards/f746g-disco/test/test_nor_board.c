/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Issue #250 -- the shared `nor` on this board: shell/cmds/cmd_nor.c,
 * shell/cmds/nor_console.c and this board's table (cmds/cmd_nor_board.c),
 * compiled unchanged and reached through the root command table, over a 16 MB
 * array for the QSPI driver and a counted busy flag.
 *
 * What it asks:
 *   - an erase is 4 KB subsector erases of exactly the range, in order, and
 *     never the 64 KB sector erase or the chip erase; bytes outside the range
 *     are untouched; Ctrl+C after a subsector stops it there;
 *   - the kv partition (port/flashdb/fal_cfg.h, the real one) is refused by
 *     its range, before the flag; telnet, a job launched from it and a job of
 *     the VCP console are refused before the flag;
 *   - the flag: taken once and given back once on every run that took it;
 *     with the flag held elsewhere, one failed take and no driver call; with
 *     the driver down, no take;
 *   - [!] no erase, program or read of the test is made while the flag is not
 *     held, in any run (issue #249's lesson);
 *   - `nor test` passes on a blank subsector and leaves it blank, refuses one
 *     that is not blank before erasing, and every failure gives the flag back;
 *   - there is no `nor write` here, and asking for one calls nothing;
 *   - `nor info` samples the controller's registers BEFORE it sends the read
 *     id or the status read;
 *   - `nor probe` (the board's own) is in the table, and no name is twice.
 */
#include "cli.h"
#include "cli_instance.h"
#include "net_tty.h"
#include "blob.h"
#include "fal_cfg.h"
#include "log.h"
#include "qspi_flash.h"
#include "stm32f7xx_hal.h"
#include "tx_api.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- the driver ----------------------------------------------------------- */

static uint8_t *dev;
static int ready = 1, flag_held, flag_other, takes, take_fails, gives;
static int n_sub, n_sector, n_chip, n_write, n_read, n_unheld, n_id, n_status;
static int fail_sub_at, seq, seq_regs_first, seq_id;
static uint32_t sub_addr[64];

int qspi_flash_ready(void) { return ready; }
int qspi_flash_read_id(uint8_t id[3])
{
	n_id++;
	seq_id = ++seq;
	id[0] = 0x20; id[1] = 0xBA; id[2] = 0x18;
	return 0;
}
int qspi_flash_read(uint32_t addr, void *buf, uint32_t len)
{
	n_read++;
	if (!flag_held)
		n_unheld++;
	memcpy(buf, dev + addr, len);
	return 0;
}
int qspi_flash_write(uint32_t addr, const void *buf, uint32_t len)
{
	n_write++;
	if (!flag_held)
		n_unheld++;
	for (uint32_t i = 0; i < len; i++)
		dev[addr + i] &= ((const uint8_t *)buf)[i];
	return 0;
}
int qspi_flash_erase_subsector(uint32_t addr)
{
	if (!flag_held)
		n_unheld++;
	if (n_sub < 64)
		sub_addr[n_sub] = addr;
	n_sub++;
	if (fail_sub_at == n_sub)
		return QSPI_FLASH_ERR_FLASH;
	memset(dev + addr, 0xFF, QSPI_FLASH_SUBSECTOR_SIZE);
	return 0;
}
int qspi_flash_erase_sector(uint32_t addr) { (void)addr; n_sector++; return 0; }
int qspi_flash_erase_chip(void) { n_chip++; return 0; }
int qspi_flash_quad_enabled(void) { return 1; }
int qspi_flash_read_status(uint8_t *sr) { n_status++; *sr = 0; return 0; }
const struct qspi_flash_info *qspi_flash_get_info(void)
{
	static const struct qspi_flash_info fi = { QSPI_FLASH_SIZE, 0x10000u, 0x1000u,
	                                           0x100u, 108000000u };
	return &fi;
}
void qspi_flash_regs_now(struct qspi_flash_regs *r, uint32_t *state, uint32_t *error)
{
	if (seq_regs_first == 0)
		seq_regs_first = ++seq;
	memset(r, 0, sizeof *r);
	*state = 0x20;
	*error = 0;
}
void qspi_flash_record_get(struct qspi_flash_record *out) { memset(out, 0, sizeof *out); }
uint32_t HAL_GetTick(void) { return 0; }

int blob_busy_acquire(void)
{
	takes++;
	if (flag_held || flag_other) {
		take_fails++;
		return BLOB_ERR_BUSY;
	}
	flag_held = 1;
	return BLOB_OK;
}
void blob_busy_release(void) { gives++; flag_held = 0; }

/* ---- the console ---------------------------------------------------------- */

static struct cli_instance vcp, telnet;
static struct cli_instance vcp_job = { .fg = &vcp };
static struct cli_instance telnet_job = { .fg = &telnet };

bool net_tty_is_console(const struct cli_instance *sh)
{
	const struct cli_instance *owner = (sh->fg != NULL) ? sh->fg : sh;

	return owner == &telnet;
}
const struct net_tty_facts net_tty_board_facts = { .other_console = "VCP" };

static char text[32768];
static size_t text_len;
static int cancel_at, cancel_asks;

static int out(const char *fmt, va_list ap)
{
	int n = vsnprintf(text + text_len, sizeof text - text_len, fmt, ap);

	if (n > 0)
		text_len += ((size_t)n < sizeof text - text_len) ? (size_t)n
		                                                 : sizeof text - text_len - 1;
	return n;
}
int cli_print(struct cli_instance *sh, const char *f, ...)
{ va_list ap; int n; (void)sh; va_start(ap, f); n = out(f, ap); va_end(ap); return n; }
int cli_error(struct cli_instance *sh, const char *f, ...)
{ va_list ap; int n; (void)sh; va_start(ap, f); n = out(f, ap); va_end(ap); return n; }
int cli_write(struct cli_instance *sh, const void *d, size_t n)
{
	(void)sh;
	if (n < sizeof text - text_len) {
		memcpy(text + text_len, d, n);
		text_len += n;
		text[text_len] = '\0';
	}
	return (int)n;
}
int cli_hexdump_base(struct cli_instance *sh, const void *d, size_t n,
                     unsigned long long b)
{ (void)sh; (void)d; (void)b; return (int)n; }
bool cli_cancel_requested(struct cli_instance *sh)
{
	(void)sh;
	return cancel_at != 0 && ++cancel_asks == cancel_at;
}
void log_write(unsigned level, const char *tag, const char *fmt, ...)
{ (void)level; (void)tag; (void)fmt; }
ULONG tx_time_get(void) { return 0; }

/* ---- the harness ---------------------------------------------------------- */

static int failures;

static void check(int ok, const char *what, const char *line)
{
	if (!ok) {
		printf("FAIL: %s -- `%s`\n  (sub %d sector %d chip %d write %d read %d, "
		       "takes %d fails %d gives %d unheld %d)\n%s\n", what, line, n_sub,
		       n_sector, n_chip, n_write, n_read, takes, take_fails, gives,
		       n_unheld, text);
		failures++;
	}
}

static const struct cli_cmd *nor_table(void)
{
	CLI_ROOT_CMD_FOREACH(c)
		if (strcmp(c->name, "nor") == 0)
			return c->subcmds;
	return NULL;
}

static const struct cli_cmd *nor_sub(const char *name)
{
	for (const struct cli_cmd *s = nor_table(); s && s->name; s++)
		if (strcmp(s->name, name) == 0)
			return s;
	return NULL;
}

static void reset(void)
{
	ready = 1;
	flag_held = flag_other = takes = take_fails = gives = 0;
	n_sub = n_sector = n_chip = n_write = n_read = n_unheld = n_id = n_status = 0;
	fail_sub_at = seq = seq_regs_first = seq_id = 0;
	cancel_at = cancel_asks = 0;
	text_len = 0;
	text[0] = '\0';
}

static int has(const char *s) { return strstr(text, s) != NULL; }
static int driver_calls(void) { return n_sub + n_sector + n_chip + n_write + n_read; }

static int run(struct cli_instance *sh, const char *line)
{
	static char buf[128];
	char *argv[8];
	int argc = 0, rc;
	const struct cli_cmd *s;
	char *p;

	snprintf(buf, sizeof buf, "%s", line);
	for (p = strtok(buf, " "); p != NULL && argc < 8; p = strtok(NULL, " "))
		argv[argc++] = p;
	s = nor_sub(argv[0]);
	if (s == NULL)
		return -100;
	rc = s->handler(sh, argc, argv);
	if (strcmp(argv[0], "read") != 0)
		check(n_unheld == 0, "no driver erase, program or read without the flag",
		      line);
	check(gives == takes - take_fails && !flag_held,
	      "the flag given back once for every take", line);
	check(n_sector == 0 && n_chip == 0, "never the 64 KB or the chip erase", line);
	return rc;
}

static void refused_early(struct cli_instance *sh, const char *line, const char *why)
{
	reset();
	check(run(sh, line) == 1, "refused", line);
	check(takes == 0 && driver_calls() == 0, "refused before the flag", line);
	check(has(why), "the reason is said", line);
}

int main(void)
{
	int dups = 0, rc;

	dev = malloc(QSPI_FLASH_SIZE);
	if (dev == NULL)
		return 2;
	memset(dev, 0xFF, QSPI_FLASH_SIZE);

	for (const struct cli_cmd *s = nor_table(); s && s->name; s++)
		for (const struct cli_cmd *t = nor_table(); t < s; t++)
			dups += strcmp(s->name, t->name) == 0;
	check(nor_table() != NULL && dups == 0 && nor_sub("probe") != NULL,
	      "`nor probe` is in the table, no name twice", "nor");
	reset();
	check(run(&vcp, "probe") == 0 && n_id == 1 && has("Micron N25Q128A"), "probe",
	      "probe");

	/* ---- info: the registers before anything is sent ---- */
	reset();
	check(run(&vcp, "info") == 0 && seq_regs_first != 0 && seq_regs_first < seq_id,
	      "the registers are sampled before the read id", "info");
	check(has("NOR: N25Q128A on QUADSPI") && has("  jedec   : 20 BA 18\r\n") &&
	      has("before ") && has("failures: 0"), "the shared lines and the board's",
	      "info");

	/* ---- erase: 4 KB subsectors of exactly the range ---- */
	reset();
	memset(dev + 0x7FF000, 0x00, 0x1000);
	memset(dev + 0x810000, 0x00, 0x1000);
	rc = run(&vcp, "erase 0x800000 0x10000");
	check(rc == 0 && n_sub == 16 && sub_addr[0] == 0x800000u &&
	      sub_addr[15] == 0x80F000u, "sixteen subsectors, in order", "erase 64 KB");
	check(dev[0x7FFFFF] == 0x00 && dev[0x810000] == 0x00,
	      "the bytes on either side are untouched", "erase 64 KB");
	check(takes == 1 && gives == 1 && has("Ctrl+C stops it"), "the flag once; Ctrl+C",
	      "erase 64 KB");
	reset();
	cancel_at = 2;
	rc = run(&vcp, "erase 0x800000 0x10000");
	check(rc == 1 && n_sub == 2 && has("cancelled after 8192 B of 65536"),
	      "Ctrl+C after the second subsector stops there", "erase cancel");
	reset();
	fail_sub_at = 3;
	rc = run(&vcp, "erase 0x800000 0x10000");
	check(rc == 1 && n_sub == 3 && has("erase failed: flash reported program/erase error"),
	      "a failed subsector stops the erase", "erase fail");

	/* ---- the kv partition, the console, the flag ---- */
	refused_early(&vcp, "erase 0xFF000 0x2000", "is in the kv store (0x000000 .. 0x0FFFFF)");
	refused_early(&vcp, "erase 0 4096", "`kv format yes` erases it");
	refused_early(&vcp, "test 0xFF000", "is in the kv store");
	refused_early(&vcp, "erase 0x800800 4096", "whole 4 KB sectors");
	refused_early(&vcp, "erase 0xFFF000 0x2000", "past the end");
	refused_early(&vcp, "read 0xFFFFFF 2", "past the end");
	{
		static const char *const lines[] = { "erase 0x800000 4096", "test 0x800000" };

		for (unsigned i = 0; i < 2; i++) {
			refused_early(&telnet, lines[i],
			              "foreground of the VCP console; this is the telnet console");
			refused_early(&telnet_job, lines[i],
			              "this is a background job of the telnet console");
			refused_early(&vcp_job, lines[i], "this is a background job");
			reset();
			ready = 0;
			check(run(&vcp, lines[i]) == 1 && takes == 0 && driver_calls() == 0 &&
			      has("driver not initialized"), "down: no take", lines[i]);
			reset();
			flag_other = 1;
			check(run(&vcp, lines[i]) == 1 && takes == 1 && take_fails == 1 &&
			      driver_calls() == 0 && has(BLOB_BUSY_HOLDERS),
			      "busy: one failed take, the holders named", lines[i]);
		}
	}
	check(strstr(BLOB_BUSY_HOLDERS, "`nor erase` / `nor test`") != NULL &&
	      strstr(BLOB_BUSY_HOLDERS, "qspi") == NULL, "the holders say `nor`", "blob.h");

	/* ---- no `nor write` ---- */
	reset();
	check(run(&vcp, "write 0x800000 00") == 1 && takes == 0 && driver_calls() == 0 &&
	      has("no `nor write` on this board\r\n"), "no write, nothing called", "write");

	/* ---- test ---- */
	reset();
	memset(dev + 0x800000, 0xFF, 0x1000);
	dev[0x800010] = 0x00;
	check(run(&vcp, "test 0x800000") == 1 && n_sub == 0 && n_write == 0 &&
	      dev[0x800010] == 0x00 && has("is not blank"), "not blank: nothing erased",
	      "test");
	reset();
	memset(dev + 0x800000, 0xFF, 0x1000);
	check(run(&vcp, "test 0x800000") == 0 && has("nor: PASS") && n_sub == 3 &&
	      has("overwrite:"), "a pass, three subsector erases, the overwrite seen",
	      "test");
	for (int i = 0; i < 0x1000; i++)
		if (dev[0x800000 + i] != 0xFF) {
			check(0, "left blank", "test");
			break;
		}
	for (int k = 1; k <= 3; k++) {
		reset();
		memset(dev + 0x800000, 0xFF, 0x1000);
		fail_sub_at = k;
		check(run(&vcp, "test 0x800000") == 1 && !has("PASS") &&
		      has("`nor erase 0x800000 4096` before the next"),
		      "a failed erase: no PASS, the way out", "test");
	}

	free(dev);
	printf("test_nor_board (f746g-disco): %s\n", failures ? "FAILED" : "OK");
	return failures ? 1 : 0;
}
