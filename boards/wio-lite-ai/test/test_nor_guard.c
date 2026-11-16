/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Issue #249 -- what guards `nor erase` / `nor write` / `nor test`.
 *
 * cmds/cmd_nor.c is compiled unchanged and reached through the `nor` entry of the
 * root command table.  This file stands in for what it calls: the NOR driver (a
 * 16 MB array that erases to 0xFF and programs by clearing bits, every call
 * counted and any one of them made to fail or to do nothing), the blob store's
 * busy flag (counted, and holdable by "somebody else"), the telnet question and
 * the console output (kept as text, so a refusal's reason can be looked for).
 *
 * What it asks:
 *   - a refusal BEFORE the flag (the kv partition, telnet, a job launched from
 *     telnet, a job of the USB console, the NOR down) touches the NOR zero times
 *     and the flag zero times, and says why;
 *   - with the flag held elsewhere: one failed take, no give-back, no NOR call;
 *   - `nor test` on a sector that is not blank: the flag taken and given back
 *     once, no erase and no program, the sector as it was;
 *   - each accepted run gives the flag back once, and `nor test` erases twice,
 *     passes and leaves the sector blank;
 *   - in every run of erase / write / test, no NOR erase, program or read is
 *     made while the flag is not held (`nor read` runs without it);
 *   - a failed `nor erase` / `nor write` gives the flag back once;
 *   - every failure point of `nor test` after the flag (each erase, program and
 *     read failing, the last erase doing nothing, each partial program doing
 *     nothing) ends without PASS and with the flag given back once;
 *   - the edge of the kv partition: 0xFFFFF refused, 0x100000 accepted, 8 KB at
 *     0xFF000 refused, a length near 2^32 refused.
 */
#include "cli.h"
#include "cli_instance.h"
#include "net_tty.h"
#include "nor_flash.h"
#include "blob.h"
#include "blob_err.h"
#include "fal_cfg.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- the fakes ------------------------------------------------------------ */

static uint8_t *dev;                        /* the 16 MB part */
static int ready = 1;

enum op { OP_ERASE, OP_WRITE, OP_READ, OP_N };
static int calls[OP_N];
static int fail_op = -1, fail_at;           /* the fail_at-th call of fail_op fails */
static int noop_op = -1, noop_at;           /* ...or does nothing and says OK */
static uint32_t last_erase_addr, last_erase_len;

/* The busy flag's state, read by every NOR call below: an erase, program or read
 * made while the flag is not held is counted, so "gave the flag back, then
 * touched the NOR" fails even though the take and the give-back balance.
 * `nor read` / `nor info` take no flag and are not held to it (run()). */
static int flag_held;
static int unheld[OP_N];

static int hit(enum op o, int *noop)
{
	calls[o]++;
	if (!flag_held)
		unheld[o]++;
	*noop = (noop_op == (int)o && noop_at == calls[o]);
	return (fail_op == (int)o && fail_at == calls[o]) ? NOR_ERR_IO : NOR_OK;
}

int nor_flash_ready(void) { return ready; }
void nor_flash_get_id(uint8_t id[3]) { id[0] = 0xEF; id[1] = 0x40; id[2] = 0x18; }
uint32_t nor_flash_clock_hz(void) { return 50000000u; }
void nor_flash_get_regs(uint32_t regs[4]) { memset(regs, 0, 4 * sizeof regs[0]); }

int nor_read(uint32_t addr, void *buf, uint32_t len)
{
	int noop, rc = hit(OP_READ, &noop);

	if (rc == NOR_OK && !noop)
		memcpy(buf, dev + addr, len);
	return rc;
}

int nor_write(uint32_t addr, const void *buf, uint32_t len)
{
	int noop, rc = hit(OP_WRITE, &noop);
	uint32_t i;

	if (rc == NOR_OK && !noop)
		for (i = 0; i < len; i++)
			dev[addr + i] &= ((const uint8_t *)buf)[i];
	return rc;
}

int nor_erase(uint32_t addr, uint32_t len)
{
	int noop, rc = hit(OP_ERASE, &noop);

	last_erase_addr = addr;
	last_erase_len = len;
	if (rc == NOR_OK && !noop)
		memset(dev + addr, 0xFF, len);
	return rc;
}

static int flag_other, takes, take_fails, gives;

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

void blob_busy_release(void)
{
	gives++;
	flag_held = 0;
}

static struct cli_instance usb, telnet;
static struct cli_instance usb_job = { .fg = &usb };
static struct cli_instance telnet_job = { .fg = &telnet };

/* As shell/cmds/net_tty.c answers: a job answers for its launcher. */
bool net_tty_is_console(const struct cli_instance *sh)
{
	const struct cli_instance *owner = (sh->fg != NULL) ? sh->fg : sh;

	return owner == &telnet;
}
const struct net_tty_facts net_tty_board_facts = { .other_console = "USB CDC" };

static char text[16384];
static size_t text_len;

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
int cli_hexdump_base(struct cli_instance *sh, const void *d, size_t n,
                     unsigned long long b)
{ (void)sh; (void)d; (void)b; return (int)n; }

/* ---- the harness ---------------------------------------------------------- */

static int failures;

static void check(int ok, const char *what, const char *line)
{
	if (!ok) {
		printf("FAIL: %s -- `%s`\n", what, line);
		printf("  (erase %d write %d read %d, takes %d fails %d gives %d)\n%s\n",
		       calls[OP_ERASE], calls[OP_WRITE], calls[OP_READ], takes,
		       take_fails, gives, text);
		failures++;
	}
}

static const struct cli_cmd *nor_sub(const char *name)
{
	CLI_ROOT_CMD_FOREACH(c) {
		const struct cli_cmd *s;

		if (strcmp(c->name, "nor") != 0)
			continue;
		for (s = c->subcmds; s->name != NULL; s++)
			if (strcmp(s->name, name) == 0)
				return s;
	}
	return NULL;
}

static void reset(void)
{
	memset(calls, 0, sizeof calls);
	memset(unheld, 0, sizeof unheld);
	fail_op = noop_op = -1;
	takes = take_fails = gives = 0;
	flag_held = flag_other = 0;
	ready = 1;
	text_len = 0;
	text[0] = '\0';
}

/* Run "nor <line>" on @p sh; the counters and text are those of this run. */
static int run(struct cli_instance *sh, const char *line)
{
	static char buf[128];
	char *argv[8];
	int argc = 0;
	const struct cli_cmd *s;
	char *p;
	int rc;

	snprintf(buf, sizeof buf, "%s", line);
	for (p = strtok(buf, " "); p != NULL && argc < 8; p = strtok(NULL, " "))
		argv[argc++] = p;
	s = nor_sub(argv[0]);
	if (s == NULL) {
		printf("FAIL: no `nor %s` in the table\n", argv[0]);
		failures++;
		return -1;
	}
	rc = s->handler(sh, argc, argv);
	/* erase / write / test touch the NOR only while they hold the flag: in
	 * every run, accepted, refused or failed */
	if (strcmp(argv[0], "read") != 0 && strcmp(argv[0], "info") != 0)
		check(unheld[OP_ERASE] == 0 && unheld[OP_WRITE] == 0 &&
		      unheld[OP_READ] == 0, "no NOR call without the flag", line);
	return rc;
}

static int has(const char *s) { return strstr(text, s) != NULL; }
static int nor_calls(void) { return calls[OP_ERASE] + calls[OP_WRITE] + calls[OP_READ]; }

static void blank(uint32_t addr, uint32_t len) { memset(dev + addr, 0xFF, len); }
static int is_blank(uint32_t addr, uint32_t len)
{
	uint32_t i;

	for (i = 0; i < len; i++)
		if (dev[addr + i] != 0xFFu)
			return 0;
	return 1;
}

/* A refusal before the flag: no NOR call, no take, the reason printed. */
static void refused_early(struct cli_instance *sh, const char *line, const char *why,
                          const char *what)
{
	int rc;

	reset();
	rc = run(sh, line);
	check(rc == 1, what, line);
	check(nor_calls() == 0, "no NOR call before the flag", line);
	check(takes == 0 && gives == 0, "the flag is not touched", line);
	check(has(why), "the reason is said", line);
	check(!has("PASS"), "no PASS", line);
}

int main(void)
{
	static const char *const ops[] = {
		"erase 0x800000", "write 0x800000 00", "test 0x800000",
	};
	int base[OP_N], rc, k, o;
	size_t i;

	dev = malloc(NOR_SIZE_BYTES);
	if (dev == NULL)
		return 2;
	memset(dev, 0xFF, NOR_SIZE_BYTES);

	/* -- the kv partition, asked of the range actually erased or written -- */
	refused_early(&usb, "erase 0", "in the kv store", "kv: erase 0");
	refused_early(&usb, "erase 0xFFFFF", "in the kv store", "kv: last byte");
	refused_early(&usb, "erase 0xFF000 0x2000", "in the kv store", "kv: 8 KB across the end");
	refused_early(&usb, "write 0x10 00", "in the kv store", "kv: write");
	refused_early(&usb, "write 0xFFFFF 0000", "in the kv store", "kv: write across the end");
	refused_early(&usb, "test 0", "in the kv store", "kv: test");
	refused_early(&usb, "test 0xFFFFF", "in the kv store", "kv: test, last sector");
	refused_early(&usb, "erase 0x100000 0xFFFFFFFF", "past the end", "length near 2^32");
	refused_early(&usb, "erase 0 0xFFFFF001", "past the end", "length near 2^32 at 0");
	check(has("past the end") && !has("kv store"), "range first, then kv",
	      "erase 0 0xFFFFF001");

	reset();
	rc = run(&usb, "erase 0x100000");
	check(rc == 0 && calls[OP_ERASE] == 1 && last_erase_addr == 0x100000u &&
	      last_erase_len == NOR_SECTOR_SIZE, "0x100000 is erased", "erase 0x100000");
	check(takes == 1 && gives == 1 && !flag_held, "flag taken and given back once",
	      "erase 0x100000");
	reset();
	rc = run(&usb, "write 0x100000 A55A");
	check(rc == 0 && calls[OP_WRITE] == 1 && dev[0x100000] == 0xA5u,
	      "0x100000 is written", "write 0x100000 A55A");
	check(takes == 1 && gives == 1 && !flag_held, "flag taken and given back once",
	      "write 0x100000 A55A");
	blank(0x100000, NOR_SECTOR_SIZE);
	for (o = OP_ERASE; o <= OP_WRITE; o++) {
		const char *line = (o == OP_ERASE) ? "erase 0x100000" : "write 0x100000 A55A";

		reset();
		fail_op = o;
		fail_at = 1;
		rc = run(&usb, line);
		check(rc == 1 && has("failed"), "a failed operation is reported", line);
		check(takes == 1 && gives == 1 && !flag_held, "and the flag given back once",
		      line);
	}

	/* `nor read` takes no flag and runs from telnet: the reads are made without it */
	reset();
	rc = run(&telnet, "read 0x100000 16");
	check(rc == 0 && takes == 0 && calls[OP_READ] == 1 && unheld[OP_READ] == 1,
	      "read: from telnet, no flag", "read 0x100000 16");

	/* -- the console, the device, the flag: for each of the three -- */
	for (i = 0; i < sizeof ops / sizeof ops[0]; i++) {
		refused_early(&telnet, ops[i], "this is the telnet console", "telnet");
		refused_early(&telnet_job, ops[i],
		              "this is a background job of the telnet console", "telnet job");
		refused_early(&usb_job, ops[i], "this is a background job", "usb job");
		check(has("foreground of the USB CDC console"), "names the console", ops[i]);
		reset();
		ready = 0;
		rc = run(&usb, ops[i]);
		check(rc == 1 && nor_calls() == 0 && takes == 0 && gives == 0 &&
		      has("device down"), "NOR down: refused before the flag", ops[i]);

		reset();
		flag_other = 1;
		rc = run(&usb, ops[i]);
		check(rc == 1, "busy: refused", ops[i]);
		check(takes == 1 && take_fails == 1 && gives == 0,
		      "busy: one failed take, nothing given back", ops[i]);
		check(nor_calls() == 0, "busy: no NOR call", ops[i]);
		check(has("busy -- ") && has(BLOB_BUSY_HOLDERS), "busy: names who", ops[i]);
	}

	/* -- nor test on a sector that is not blank -- */
	reset();
	blank(0x800000, NOR_SECTOR_SIZE);
	dev[0x800000 + 4000] = 0x12;
	rc = run(&usb, "test");
	check(rc == 1 && has("is not blank") && has("`nor erase 0x800000`"),
	      "not blank: refused with the way out", "test");
	check(calls[OP_ERASE] == 0 && calls[OP_WRITE] == 0, "not blank: nothing erased "
	      "or programmed", "test");
	check(dev[0x800000 + 4000] == 0x12, "not blank: the data is still there", "test");
	check(takes == 1 && gives == 1 && !flag_held, "not blank: flag back once", "test");
	check(!has("PASS"), "not blank: no PASS", "test");

	/* -- nor test that passes -- */
	reset();
	blank(0x800000, NOR_SECTOR_SIZE);
	rc = run(&usb, "test");
	check(rc == 0 && has("nor: PASS"), "test passes", "test");
	check(calls[OP_ERASE] == 2, "test erases twice (the last leaves it blank)", "test");
	check(is_blank(0x800000, NOR_SECTOR_SIZE), "test leaves the sector blank", "test");
	check(takes == 1 && gives == 1 && !flag_held, "test: flag back once", "test");
	memcpy(base, calls, sizeof base);

	/* -- every failure point after the flag -- */
	for (o = 0; o < OP_N; o++) {
		for (k = 1; k <= base[o]; k++) {
			char what[64];

			reset();
			blank(0x800000, NOR_SECTOR_SIZE);
			fail_op = o;
			fail_at = k;
			rc = run(&usb, "test");
			snprintf(what, sizeof what, "%s %d of %d fails",
			         o == OP_ERASE ? "erase" : o == OP_WRITE ? "write" : "read",
			         k, base[o]);
			check(rc == 1 && !has("PASS"), what, "test");
			check(takes == 1 && gives == 1 && !flag_held, "flag back once", what);
		}
	}
	/* The last erase doing nothing leaves the programs in place.  (The first one
	 * doing nothing cannot be told apart: the sector read blank just before.) */
	reset();
	blank(0x800000, NOR_SECTOR_SIZE);
	noop_op = OP_ERASE;
	noop_at = base[OP_ERASE];
	rc = run(&usb, "test");
	check(rc == 1 && !has("PASS") && has("not blank after the final erase"),
	      "a last erase that does nothing is caught", "test");
	check(takes == 1 && gives == 1 && !flag_held, "flag back once", "dead erase");
	check(has("`nor erase 0x800000` before the next"), "says how to clean up", "test");
	for (k = 1; k <= 3; k++) {
		reset();
		blank(0x800000, NOR_SECTOR_SIZE);
		noop_op = OP_WRITE;
		noop_at = k;
		rc = run(&usb, "test");
		check(rc == 1 && !has("PASS") && has("disturbs the page"),
		      "a partial program that does nothing is caught", "test");
		check(takes == 1 && gives == 1 && !flag_held, "flag back once", "dead program");
	}

	free(dev);
	if (failures) {
		printf("test_nor_guard: %d FAILED\n", failures);
		return 1;
	}
	printf("test_nor_guard: OK (%d erases, %d programs, %d reads in a passing test, "
	       "each made to fail)\n", base[OP_ERASE], base[OP_WRITE], base[OP_READ]);
	return 0;
}
