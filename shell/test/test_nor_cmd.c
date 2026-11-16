/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Issue #250 -- the one shared `nor` (shell/cmds/cmd_nor.c), compiled unchanged
 * with shell/cmds/nor_console.c over a fake board table, and reached through the
 * root command table the way a console reaches it.
 *
 * The fake board is a 1 MB array that erases to 0xFF and programs by clearing
 * bits.  Every call of the table is counted, any operation can be made to fail
 * or to do nothing, the hold can be "somebody else's" or the device "down", and
 * -- the lesson of issue #249 -- every erase, program or test read made while
 * the hold is NOT held is counted apart and must be 0 in every run.
 *
 * What it asks, per shape (-DNOR_TEST_SHAPE_FULL / _ABSENT, and
 * CLI_ENABLE_DANGEROUS_CMDS 0 / 1, SHELL_HAS_NET_TTY 0 / 1):
 *   - every argument refusal (unaligned, length 0, past the end, near 2^32,
 *     bad hex, too long, read 0 / 1025 / past the end) calls nothing of the
 *     board: no range, no hold, no operation;
 *   - the range refusal and the console refusals (telnet, its jobs, a job of
 *     the local console) take no hold;
 *   - busy and down: no operation, the board's words said;
 *   - an erase is one operation of exactly the range, Ctrl+C offered only by a
 *     board that can stop between sectors, and a cancel said as such;
 *   - the test: not blank -> nothing erased; every failure point and every
 *     Ctrl+C -> no PASS (and the way out said once anything was erased); a
 *     pass erases three times and leaves the sector blank;
 *   - a subcommand the board does not have calls nothing and says the hint;
 *   - with the dangerous commands off, erase / write / test are not in the
 *     table; no name twice in it.
 */
#include "cli.h"
#include "cli_instance.h"
#include "nor_cmd_core.h"
#include "nor_svc.h"
#include "log.h"
#include "tx_api.h"
#if SHELL_HAS_NET_TTY
#include "net_tty.h"
#endif

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEV_SIZE 0x100000u      /* 1 MB */
#define UNUSED_OK __attribute__((unused))   /* some helpers serve some shapes */
#define KV_END   0x10000u       /* the fake board refuses [0, 64 KB) */

/* ---- the fake board ------------------------------------------------------- */

static uint8_t dev[DEV_SIZE];

enum op { OP_ERASE, OP_WRITE, OP_READ, OP_N };
static int calls[OP_N], unheld[OP_N];
static int fail_op = -1, fail_at, noop_op = -1, noop_at;
static int range_calls, hold_calls, read_calls, info_calls, probe_calls;
static int held, busy, down;
static uint32_t last_addr, last_len;
static int ticks;

static int hit(enum op o, int *noop)
{
	calls[o]++;
	if (!held)
		unheld[o]++;
	*noop = (noop_op == (int)o && noop_at == calls[o]);
	return fail_op == (int)o && fail_at == calls[o];
}

static void op_erase(void *d, uint32_t addr, uint32_t len,
                     const struct nor_svc_progress *p, struct nor_svc_result *r)
{
	int noop, fail = hit(OP_ERASE, &noop);
	uint32_t done;

	(void)d;
	last_addr = addr;
	last_len = len;
	if (fail) {
		r->status = NOR_SVC_IO;
		r->why = "the fake failed";
		return;
	}
	for (done = 0u; done < len; done += NOR_SVC_ERASE_UNIT) {
		if (!noop)
			memset(dev + addr + done, 0xFF, NOR_SVC_ERASE_UNIT);
		if (p != NULL && nor_svc_board.erase_cancels) {
			ticks++;
			if (p->tick(p->ctx, done + NOR_SVC_ERASE_UNIT, len) &&
			    done + NOR_SVC_ERASE_UNIT < len) {
				r->status = NOR_SVC_CANCELLED;
				r->done = done + NOR_SVC_ERASE_UNIT;
				return;
			}
		}
	}
	r->status = NOR_SVC_OK;
	r->done = len;
}

static void op_program(void *d, uint32_t addr, const uint8_t *data, uint32_t len,
                       struct nor_svc_result *r)
{
	int noop, fail = hit(OP_WRITE, &noop);
	uint32_t i;

	(void)d;
	last_addr = addr;
	last_len = len;
	if (fail) {
		r->status = NOR_SVC_IO;
		r->why = "the fake failed";
		return;
	}
	if (!noop)
		for (i = 0; i < len; i++)
			dev[addr + i] &= data[i];
	r->status = NOR_SVC_OK;
	r->done = len;
}

static void op_read(void *d, uint32_t addr, uint8_t *buf, uint32_t len,
                    struct nor_svc_result *r)
{
	int noop, fail = hit(OP_READ, &noop);

	(void)d;
	(void)noop;
	if (fail) {
		r->status = NOR_SVC_IO;
		r->why = "the fake failed";
		return;
	}
	memcpy(buf, dev + addr, len);
	r->status = NOR_SVC_OK;
}

static const struct nor_svc_ops ops = { NULL, op_erase, op_program, op_read };

static void b_hold(nor_svc_body_fn body, void *ctx, struct nor_svc_result *r)
{
	hold_calls++;
	if (down) {
		r->status = NOR_SVC_DOWN;
		r->why = "the fake is down";
		return;
	}
	if (busy) {
		r->status = NOR_SVC_BUSY;
		r->why = "the fake's other user";
		return;
	}
	held = 1;
	body(ctx, &ops);
	held = 0;
	r->status = NOR_SVC_OK;
}

static void b_range(enum nor_svc_op op, uint32_t addr, uint32_t len,
                    struct nor_svc_range *out)
{
	(void)op;
	range_calls++;
	if (len == 0u || addr >= DEV_SIZE || len > DEV_SIZE - addr) {
		/* the shared command promised this never happens */
		printf("FAIL: range asked of an argument it should have refused\n");
		exit(1);
	}
	out->kind = (addr < KV_END) ? NOR_SVC_RANGE_INSIDE : NOR_SVC_RANGE_OK;
	out->lo = 0u;
	out->hi = KV_END;
	out->name = "fake kv";
	out->hint = " -- the fake's hint";
}

UNUSED_OK static void b_read(uint32_t addr, uint8_t *buf, uint32_t len, struct nor_svc_result *r)
{
	read_calls++;
	memcpy(buf, dev + addr, len);
	r->status = NOR_SVC_OK;
}

static int b_info(nor_svc_common_fn common, nor_svc_write_fn write, void *ctx,
                  struct nor_svc_result *r)
{
	struct nor_svc_info in = { "fine", "", 1u, { 1, 2, 3 }, "" };

	(void)r;
	info_calls++;
	if (common(ctx, &in) != 0)
		return -1;
	return write(ctx, "  board   : line\r\n", 18) < 0 ? -1 : 0;
}

int nor_test_probe(struct cli_instance *sh, int argc, char **argv)
{
	(void)sh; (void)argc; (void)argv;
	probe_calls++;
	return 0;
}

const struct nor_svc_board nor_svc_board = {
	.part = "fake part", .size = DEV_SIZE, .erase_unit = NOR_SVC_ERASE_UNIT,
	.page = 256u,
#ifdef NOR_TEST_SHAPE_ABSENT
	.erase_cancels = 0u,
	.no_read = "the fake's read hint", .no_write = "", .no_test = "use X",
	.read = NULL,
#else
	.erase_cancels = 1u,
	.no_read = NULL, .no_write = NULL, .no_test = NULL,
	.read = b_read,
#endif
	.info = b_info, .range = b_range, .hold = b_hold,
};

/* ---- the console and the rest of the shell -------------------------------- */

static struct cli_instance usb, telnet;
UNUSED_OK static struct cli_instance usb_job = { .fg = &usb };
UNUSED_OK static struct cli_instance telnet_job = { .fg = &telnet };

#if SHELL_HAS_NET_TTY
bool net_tty_is_console(const struct cli_instance *sh)
{
	const struct cli_instance *owner = (sh->fg != NULL) ? sh->fg : sh;

	return owner == &telnet;
}
const struct net_tty_facts net_tty_board_facts = { .other_console = "USB" };
#endif

static char text[32768];
static size_t text_len;
static char logline[256];
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
{
	va_list ap;

	(void)level; (void)tag;
	va_start(ap, fmt);
	vsnprintf(logline, sizeof logline, fmt, ap);
	va_end(ap);
}
ULONG tx_time_get(void) { return 0; }

/* ---- the harness ---------------------------------------------------------- */

static int failures;

static void check(int ok, const char *what, const char *line)
{
	if (!ok) {
		printf("FAIL: %s -- `%s`\n  (erase %d write %d read %d, range %d hold %d)\n%s\n",
		       what, line, calls[OP_ERASE], calls[OP_WRITE], calls[OP_READ],
		       range_calls, hold_calls, text);
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
	const struct cli_cmd *s;

	for (s = nor_table(); s != NULL && s->name != NULL; s++)
		if (strcmp(s->name, name) == 0)
			return s;
	return NULL;
}

static void reset(void)
{
	memset(calls, 0, sizeof calls);
	memset(unheld, 0, sizeof unheld);
	fail_op = noop_op = -1;
	range_calls = hold_calls = read_calls = info_calls = probe_calls = 0;
	held = busy = down = 0;
	cancel_at = cancel_asks = ticks = 0;
	text_len = 0;
	text[0] = '\0';
	logline[0] = '\0';
}

UNUSED_OK static int ops_calls(void) { return calls[OP_ERASE] + calls[OP_WRITE] + calls[OP_READ]; }
UNUSED_OK static int has(const char *s) { return strstr(text, s) != NULL; }

static int run(struct cli_instance *sh, const char *line)
{
	static char buf[700];
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
	/* [!] #249: nothing reaches the device without the hold, in any run */
	check(unheld[OP_ERASE] == 0 && unheld[OP_WRITE] == 0 && unheld[OP_READ] == 0,
	      "no erase, program or test read without the hold", line);
	return rc;
}

/* Refused before the board is asked anything at all. */
UNUSED_OK static void refused_args(const char *line, const char *why)
{
	reset();
	check(run(&usb, line) == 1, "refused", line);
	check(range_calls == 0 && hold_calls == 0 && read_calls == 0 && ops_calls() == 0,
	      "an argument refusal calls nothing of the board", line);
	check(has(why), "the reason is said", line);
}

/* Refused after the range and before the hold. */
UNUSED_OK static void refused_before_hold(struct cli_instance *sh, const char *line,
                                const char *why)
{
	reset();
	check(run(sh, line) == 1, "refused", line);
	check(hold_calls == 0 && ops_calls() == 0, "no hold, no operation", line);
	check(has(why), "the reason is said", line);
}

UNUSED_OK static void blank(uint32_t a, uint32_t n) { memset(dev + a, 0xFF, n); }
UNUSED_OK static int is_blank(uint32_t a, uint32_t n)
{
	for (uint32_t i = 0; i < n; i++)
		if (dev[a + i] != 0xFFu)
			return 0;
	return 1;
}

int main(void)
{
	const struct cli_cmd *s;
	int dups = 0, rc UNUSED_OK;

	memset(dev, 0xFF, sizeof dev);
	for (s = nor_table(); s != NULL && s->name != NULL; s++)
		for (const struct cli_cmd *t = nor_table(); t < s; t++)
			dups += strcmp(s->name, t->name) == 0;
	check(nor_table() != NULL && dups == 0, "the table has no name twice", "nor");
	check(nor_sub("probe") != NULL, "the board's own subcommand is in it", "nor");

	reset();
	check(run(&usb, "info") == 0 && info_calls == 1 && has("NOR: fake part") &&
	      has("  jedec   : 01 02 03\r\n") && has("  board   : line"),
	      "info: shared lines, then the board's", "info");

#if !CLI_ENABLE_DANGEROUS_CMDS
	check(nor_sub("erase") == NULL && nor_sub("write") == NULL &&
	      nor_sub("test") == NULL, "no erase / write / test without the setting",
	      "nor");
	printf("test_nor_cmd (no dangerous commands): %s\n", failures ? "FAILED" : "OK");
	return failures ? 1 : 0;
#else
# ifdef NOR_TEST_SHAPE_ABSENT
	{
		static const char *const lines[] = { "read 0x20000", "write 0x20000 00",
		                                     "test 0x20000" };
		static const char *const says[] = {
			"no `nor read` on this board -- the fake's read hint\r\n",
			"no `nor write` on this board\r\n",
			"no `nor test` on this board -- use X\r\n" };

		for (unsigned i = 0; i < 3; i++) {
			reset();
			check(run(&usb, lines[i]) == 1 && range_calls == 0 && hold_calls == 0 &&
			      read_calls == 0 && ops_calls() == 0, "absent: calls nothing",
			      lines[i]);
			check(has(says[i]), "absent: says so, with the hint", lines[i]);
		}
	}
	reset();
	blank(0x20000, 0x2000);
	check(run(&usb, "erase 0x20000 0x2000") == 0 && ticks == 0 && !has("Ctrl+C"),
	      "a board that cannot stop: no progress, no Ctrl+C offered", "erase");
	printf("test_nor_cmd (absent subcommands): %s\n", failures ? "FAILED" : "OK");
	return failures ? 1 : 0;
# else
	/* ---- the arguments: nothing of the board is asked ---- */
	refused_args("erase 0x20001 4096", "whole 4 KB sectors");
	refused_args("erase 0x20000 4095", "whole 4 KB sectors");
	refused_args("erase 0x20000 0", "whole 4 KB sectors");
	refused_args("erase 0x20000 x", "bad length");
	refused_args("erase 0x100000 4096", "bad address");
	refused_args("erase 0xFFFFF000 4096", "bad address");
	refused_args("erase 0xFF000 0x2000", "past the end");
	refused_args("erase 0x20000 0xFFFFF000", "past the end");
	refused_args("erase 0 0xFFFFF000", "past the end");
	refused_args("write 0x20000 0", "bad hex");
	refused_args("write 0x20000 0g", "bad hex");
	refused_args("write 0xFFFFF 0000", "past the end");
	refused_args("write 0xFFFFFFFF 00", "bad address");
	{
		char line[600] = "write 0x20000 ";

		for (int i = 0; i < 257; i++)
			strcat(line, "00");
		refused_args(line, "bad hex");
		line[14 + 512] = '\0';                  /* 256 B: taken */
		reset();
		blank(0x20000, 0x1000);
		check(run(&usb, line) == 0 && calls[OP_WRITE] == 1 && last_len == 256u,
		      "256 B is one program", "write 256 B");
	}
	refused_args("test 0x20010", "whole 4 KB sectors");
	refused_args("test 0x100000", "bad address");
	refused_args("read 0x20000 0", "1 .. 1024");
	refused_args("read 0x20000 1025", "1 .. 1024");
	refused_args("read 0xFFFFF 2", "past the end");
	refused_args("read 0x100000", "bad address");
	reset();
	check(run(&telnet, "read 0xFFC00 1024") == 0 && read_calls == 4 && hold_calls == 0,
	      "read: up to the end, in four pieces, no hold, from telnet too", "read");

	/* ---- the range, then the console ---- */
	refused_before_hold(&usb, "erase 0xF000 0x2000",
	                    "0x00F000 .. 0x010FFF is in the fake kv (0x000000 .. "
	                    "0x00FFFF) -- the fake's hint");
	refused_before_hold(&usb, "write 0xFFFF 00", "is in the fake kv");
	refused_before_hold(&usb, "test 0", "is in the fake kv");
	{
		static const char *const lines[] = { "erase 0x20000 4096", "write 0x20000 00",
		                                     "test 0x20000" };

		for (unsigned i = 0; i < 3; i++) {
			refused_before_hold(&usb_job, lines[i], "this is a background job");
#  if SHELL_HAS_NET_TTY
			refused_before_hold(&telnet, lines[i], "this is the telnet console");
			refused_before_hold(&telnet_job, lines[i],
			                    "this is a background job of the telnet console");
#  else
			reset();
			check(run(&telnet, lines[i]) != 1 || hold_calls == 1,
			      "no telnet: only background jobs are refused", lines[i]);
#  endif
			reset();
			busy = 1;
			check(run(&usb, lines[i]) == 1 && hold_calls == 1 && ops_calls() == 0 &&
			      has("nor: busy -- the fake's other user"), "busy: the board's words",
			      lines[i]);
			check(strstr(logline, "busy") != NULL, "busy: logged", lines[i]);
			reset();
			down = 1;
			check(run(&usb, lines[i]) == 1 && hold_calls == 1 && ops_calls() == 0 &&
			      has("nor: the fake is down"), "down: the board's words", lines[i]);
		}
	}

	/* ---- erase ---- */
	reset();
	rc = run(&usb, "erase 0x20000 0x40000");
	check(rc == 0 && calls[OP_ERASE] == 1 && last_addr == 0x20000u &&
	      last_len == 0x40000u, "one erase of exactly the range", "erase");
	check(has("erasing  : 0x020000 .. 0x05FFFF (256 KB, 64 sector(s))") &&
	      has("Ctrl+C stops it"), "the span lost is said first", "erase");
	check(ticks == 64 && strstr(logline, "erase ok 0x20000+262144") != NULL,
	      "a tick per sector, one log line", "erase");
	reset();
	cancel_at = 3;
	rc = run(&usb, "erase 0x20000 0x40000");
	check(rc == 1 && has("cancelled after 12288 B of 262144") &&
	      strstr(logline, "cancelled") != NULL, "Ctrl+C: said and logged", "erase");
	reset();
	fail_op = OP_ERASE;
	fail_at = 1;
	check(run(&usb, "erase 0x20000 4096") == 1 && has("erase failed: the fake failed"),
	      "a failed erase is said", "erase");

	/* ---- write ---- */
	reset();
	blank(0x20000, 0x1000);
	rc = run(&usb, "write 0x20001 A55A");
	check(rc == 0 && calls[OP_WRITE] == 1 && dev[0x20001] == 0xA5u &&
	      dev[0x20002] == 0x5Au && last_len == 2u, "written as given", "write");

	/* ---- test ---- */
	reset();
	blank(0x20000, 0x1000);
	dev[0x20FFF] = 0x00;
	rc = run(&usb, "test 0x20000");
	check(rc == 1 && has("is not blank") && has("`nor erase 0x020000 4096` first") &&
	      calls[OP_ERASE] == 0 && calls[OP_WRITE] == 0 && dev[0x20FFF] == 0x00,
	      "not blank: refused before anything is erased", "test");
	reset();
	blank(0x20000, 0x1000);
	dev[0x21000] = 0x00;                     /* the next sector is not ours */
	rc = run(&usb, "test 0x20000");
	check(rc == 0 && has("nor: PASS") && calls[OP_ERASE] == 3 &&
	      is_blank(0x20000, 0x1000) && dev[0x21000] == 0x00,
	      "a pass erases three times and leaves only its sector blank", "test");
	check(has("overwrite: 0xFF -> write 0xFE -> read 0xFE -> write 0xFC -> read 0xFC"),
	      "the overwrite is observed", "test");
	check(strstr(logline, "test 0x20000: pass") != NULL, "logged", "test");
	{
		int base[OP_N], asks;

		memcpy(base, calls, sizeof base);
		for (int o = 0; o < OP_N; o++)
			for (int k = 1; k <= base[o]; k++) {
				reset();
				blank(0x20000, 0x1000);
				fail_op = o;
				fail_at = k;
				rc = run(&usb, "test 0x20000");
				check(rc == 1 && !has("PASS"), "a failure point: no PASS", "test");
				/* the first 16 reads are the blank check before any erase */
				check(has(o == OP_READ && k <= 16 ? "nothing was erased"
				                                  : "before the next `nor test`"),
				      "a failure point: the way out", "test");
			}
		/* a program or erase that does nothing (but the two observed
		 * overwrites and the first erase, which met a blank sector) */
		for (int k = 1; k <= base[OP_WRITE] - 2; k++) {
			reset();
			blank(0x20000, 0x1000);
			noop_op = OP_WRITE;
			noop_at = k;
			check(run(&usb, "test 0x20000") == 1 && !has("PASS"),
			      "a program that does nothing is caught", "test");
		}
		for (int k = 2; k <= base[OP_ERASE]; k++) {
			reset();
			blank(0x20000, 0x1000);
			noop_op = OP_ERASE;
			noop_at = k;
			check(run(&usb, "test 0x20000") == 1 && !has("PASS"),
			      "an erase that does nothing is caught", "test");
		}
		reset();
		blank(0x20000, 0x1000);
		cancel_at = 1000000;
		check(run(&usb, "test 0x20000") == 0, "pass", "test");
		asks = cancel_asks;
		/* four blank checks of 16 pages, three erases, 16 pages programmed
		 * and 16 read, 14 partial writes, 2 overwrites: before every one */
		check(asks == 4 * 16 + 3 + 16 + 16 + 14 + 2, "Ctrl+C asked before every page",
		      "test");
		for (int k = 1; k <= asks; k++) {
			reset();
			blank(0x20000, 0x1000);
			cancel_at = k;
			check(run(&usb, "test 0x20000") == 1 && !has("PASS") && has("cancelled"),
			      "Ctrl+C stops the test", "test");
		}
	}
	printf("test_nor_cmd (full%s): %s\n", SHELL_HAS_NET_TTY ? ", telnet" : ", no telnet",
	       failures ? "FAILED" : "OK");
	return failures ? 1 : 0;
# endif
#endif
}
