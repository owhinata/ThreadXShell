/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Issue #250 -- the shared `nor` on this board: shell/cmds/cmd_nor.c,
 * shell/cmds/nor_console.c and this board's table (cmds/cmd_nor_board.c),
 * compiled unchanged and reached through the root command table, over stand-ins
 * of the port (the leases, the writer reservation, the report, the lifecycle)
 * and of port/nor/nor_write.c's three transactions.
 *
 * What it asks:
 *   - one `nor erase` or `nor write` is ONE call of nor_write_erase() /
 *     nor_write_program(), made inside the reservation (taken once, given
 *     back once), with exactly the range; a `nor cycle` is one
 *     nor_write_cycle(); [!] none of the three is ever called without the
 *     reservation held (issue #249's lesson);
 *   - the range is the seam's own record: outside it, and a write that does
 *     not start on a word, are refused before the reservation; an odd length
 *     is fine;
 *   - a background job is refused before the reservation, for erase, write
 *     and the board's own `cycle` reached through the real registration;
 *   - a refused reservation names the one holder, says "readers" for two,
 *     names the camera only from XIP, and says the port's reason when faulted;
 *     `blob`'s nor_cmd_say_busy() says the same words;
 *   - every transaction outcome is said in the shared lines (FAULTED with the
 *     first bad byte, a refused target, the camera, a cancel);
 *   - there is no `nor read` and no `nor test`, and asking calls nothing.
 */
#include "cli.h"
#include "cli_instance.h"
#include "log.h"
#include "nor_cmd.h"
#include "nor_flash.h"
#include "nor_seam.h"
#include "nor_write.h"
#include "tx_api.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

/* ---- the port ------------------------------------------------------------- */

const struct nor_seam_limits nor_seam_limits = { 0x200000u, 0xFFE000u, 0x1000u };

static enum nor_state state = NOR_ST_XIP;
static uint32_t live, readers;
static int camera, acquire_fails, reserve_fails;
static int reserved, reserves, unreserves, acquires, releases;
static int n_erase, n_program, n_cycle, n_unheld, ticks;
static uint32_t last_addr, last_len;
static uint8_t last_data[256];
static enum nor_write_status next_st = NOR_WRITE_OK;
static struct nor_write_report next_r;
static const char *fail_reason;

int nor_acquire(enum nor_lease_slot slot, uint32_t *out)
{
	(void)slot;
	acquires++;
	if (acquire_fails)
		return -1;
	*out = 1u;
	if (state == NOR_ST_OFF)
		state = NOR_ST_XIP;
	return 0;
}
int nor_release(uint32_t token) { (void)token; releases++; return 0; }
enum nor_state nor_lifecycle_state(void) { return state; }
const char *nor_fail_reason(void) { return fail_reason; }
void nor_report(struct nor_report *r)
{
	memset(r, 0, sizeof *r);
	r->state = state;
	r->live = live;
	r->readers = readers;
	r->jedec_valid = 1u;
	r->jedec[0] = 0xC8;
	r->irq = -1;
}
int nor_reserve(uint32_t *out)
{
	reserves++;
	if (reserve_fails)
		return -1;
	reserved = 1;
	*out = 0x77u;
	return 0;
}
int nor_unreserve(uint32_t token)
{
	unreserves++;
	if (token != 0x77u)
		printf("FAIL: unreserve with a token that is not the reservation's\n");
	reserved = 0;
	return 0;
}
int nor_write_camera_live(void) { return camera; }
const char *nor_state_name(enum nor_state st)
{
	return st == NOR_ST_XIP ? "xip" : st == NOR_ST_OFF ? "off" : "faulted";
}

static enum nor_write_status answer(uint32_t token, struct nor_write_report *r)
{
	if (!reserved || token != 0x77u)
		n_unheld++;
	*r = next_r;
	return next_st;
}

enum nor_write_status nor_write_erase(uint32_t token, uint32_t addr, uint32_t len,
                                      const struct nor_erase_progress *prog,
                                      struct nor_write_report *r)
{
	n_erase++;
	last_addr = addr;
	last_len = len;
	if (prog != NULL)
		for (uint32_t d = 0x1000u; d <= len; d += 0x1000u) {
			ticks++;
			if (prog->tick(prog->ctx, d, len) && d < len) {
				answer(token, r);
				r->done = d;
				r->cancelled = 1u;
				return NOR_WRITE_INCOMPLETE;
			}
		}
	return answer(token, r);
}
enum nor_write_status nor_write_program(uint32_t token, uint32_t addr,
                                        const void *data, uint32_t len,
                                        struct nor_write_report *r)
{
	n_program++;
	last_addr = addr;
	last_len = len;
	memcpy(last_data, data, len < sizeof last_data ? len : sizeof last_data);
	return answer(token, r);
}
enum nor_write_status nor_write_cycle(uint32_t token, struct nor_write_report *r)
{
	n_cycle++;
	return answer(token, r);
}

/* ---- the console ---------------------------------------------------------- */

static struct cli_instance uart;
static struct cli_instance uart_job = { .fg = &uart };

static char text[16384];
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
int cli_sleep(struct cli_instance *sh, unsigned ticks_)
{ (void)sh; (void)ticks_; return 0; }
void log_write(unsigned level, const char *tag, const char *fmt, ...)
{ (void)level; (void)tag; (void)fmt; }
ULONG tx_time_get(void) { return 0; }

/* ---- the harness ---------------------------------------------------------- */

static int failures;

static void check(int ok, const char *what, const char *line)
{
	if (!ok) {
		printf("FAIL: %s -- `%s`\n  (erase %d program %d cycle %d unheld %d, "
		       "reserve %d unreserve %d)\n%s\n", what, line, n_erase, n_program,
		       n_cycle, n_unheld, reserves, unreserves, text);
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
	state = NOR_ST_XIP;
	live = readers = 0u;
	camera = acquire_fails = reserve_fails = 0;
	reserved = reserves = unreserves = acquires = releases = 0;
	n_erase = n_program = n_cycle = n_unheld = ticks = 0;
	next_st = NOR_WRITE_OK;
	memset(&next_r, 0, sizeof next_r);
	fail_reason = NULL;
	cancel_at = cancel_asks = 0;
	text_len = 0;
	text[0] = '\0';
}

static int has(const char *s) { return strstr(text, s) != NULL; }
static int writes(void) { return n_erase + n_program + n_cycle; }

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
	check(n_unheld == 0, "no transaction without the reservation", line);
	check(unreserves == reserves - reserve_fails && !reserved,
	      "the reservation given back once for every one taken", line);
	check(acquires == releases + acquire_fails, "every bring-up lease handed back",
	      line);
	return rc;
}

static void refused_early(struct cli_instance *sh, const char *line, const char *why)
{
	reset();
	check(run(sh, line) == 1, "refused", line);
	check(reserves == 0 && writes() == 0, "refused before the reservation", line);
	check(has(why), "the reason is said", line);
}

int main(void)
{
	int dups = 0;

	for (const struct cli_cmd *s = nor_table(); s && s->name; s++)
		for (const struct cli_cmd *t = nor_table(); t < s; t++)
			dups += strcmp(s->name, t->name) == 0;
	check(nor_table() != NULL && dups == 0 && nor_sub("scan") && nor_sub("cycle"),
	      "scan and cycle in the table, no name twice", "nor");

	/* ---- what this board does not have ---- */
	reset();
	check(run(&uart, "read 0x200000") == 1 && acquires == 0 && writes() == 0 &&
	      has("no `nor read` on this board -- the flash is mapped at 0x3A000000; "
	          "`devmem dump` reads it"), "no read", "read");
	reset();
	check(run(&uart, "test 0x200000") == 1 && reserves == 0 && writes() == 0 &&
	      has("no `nor test` on this board -- every `nor write` reads back"),
	      "no test", "test");

	/* ---- one transaction per command ---- */
	reset();
	check(run(&uart, "erase 0x200000 0x2000") == 0 && n_erase == 1 &&
	      last_addr == 0x200000u && last_len == 0x2000u && reserves == 1 &&
	      ticks == 2, "erase: one transaction, exactly the range, a tick per unit",
	      "erase");
	reset();
	cancel_at = 1;
	check(run(&uart, "erase 0x200000 0x2000") == 1 && n_erase == 1 &&
	      has("cancelled after 4096 B of 8192"), "erase: a cancel is said as one",
	      "erase cancel");
	reset();
	check(run(&uart, "write 0x200004 A55A01") == 0 && n_program == 1 &&
	      last_addr == 0x200004u && last_len == 3u && last_data[0] == 0xA5u &&
	      last_data[2] == 0x01u, "write: one transaction, an odd length", "write");
	reset();
	check(run(&uart, "cycle") == 0 && n_cycle == 1 && reserves == 1 &&
	      has("costs two status-register writes") && has("no data touched"),
	      "cycle: one transaction", "cycle");

	/* ---- the seam's interval and the word a write starts on ---- */
	refused_early(&uart, "erase 0x1FF000 0x2000", "is not inside what this board writes "
	              "(0x200000 .. 0xFFDFFF)");
	refused_early(&uart, "erase 0xFFD000 0x2000", "is not inside");
	refused_early(&uart, "erase 0xFFE000 4096", "is not inside");
	refused_early(&uart, "write 0x1FFFFF 00", "is not inside");
	refused_early(&uart, "write 0x200002 00", "a write starts on a multiple of 4 B");
	refused_early(&uart, "erase 0x200800 4096", "whole 4 KB sectors");

	/* ---- background jobs, through the real registration ---- */
	refused_early(&uart_job, "erase 0x200000 4096", "this is a background job");
	refused_early(&uart_job, "write 0x200000 00", "this is a background job");
	refused_early(&uart_job, "cycle", "this is a background job");
	check(!has("telnet"), "this board names no telnet console", "cycle");

	/* ---- who holds it ---- */
	reset();
	reserve_fails = 1;
	live = 1u << NOR_LEASE_NPU;
	readers = 1u;
	check(run(&uart, "erase 0x200000 4096") == 1 && writes() == 0 &&
	      has("nor: busy -- `nn` has a model open (nn model unload)"),
	      "one holder, named", "busy nn");
	reset();
	reserve_fails = 1;
	live = (1u << NOR_LEASE_NPU) | (1u << NOR_LEASE_DEVMEM);
	readers = 2u;
	check(run(&uart, "write 0x200000 00") == 1 &&
	      has("nor: busy -- readers hold the window (`nor info` lists them)"),
	      "two readers, not counted", "busy two");
	reset();
	reserve_fails = 1;
	camera = 1;
	check(run(&uart, "cycle") == 1 && has("busy -- the camera is streaming"),
	      "the camera, from XIP", "busy camera");
	reset();
	reserve_fails = 1;
	camera = 1;
	state = NOR_ST_RESERVED;
	check(run(&uart, "cycle") == 1 && has("another writer holds the part"),
	      "not the camera when another writer holds it", "busy writer");
	reset();
	reserve_fails = 1;
	state = NOR_ST_FAULTED;
	fail_reason = "read-back mismatch";
	check(run(&uart, "erase 0x200000 4096") == 1 && has("nor: read-back mismatch"),
	      "faulted: the port's reason", "faulted");
	reset();
	acquire_fails = 1;
	check(run(&uart, "erase 0x200000 4096") == 1 && reserves == 0 &&
	      has("could not be brought up"), "bring-up refused: no reservation",
	      "bring-up");
	reset();
	live = 1u << NOR_LEASE_BLOB;
	nor_cmd_say_busy(&uart, "blob");
	check(has("blob: busy -- a `blob` read is in flight"), "blob says the same",
	      "blob");

	/* ---- every outcome in the shared lines ---- */
	reset();
	next_st = NOR_WRITE_FAULTED;
	next_r.fail = "read-back mismatch";
	next_r.bad_valid = 1u;
	next_r.bad_off = 0x200010u;
	next_r.bad_got = 0x00u;
	next_r.bad_want = 0xA5u;
	next_r.jedec_ok = 1u;
	next_r.jedec[0] = 0xC8u;
	check(run(&uart, "write 0x200000 A5") == 1 &&
	      has("nor: FAULTED -- read-back mismatch") &&
	      has("first at 0x200010: read 0x00, wanted 0xA5") &&
	      has("jedec    : C8 00 00"), "faulted: the first bad byte", "faulted");
	reset();
	next_st = NOR_WRITE_REFUSED;
	next_r.fail = "the target is not erased";
	check(run(&uart, "write 0x200000 A5") == 1 &&
	      has("nor: refused -- the target is not erased"), "refused target", "refused");
	reset();
	next_st = NOR_WRITE_CAMERA;
	check(run(&uart, "write 0x200000 A5") == 1 && has("busy -- the camera is streaming"),
	      "camera at the claim", "camera");
	reset();
	next_st = NOR_WRITE_NO_TRANSPORT;
	check(run(&uart, "erase 0x200000 4096") == 1 && has("did not answer; nothing was sent"),
	      "no transport", "transport");
	reset();
	next_st = NOR_WRITE_OK;
	next_r.done = 1u;
	next_r.verified = 1u;
	check(run(&uart, "write 0x200000 A5") == 0 &&
	      has("ok       : 1 B done, 1 B read back and compared"), "ok", "ok");

	/* ---- info ---- */
	reset();
	state = NOR_ST_OFF;
	check(run(&uart, "info") == 0 && acquires == 1 && has("NOR: QSPI NOR") &&
	      has("  writable: 0x00200000..0x00ffe000, 4096 B unit") &&
	      has("  jedec   : C8 00 00 (read before XIP)"),
	      "info: brought up by a lease, the seam's record", "info");

	printf("test_nor_board (grove-vision-ai-v2): %s\n", failures ? "FAILED" : "OK");
	return failures ? 1 : 0;
}
