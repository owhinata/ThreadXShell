/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Issues #249, #250 -- what guards `nor erase` / `nor write`, and that this
 * board has no `nor test` (issue #250, the user's decision, for the internal
 * flash: the test's code is not compiled here).
 *
 * The shared shell/cmds/cmd_nor.c and this board's table (cmds/cmd_nor_board.c)
 * are compiled unchanged and reached through the `nor` entry of the root command
 * table.  This file stands in for what they call: the NOR driver (a
 * 16 MB array that erases to 0xFF and programs by clearing bits, every call
 * counted and any one of them made to fail), the blob store's busy flag
 * (counted, and holdable by "somebody else"), the telnet question and the
 * console output (kept as text, so a refusal's reason can be looked for).
 *
 * What it asks:
 *   - a refusal BEFORE the flag (the arguments, the kv partition, telnet, a job
 *     launched from telnet, a job of the USB console, the NOR down) touches the
 *     NOR zero times and the flag zero times, and says why;
 *   - with the flag held elsewhere: one failed take, no give-back, no NOR call;
 *   - each accepted run gives the flag back once;
 *   - in every run of erase / write, no NOR erase, program or read is made
 *     while the flag is not held (`nor read` runs without it);
 *   - a failed `nor erase` / `nor write` gives the flag back once;
 *   - `nor read` refuses while the NOR is down, and past the end;
 *   - `nor test` answers that this board has none and calls nothing;
 *   - the edge of the kv partition: 0xFF000 refused, 0x100000 accepted, 8 KB at
 *     0xFF000 refused, a length near 2^32 refused.
 */
#include "cli.h"
#include "cli_instance.h"
#include "net_tty.h"
#include "nor_flash.h"
#include "blob.h"
#include "blob_err.h"
#include "fal_cfg.h"
#include "log.h"
#include "tx_api.h"

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

/* Ctrl+C: the cancel_at-th question is answered yes (0: never). */
static int cancel_at, cancel_asks;
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
	cancel_at = cancel_asks = 0;
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
		"erase 0x800000 4096", "write 0x800000 00",
	};
	int rc, o;
	size_t i;

	dev = malloc(NOR_SIZE_BYTES);
	if (dev == NULL)
		return 2;
	memset(dev, 0xFF, NOR_SIZE_BYTES);

	/* -- the kv partition, asked of the range actually erased or written -- */
	refused_early(&usb, "erase 0 4096", "hits the kv store", "kv: erase 0");
	refused_early(&usb, "erase 0xFF000 4096", "hits the kv store", "kv: last sector");
	refused_early(&usb, "erase 0xFF000 0x2000", "hits the kv store", "kv: 8 KB across the end");
	refused_early(&usb, "write 0x10 00", "hits the kv store", "kv: write");
	refused_early(&usb, "write 0xFFFFF 0000", "hits the kv store", "kv: write across the end");
	refused_early(&usb, "erase 0x100000 0xFFFFF000", "past the end", "length near 2^32");
	refused_early(&usb, "erase 0 0xFFFFF000", "past the end", "length near 2^32 at 0");
	check(has("past the end") && !has("kv store"), "range first, then kv",
	      "erase 0 0xFFFFF000");
	/* -- nothing is rounded: an erase or test names whole 4 KB sectors -- */
	refused_early(&usb, "erase 0x100001 4096", "4 KB multiples", "unaligned address");
	refused_early(&usb, "erase 0x100000 4095", "4 KB multiples", "unaligned length");
	refused_early(&usb, "erase 0x100000 0", "4 KB multiples", "length 0");
	refused_early(&usb, "erase 0x1000000 4096", "bad address", "address off the device");
	refused_early(&usb, "write 0xFFFFFF 0000", "past the end", "write past the end");
	refused_early(&usb, "write 0x100000 0", "bad hex", "odd hex");

	reset();
	rc = run(&usb, "erase 0x100000 4096");
	check(rc == 0 && calls[OP_ERASE] == 1 && last_erase_addr == 0x100000u &&
	      last_erase_len == NOR_SECTOR_SIZE, "0x100000 is erased", "erase 0x100000");
	check(takes == 1 && gives == 1 && !flag_held, "flag taken and given back once",
	      "erase 0x100000");
	check(!has("Ctrl+C"), "one driver call: no Ctrl+C offered", "erase 0x100000");
	reset();
	rc = run(&usb, "erase 0x100000 0x10000");
	check(rc == 0 && calls[OP_ERASE] == 1 && last_erase_len == 0x10000u,
	      "a 64 KB erase is one call of exactly the range", "erase 0x100000 0x10000");
	reset();
	rc = run(&usb, "write 0x100000 A55A");
	check(rc == 0 && calls[OP_WRITE] == 1 && dev[0x100000] == 0xA5u,
	      "0x100000 is written", "write 0x100000 A55A");
	check(takes == 1 && gives == 1 && !flag_held, "flag taken and given back once",
	      "write 0x100000 A55A");
	blank(0x100000, NOR_SECTOR_SIZE);
	for (o = OP_ERASE; o <= OP_WRITE; o++) {
		const char *line = (o == OP_ERASE) ? "erase 0x100000 4096" : "write 0x100000 A55A";

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
	reset();
	rc = run(&usb, "read 0xFFFFFF 2");
	check(rc == 1 && calls[OP_READ] == 0 && has("past the end"),
	      "read past the end refused, not cut", "read 0xFFFFFF 2");
	reset();
	rc = run(&usb, "read 0 1025");
	check(rc == 1 && calls[OP_READ] == 0 && has("1..1024"), "read: 1025 B refused",
	      "read 0 1025");
	reset();
	ready = 0;
	rc = run(&usb, "read 0x100000 16");
	check(rc == 1 && calls[OP_READ] == 0 && has("read failed at 0x100000: device down"),
	      "read: refused while the NOR is down", "read 0x100000 16");
	reset();
	rc = run(&usb, "read 0 1024");
	check(rc == 0 && calls[OP_READ] == 4, "read: 1024 B in 4 pieces", "read 0 1024");

	/* -- the console, the device, the flag: for each of the three -- */
	for (i = 0; i < sizeof ops / sizeof ops[0]; i++) {
		refused_early(&telnet, ops[i], "not telnet\r\n", "telnet");
		refused_early(&telnet_job, ops[i], "not a telnet job\r\n", "telnet job");
		refused_early(&usb_job, ops[i], "not a job\r\n", "usb job");
		check(has("only in the USB CDC foreground"), "names the console", ops[i]);
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

	/* -- no `nor test` on this board: the answer, and nothing called -- */
	reset();
	rc = run(&usb, "test 0x800000");
	check(rc == 1 && nor_calls() == 0 && takes == 0 &&
	      has("nor: no `test` on this board\r\n"), "no test, nothing called", "test");

	free(dev);
	if (failures) {
		printf("test_nor_guard: %d FAILED\n", failures);
		return 1;
	}
	printf("test_nor_guard: OK\n");
	return 0;
}
