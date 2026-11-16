/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Issue #251 -- the one shared external RAM command (shell/cmds/cmd_ext_ram.c),
 * compiled unchanged over a fake board table (as `xram`) and reached through
 * the root command table the way a console reaches it.
 *
 * The fake window is a 1 MB array: [0, 256 KB) and [256 KB, 512 KB)
 * non-cacheable, [512 KB, 768 KB) cacheable, [768 KB, 1 MB) non-cacheable
 * again (after the cacheable range, so never tested either).  Four steps; any
 * take can refuse, the second can ask for a skip, any give can warn.  Every
 * word the command stores or loads goes through the test's pair
 * (EXT_RAM_TEST_HOOKS), which checks it is inside the window, before the first
 * cacheable range, outside this run's skip -- and made while every step is
 * taken and none given back yet: an access outside that is counted apart and
 * must be 0 in every run (the lesson of issue #249).
 *
 * What it asks:
 *   - the pure functions: the table check (gaps, overlaps, odd sizes, a
 *     cacheable first range, too many entries), the length (none, 0, odd, too
 *     long, garbage) and the spans around a skip (none, at the start, in the
 *     middle, past the end, covering all, not on whole words);
 *   - a length refusal and a bad table call nothing of the board; a board that
 *     is not ready is asked, and nothing is taken;
 *   - a take refused at each step gives back exactly the steps before it, in
 *     the reverse order, and nothing is written;
 *   - every way a test ends gives every step back once, in the reverse order;
 *     a give that warns is said and the others still run;
 *   - a pass: every word of [0, len) minus the skip written and read three
 *     times, the cacheable range and the rest never touched; a skip said;
 *   - a bad word is found in each pass, an aliased address line in the first;
 *   - Ctrl+C is asked after every 64 KB, and at each ask stops the test;
 *   - no name twice in the table (and -DEXT_RAM_TEST_DUP must fail on it).
 */
#include "cli.h"
#include "cli_instance.h"
#include "ext_ram_cmd_board.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WIN       0x100000u      /* 1 MB */
#define TESTABLE  0x80000u       /* where the cacheable range starts */
#define NSTEPS    4u

/* ---- the fake board ------------------------------------------------------- */

static uint32_t mem[WIN / 4u];

static const struct ext_ram_range ranges[] = {
	{ 0x00000u, 0x40000u, 0u, "low" },
	{ 0x40000u, 0x40000u, 0u, "eth" },
	{ 0x80000u, 0x40000u, 1u, "nn" },
	{ 0xC0000u, 0x40000u, 0u, "after" },
};

static char seq[256];            /* "T0 T1 G0 " ... */
static int refuse_at = -1, warn_at = -1, ready_ok = 1;
static int taken_n, held, ready_calls, tune_calls;
static uint32_t skip_off, skip_len;
static int span_refusal;
static long stores, loads, bad_access;
static uint32_t flip_off = UINT32_MAX;   /* a load of this word is wrong ...  */
static long flip_load = -1;              /* ... the flip_load-th time         */
static long flip_seen;
static uint32_t alias_bit;               /* a store with this offset bit lands without it */
static uint32_t run_skip_off, run_skip_len;

static void say(const char *s)
{
	strncat(seq, s, sizeof seq - strlen(seq) - 1u);
}

static int step_take(unsigned k, struct ext_ram_claim *c)
{
	char b[8];

	snprintf(b, sizeof b, "T%u ", k);
	say(b);
	if ((int)k == refuse_at) {
		c->why = "the fake refused";
		if (span_refusal) {
			c->span_valid = 1u;
			c->lo = 0x1000u;
			c->hi = 0x1FFFu;
		}
		return -1;
	}
	if (k == 1u && skip_len != 0u) {
		c->skip_off = skip_off;
		c->skip_len = skip_len;
		c->skip_why = "the fake's network";
	}
	taken_n++;
	if (k == NSTEPS - 1u) {             /* the last step: all held */
		held = 1;
		run_skip_off = c->skip_off;
		run_skip_len = c->skip_len;
	}
	return 0;
}

static const char *step_give(unsigned k)
{
	char b[8];

	snprintf(b, sizeof b, "G%u ", k);
	say(b);
	held = 0;
	return ((int)k == warn_at) ? "the fake could not restore; run X" : NULL;
}

static int t0(struct ext_ram_claim *c) { return step_take(0u, c); }
static int t1(struct ext_ram_claim *c) { return step_take(1u, c); }
static int t3(struct ext_ram_claim *c) { return step_take(3u, c); }
static const char *g0(void) { return step_give(0u); }
static const char *g2(void) { return step_give(2u); }
static const char *g3(void) { return step_give(3u); }

/* Step 2 has no take, step 1 no give: NULL is allowed on either side. */
static const struct ext_ram_step steps[NSTEPS] = {
	{ t0, g0 }, { t1, NULL }, { NULL, g2 }, { t3, g3 },
};

static int b_ready(void)
{
	ready_calls++;
	return ready_ok;
}

static int b_info(ext_ram_write_fn write, void *ctx)
{
	return write(ctx, "  board   : line\r\n", 18) < 0 ? -1 : 0;
}

const struct ext_ram_board ext_ram_board = {
	.part = "fake part", .base = (uintptr_t)mem, .size = WIN,
	.ranges = ranges, .n_ranges = 4u, .steps = steps, .n_steps = NSTEPS,
	.ready = b_ready, .info = b_info,
};

int xram_test_tune(struct cli_instance *sh, int argc, char **argv)
{
	(void)sh; (void)argc; (void)argv;
	tune_calls++;
	return 0;
}

/* Every access: inside the window, before TESTABLE, outside the run's skip,
 * and while every step is held. */
static uint32_t check_access(volatile uint32_t *p)
{
	uintptr_t a = (uintptr_t)p, lo = (uintptr_t)mem;
	uint32_t off;

	if (a < lo || a >= lo + WIN || ((a - lo) & 3u) != 0u) {
		printf("FAIL: an access outside the window\n");
		exit(1);
	}
	off = (uint32_t)(a - lo);
	if (!held || off >= TESTABLE ||
	    (run_skip_len != 0u && off >= run_skip_off &&
	     off - run_skip_off < run_skip_len))
		bad_access++;
	return off;
}

void ext_ram_test_store(volatile uint32_t *p, uint32_t v)
{
	uint32_t off = check_access(p);

	stores++;
	if (alias_bit != 0u)
		off &= ~alias_bit;
	mem[off / 4u] = v;
}

uint32_t ext_ram_test_load(volatile uint32_t *p)
{
	uint32_t off = check_access(p), v = mem[off / 4u];

	loads++;
	if (off == flip_off && ++flip_seen == flip_load)
		v ^= 0x00010000u;
	return v;
}

/* ---- the console ---------------------------------------------------------- */

static struct cli_instance usb;
static char text[16384];
static size_t text_len;
static long cancel_at, cancel_asks;

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
int cli_warn(struct cli_instance *sh, const char *f, ...)
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
bool cli_cancel_requested(struct cli_instance *sh)
{
	(void)sh;
	return cancel_at != 0 && ++cancel_asks == cancel_at;
}

/* ---- the harness ---------------------------------------------------------- */

static int failures;

static void check(int ok, const char *what, const char *line)
{
	if (!ok) {
		printf("FAIL: %s -- `%s`\n  (steps \"%s\", stores %ld loads %ld bad %ld)\n%s\n",
		       what, line, seq, stores, loads, bad_access, text);
		failures++;
	}
}

static const struct cli_cmd *xram_table(void)
{
	CLI_ROOT_CMD_FOREACH(c)
		if (strcmp(c->name, "xram") == 0)
			return c->subcmds;
	return NULL;
}

static const struct cli_cmd *xram_sub(const char *name)
{
	const struct cli_cmd *s;

	for (s = xram_table(); s != NULL && s->name != NULL; s++)
		if (strcmp(s->name, name) == 0)
			return s;
	return NULL;
}

static void reset(void)
{
	seq[0] = '\0';
	refuse_at = warn_at = -1;
	ready_ok = 1;
	taken_n = held = ready_calls = tune_calls = span_refusal = 0;
	skip_off = skip_len = run_skip_off = run_skip_len = 0u;
	stores = loads = bad_access = 0;
	flip_off = UINT32_MAX;
	flip_load = -1;
	flip_seen = 0;
	alias_bit = 0u;
	cancel_at = cancel_asks = 0;
	text_len = 0;
	text[0] = '\0';
	memset(mem, 0xEE, sizeof mem);
}

static int has(const char *s) { return strstr(text, s) != NULL; }

/* The window outside [0, TESTABLE) still holds the fill. */
static int rest_untouched(void)
{
	for (uint32_t i = TESTABLE / 4u; i < WIN / 4u; i++)
		if (mem[i] != 0xEEEEEEEEu)
			return 0;
	return 1;
}

static int run(const char *line)
{
	static char buf[128];
	char *argv[8];
	int argc = 0, rc;
	const struct cli_cmd *s;
	char *p;

	snprintf(buf, sizeof buf, "%s", line);
	for (p = strtok(buf, " "); p != NULL && argc < 8; p = strtok(NULL, " "))
		argv[argc++] = p;
	s = xram_sub(argv[0]);
	if (s == NULL)
		return -100;
	rc = s->handler(&usb, argc, argv);
	/* [!] #249: no word is stored or loaded unless every step is held */
	check(bad_access == 0, "every access inside the steps, the testable part "
	      "and outside the skip", line);
	check(held == 0, "nothing left held", line);
	check(rest_untouched(), "the cacheable range and after it untouched", line);
	return rc;
}

/* Every step taken was given back once, in the reverse order. */
static int all_given(void)
{
	return strcmp(seq, "T0 T1 T3 G3 G2 G0 ") == 0;
}

static void refused_len(const char *line)
{
	reset();
	check(run(line) == 1, "refused", line);
	check(seq[0] == '\0' && ready_calls == 0 && stores == 0 && loads == 0,
	      "a length refusal calls nothing of the board", line);
	check(has("xram: bad length (4-byte multiple, 4..524288)\r\n"), "said", line);
}

/* ---- the pure functions --------------------------------------------------- */

static void pure(void)
{
	struct ext_ram_range r[EXT_RAM_RANGES_MAX + 1u];
	struct ext_ram_board b;
	struct ext_ram_span sp[2];
	uint32_t t, len;

	check(ext_ram_table_check(&ext_ram_board, &t) == 0 && t == TESTABLE,
	      "the fake's table: testable up to the cacheable range", "table");

#define TAB(n_) do { memset(&b, 0, sizeof b); b.size = 0x1000u; b.ranges = r; \
	b.n_ranges = (n_); b.ready = b_ready; } while (0)
#define R(i_, o_, l_, c_) do { r[i_].off = (o_); r[i_].len = (l_); \
	r[i_].cached = (c_); r[i_].what = "x"; } while (0)
	TAB(1u); R(0, 0, 0x1000u, 0);
	check(ext_ram_table_check(&b, &t) == 0 && t == 0x1000u, "one range, no cache", "table");
	TAB(2u); R(0, 0, 0x800u, 0); R(1, 0x800u, 0x800u, 1);
	check(ext_ram_table_check(&b, &t) == 0 && t == 0x800u, "cache at the top", "table");
	TAB(1u); R(0, 0, 0x1000u, 1);
	check(ext_ram_table_check(&b, &t) == -1 && t == 0u, "cacheable first: nothing testable", "table");
	TAB(2u); R(0, 0, 0x800u, 0); R(1, 0x804u, 0x7FCu, 0);
	check(ext_ram_table_check(&b, &t) == -1, "a gap", "table");
	TAB(2u); R(0, 0, 0x800u, 0); R(1, 0x7FCu, 0x804u, 0);
	check(ext_ram_table_check(&b, &t) == -1, "an overlap", "table");
	TAB(2u); R(0, 0, 0x800u, 0); R(1, 0x900u, 0x800u, 0);
	check(ext_ram_table_check(&b, &t) == -1, "the sizes add up, the offsets do not", "table");
	TAB(1u); R(0, 0, 0xFFCu, 0);
	check(ext_ram_table_check(&b, &t) == -1, "short of the end", "table");
	TAB(1u); R(0, 0, 0x2000u, 0);
	check(ext_ram_table_check(&b, &t) == -1, "past the end", "table");
	TAB(2u); R(0, 0, 0x1000u, 0); R(1, 0x1000u, 0u, 0);
	check(ext_ram_table_check(&b, &t) == -1, "an empty range", "table");
	TAB(2u); R(0, 0, 0x802u, 0); R(1, 0x802u, 0x7FEu, 0);
	check(ext_ram_table_check(&b, &t) == -1, "not on whole words", "table");
	TAB(2u); R(0, 0, 0x800u, 0); R(1, 0x800u, 0xFFFFF800u, 0);
	check(ext_ram_table_check(&b, &t) == -1, "a length that wraps", "table");
	TAB(0u);
	check(ext_ram_table_check(&b, &t) == -1, "no ranges", "table");
	TAB(EXT_RAM_RANGES_MAX + 1u);
	for (unsigned i = 0; i <= EXT_RAM_RANGES_MAX; i++)
		R(i, i * 0x100u, 0x100u, 0);
	b.size = (EXT_RAM_RANGES_MAX + 1u) * 0x100u;
	check(ext_ram_table_check(&b, &t) == -1, "too many ranges", "table");
	TAB(1u); R(0, 0, 0x1000u, 0); b.n_steps = EXT_RAM_STEPS_MAX + 1u; b.steps = steps;
	check(ext_ram_table_check(&b, &t) == -1, "too many steps", "table");
	TAB(1u); R(0, 0, 0x1000u, 0); b.n_steps = 1u;
	check(ext_ram_table_check(&b, &t) == -1, "steps counted but none given", "table");
	TAB(1u); R(0, 0, 0x1000u, 0); b.ready = NULL;
	check(ext_ram_table_check(&b, &t) == -1, "no ready()", "table");

	len = 7u;
	check(ext_ram_test_len(NULL, TESTABLE, &len) == 0 && len == TESTABLE, "none: all", "len");
	check(ext_ram_test_len("4", TESTABLE, &len) == 0 && len == 4u, "4", "len");
	check(ext_ram_test_len("0x80000", TESTABLE, &len) == 0 && len == TESTABLE,
	      "exactly all", "len");
	len = 7u;
	check(ext_ram_test_len("0", TESTABLE, &len) == -1 &&
	      ext_ram_test_len("3", TESTABLE, &len) == -1 &&
	      ext_ram_test_len("6", TESTABLE, &len) == -1 &&
	      ext_ram_test_len("0x80004", TESTABLE, &len) == -1 &&
	      ext_ram_test_len("0xFFFFFFFC", TESTABLE, &len) == -1 &&
	      ext_ram_test_len("4k", TESTABLE, &len) == -1 &&
	      ext_ram_test_len("", TESTABLE, &len) == -1 && len == 7u,
	      "0, odd, too long, garbage: refused, untouched", "len");

	check(ext_ram_spans(0x1000u, 0u, 0u, sp) == 1 && sp[0].off == 0u &&
	      sp[0].len == 0x1000u, "no skip", "spans");
	check(ext_ram_spans(0x1000u, 0x1000u, 0x100u, sp) == 1 && sp[0].len == 0x1000u,
	      "a skip at the end", "spans");
	check(ext_ram_spans(0x1000u, 0x400u, 0x400u, sp) == 2 && sp[0].off == 0u &&
	      sp[0].len == 0x400u && sp[1].off == 0x800u && sp[1].len == 0x800u,
	      "a skip in the middle", "spans");
	check(ext_ram_spans(0x600u, 0x400u, 0x400u, sp) == 1 && sp[0].len == 0x400u,
	      "ends inside the skip", "spans");
	check(ext_ram_spans(0x1000u, 0u, 0x400u, sp) == 1 && sp[0].off == 0x400u &&
	      sp[0].len == 0xC00u, "a skip at the start", "spans");
	check(ext_ram_spans(0x1000u, 0x400u, 0xFFFFFFFCu, sp) == 1 && sp[0].len == 0x400u,
	      "a skip that would wrap", "spans");
	check(ext_ram_spans(0x1000u, 0u, 0x1000u, sp) == 0, "all skipped", "spans");
	check(ext_ram_spans(0x1000u, 0x402u, 0x400u, sp) == -1 &&
	      ext_ram_spans(0x1000u, 0x400u, 0x3FEu, sp) == -1, "not on whole words", "spans");
}

int main(void)
{
	const struct cli_cmd *s;
	int dups = 0, rc;

	for (s = xram_table(); s != NULL && s->name != NULL; s++)
		for (const struct cli_cmd *t = xram_table(); t < s; t++)
			dups += strcmp(s->name, t->name) == 0;
	if (dups != 0) {
		printf("FAIL: a board subcommand has a shared name (%d twice)\n", dups);
		return 1;
	}
	check(xram_table() != NULL && xram_sub("info") != NULL &&
	      xram_sub("test") != NULL && xram_sub("tune") != NULL,
	      "info, test and the board's own", "xram");

	pure();

	/* ---- info ---- */
	reset();
	check(run("info") == 0 && seq[0] == '\0' && stores == 0 && loads == 0,
	      "info takes nothing, touches nothing", "info");
	check(has("device:  fake part\r\n") &&
	      has("range:   ") && has("non-cacheable, tested: low\r\n") &&
	      has("non-cacheable, tested: eth\r\n") &&
	      has("cacheable, not tested: nn\r\n") &&
	      has("non-cacheable, not tested: after\r\n") &&
	      has("state:   up\r\n") && has("  board   : line\r\n"),
	      "info: the ranges, the state, then the board's lines", "info");

	/* ---- the length: nothing of the board ---- */
	refused_len("test 0");
	refused_len("test 3");
	refused_len("test 6");
	refused_len("test 0x80004");
	refused_len("test 0xFFFFFFFC");
	refused_len("test x");

	/* ---- not ready ---- */
	reset();
	ready_ok = 0;
	check(run("test") == 1 && ready_calls == 1 && seq[0] == '\0' && stores == 0,
	      "not ready: nothing taken", "test");
	check(has("xram: not ready; see `xram info`"), "said", "test");

	/* ---- a take refused at each step ---- */
	{
		static const char *const want[] = { "T0 ", "T0 T1 G0 ", "", "T0 T1 T3 G2 G0 " };

		for (int k = 0; k < (int)NSTEPS; k++) {
			if (k == 2)
				continue;               /* no take */
			reset();
			refuse_at = k;
			span_refusal = (k == 3);
			check(run("test") == 1 && strcmp(seq, want[k]) == 0 && stores == 0 &&
			      loads == 0, "refused: the steps before it given back, reversed",
			      "test");
			check(has(k == 3 ? "xram: the fake refused (0x00001000..0x00001fff)\r\n"
			                 : "xram: the fake refused\r\n"), "the board's words", "test");
		}
	}

	/* ---- a pass ---- */
	reset();
	rc = run("test");
	check(rc == 0 && all_given() && stores == 3 * (long)(TESTABLE / 4u) &&
	      loads == stores, "every word three times, everything given back", "test");
	check(has("xram: DESTRUCTIVE test over 512 KB from 0x") &&
	      has("pass 1/3 (address): 512 KB OK\r\n") &&
	      has("pass 3/3 (0xAAAAAAAA): 512 KB OK\r\n") &&
	      !has("not tested") &&
	      has("xram: 512 KB tested, no errors\r\n") && !has("skipped"),
	      "the lines of a pass", "test");
	reset();
	check(run("test 4") == 0 && stores == 3 && loads == 3 && all_given() &&
	      has("xram: 4 B tested, no errors\r\n"), "4 bytes", "test 4");

	/* ---- a skip ---- */
	{
		static const struct { const char *line; uint32_t tested; int said; } c[] = {
			{ "test",          0x40000u, 1 },
			{ "test 0x40000",  0x40000u, 0 },   /* ends where it starts */
			{ "test 0x20000",  0x20000u, 0 },
			{ "test 0x40004",  0x40000u, 1 },
			{ "test 0x60000",  0x40000u, 1 },
		};

		for (unsigned i = 0; i < sizeof c / sizeof c[0]; i++) {
			reset();
			skip_off = 0x40000u;
			skip_len = 0x40000u;
			check(run(c[i].line) == 0 && all_given() &&
			      stores == 3 * (long)(c[i].tested / 4u), "skip: only the rest",
			      c[i].line);
			check(has("skipped: the fake's network") == c[i].said,
			      "skip: said when it is inside the test", c[i].line);
		}
		reset();
		skip_off = 0x40000u;
		skip_len = 0x40000u;
		run("test 0x60000");
		check(has("xram: 0x") && has("..0x") && has("skipped: the fake's network\r\n"),
		      "skip: where, said", "test 0x60000");
		reset();
		skip_off = 0u;
		skip_len = 0x80000u;
		check(run("test") == 1 && all_given() && stores == 0 &&
		      has("xram: nothing to test\r\n"), "all skipped: refused, given back",
		      "test");
		reset();
		skip_off = 0x40002u;
		skip_len = 0x40000u;
		check(run("test") == 1 && all_given() && stores == 0 &&
		      has("xram: bad skip\r\n"), "a bad skip: refused, given back", "test");
	}

	/* ---- a give that warns ---- */
	reset();
	warn_at = 2;
	check(run("test 0x1000") == 0 && all_given() &&
	      has("xram: the fake could not restore; run X\r\n"),
	      "a warning is said and the rest still given back", "test");

	/* ---- a bad word in each pass, an aliased address line ---- */
	for (long pass = 1; pass <= 3; pass++) {
		reset();
		flip_off = 0x1234u * 4u;
		flip_load = pass;
		check(run("test") == 1 && all_given() && has("FAIL at 0x") &&
		      !has("no errors"), "a bad word is found", "test");
		check(has(pass == 1 ? "pass 1/3 (address): FAIL" :
		          pass == 2 ? "pass 2/3 (0x55555555): FAIL" :
		                      "pass 3/3 (0xAAAAAAAA): FAIL"), "in its pass", "test");
	}
	reset();
	alias_bit = 0x10000u;
	check(run("test") == 1 && all_given() && has("pass 1/3 (address): FAIL"),
	      "an aliased address line: the address pass", "test");

	/* ---- Ctrl+C ---- */
	{
		long asks;

		reset();
		cancel_at = 1000000;
		check(run("test") == 0, "pass", "test");
		asks = cancel_asks;
		/* 8 x 64 KB written and 8 read, three times */
		check(asks == 3 * 2 * 8, "Ctrl+C asked after every 64 KB", "test");
		for (long k = 1; k <= asks; k++) {
			reset();
			cancel_at = k;
			check(run("test") == 1 && all_given() && has("cancelled") &&
			      !has("no errors"), "Ctrl+C stops the test", "test");
		}
	}

	/* ---- the board's own ---- */
	reset();
	check(run("tune") == 0 && tune_calls == 1, "the board's subcommand runs", "tune");

	printf("test_ext_ram_cmd: %s\n", failures ? "FAILED" : "OK");
	return failures ? 1 : 0;
}
