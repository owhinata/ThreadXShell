/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Issue #251 -- the shared `psram` on this board: shell/cmds/cmd_ext_ram.c and
 * this board's table and tuning subcommands (cmds/cmd_psram.c), compiled
 * unchanged with the board's real headers (src/psram.h, port/camera/camera.h,
 * port/ltdc/ltdc_display.h, svc/ext_ram_config.h) and reached through the root
 * command table, over 8 MB mapped at 0x90000000 and counted stand-ins for the
 * OCTOSPI1 guard, the camera frame and the LCD repaint.  Built with
 * BSP_ENABLE_LCD 1 and 0.
 *
 * What it asks:
 *   - [!] every word stored or loaded (EXT_RAM_TEST_HOOKS) is in the low 6 MB
 *     (never in the cacheable 2 MB), and made while the reconfigurer mark is
 *     held, after the captured frame was dropped and before the repaint;
 *   - the order: mark, frame, (repaint), mark given back -- the repaint
 *     before the mark goes back, on every end (pass, Ctrl+C, a bad word, a
 *     refusal after the mark); without the LCD there is no repaint;
 *   - the length: none is 6 MB; 0, 3, 6 MB + 4 and 8 MB are refused before
 *     the mark is asked for (until issue #251 they were clamped or rounded);
 *   - the mark refused: nothing else, the board's words; not ready: nothing;
 *   - `psram info`: the shared lines, then the board's bring-up lines, with
 *     the init line only when not ready;
 *   - the six tuning subcommands are in the table after `info` and `test`, no
 *     name is twice, and they still take and give the mark.
 */
#include "cli.h"
#include "cli_instance.h"
#include "ext_ram_cmd_board.h"
#include "psram.h"
#include "camera.h"
#if BSP_ENABLE_LCD
#include "ltdc_display.h"
#endif

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#define FILL 0xEEEEEEEEu

static volatile uint32_t *mem;   /* mapped at PSRAM_BASE_ADDR */

/* ---- the stand-ins -------------------------------------------------------- */

static char seq[128];
static int mark, refuse_mark, frame_dropped, painted, ready = 1;
static int acquires, releases, sets, repaints, cleaned, cleans, unclean_sets;
static long stores, loads, bad_access;
static long flip_load = -1, flip_seen;
static uint32_t flip_addr;

static void say(const char *s) { strncat(seq, s, sizeof seq - strlen(seq) - 1u); }

int psram_ready(void) { return ready; }
int psram_acquire(void)
{
	acquires++;
	if (refuse_mark || mark)
		return 0;
	mark = 1;
	say("mark+ ");
	return 1;
}
void psram_release(void)
{
	releases++;
	mark = 0;
	cleaned = 0;
	say("mark- ");
}
/* [!] #252: the D-cache written back under the mark, before any change */
void psram_clean_dcache(void)
{
	cleans++;
	if (!mark)
		bad_access++;
	cleaned = 1;
	say("clean ");
}
/* A tuning change made without the mark, or before the clean. */
static void tuned(void)
{
	sets++;
	if (!mark || !cleaned)
		unclean_sets++;
}
void camera_frame_invalidate(void)
{
	if (!mark)
		bad_access++;          /* dropped outside the mark */
	frame_dropped = 1;
	say("frame ");
}
#if BSP_ENABLE_LCD
void ltdc_fb_repaint_black(void)
{
	if (!mark)
		bad_access++;          /* [!] repainted after the mark went back */
	painted = 1;
	repaints++;
	say("paint ");
}
#endif
uint32_t psram_init_stage(void) { return PSRAM_STAGE_MR0; }
uint32_t psram_get_dqse(void) { return 1u; }
uint32_t psram_get_refresh(void) { return 2u; }
uint32_t psram_clock_hz(void) { return 133000000u; }
void psram_set_prescaler(uint32_t p) { (void)p; tuned(); }
void psram_read_id(uint8_t id[2]) { id[0] = 0x8Du; id[1] = 0x93u; }
void psram_get_latency(uint32_t *rd, uint32_t *wr) { *rd = 8u; *wr = 4u; }
void psram_set_latency(uint32_t rd, uint32_t wr) { (void)rd; (void)wr; tuned(); }
uint32_t psram_get_instruction_dtr(void) { return 0u; }
void psram_get_phase(uint32_t *sel, uint32_t *unit) { *sel = 3u; *unit = 8u; }
void psram_set_phase(uint32_t sel, uint32_t unit) { (void)sel; (void)unit; tuned(); }
uint32_t psram_get_read_flags(void) { return 0u; }
uint32_t psram_get_mr0(void) { return 0x24u; }
void psram_set_mr0(uint32_t v) { (void)v; tuned(); }
void psram_mmapscan_stop(void) {}
int psram_mmapscan_get(struct psram_scan_state *out) { (void)out; return 0; }
int psram_mmapscan_start(uint32_t phase, uint32_t ulo, uint32_t ustep, uint32_t n)
{ (void)phase; (void)ulo; (void)ustep; (void)n; tuned(); return 0; }
/* `psram wtune` (issue #252): what the sweep returns, and in what order the
 * command reached it. */
static int wt_rc, wt_calls, wt_frame_first, wt_mark_held;
static uint32_t wt_mask, wt_off;
int psram_wtune(uint32_t off, uint32_t reps, uint32_t *mask)
{
	(void)reps;
	wt_calls++;
	wt_off = off;
	wt_frame_first = frame_dropped && cleaned; /* [!] cleaned, frame dropped first */
	wt_mark_held = mark;
	*mask = (wt_rc == PSRAM_WTUNE_REFUSED) ? 0u : wt_mask;
	say("wtune ");
	return wt_rc;
}

static void check_access(volatile uint32_t *p)
{
	uint32_t a = (uint32_t)(uintptr_t)p;

	if (a < PSRAM_BASE_ADDR || a >= PSRAM_BASE_ADDR + PSRAM_SIZE_BYTES) {
		printf("FAIL: an access outside the PSRAM window\n");
		exit(1);
	}
	if (a >= PSRAM_AI_BASE_ADDR || !mark || !frame_dropped || painted)
		bad_access++;
}

void ext_ram_test_store(volatile uint32_t *p, uint32_t v)
{
	check_access(p);
	stores++;
	*p = v;
}

uint32_t ext_ram_test_load(volatile uint32_t *p)
{
	uint32_t v;

	check_access(p);
	loads++;
	v = *p;
	if ((uint32_t)(uintptr_t)p == flip_addr && ++flip_seen == flip_load)
		v ^= 0x80000000u;
	return v;
}

/* ---- the console ---------------------------------------------------------- */

static struct cli_instance usb;
static char text[8192];
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

static const struct cli_cmd *psram_table(void)
{
	CLI_ROOT_CMD_FOREACH(c)
		if (strcmp(c->name, "psram") == 0)
			return c->subcmds;
	return NULL;
}

static const struct cli_cmd *psram_sub(const char *name)
{
	for (const struct cli_cmd *s = psram_table(); s != NULL && s->name != NULL; s++)
		if (strcmp(s->name, name) == 0)
			return s;
	return NULL;
}

static void reset(void)
{
	seq[0] = '\0';
	mark = refuse_mark = frame_dropped = painted = cleaned = 0;
	cleans = unclean_sets = 0;
	ready = 1;
	acquires = releases = sets = 0;
	stores = loads = bad_access = 0;
	flip_load = -1;
	flip_seen = 0;
	flip_addr = 0u;
	cancel_at = cancel_asks = 0;
	wt_rc = PSRAM_WTUNE_OK;
	wt_calls = wt_frame_first = wt_mark_held = repaints = 0;
	wt_mask = 0x10u;
	wt_off = 0u;
	text_len = 0;
	text[0] = '\0';
	for (uint32_t i = 0; i < PSRAM_SIZE_BYTES / 4u; i++)
		mem[i] = FILL;
}

static int has(const char *s) { return strstr(text, s) != NULL; }

static int top_untouched(void)
{
	for (uint32_t i = PSRAM_NC_BYTES / 4u; i < PSRAM_SIZE_BYTES / 4u; i++)
		if (mem[i] != FILL)
			return 0;
	return 1;
}

static int run(const char *line)
{
	static char buf[64];
	char *argv[4];
	int argc = 0, rc, held_before;
	const struct cli_cmd *s;

	snprintf(buf, sizeof buf, "%s", line);
	for (char *p = strtok(buf, " "); p != NULL && argc < 4; p = strtok(NULL, " "))
		argv[argc++] = p;
	s = psram_sub(argv[0]);
	if (s == NULL)
		return -100;
	held_before = mark;
	rc = s->handler(&usb, argc, argv);
	check(bad_access == 0, "every access in the low 6 MB, under the mark, after the "
	      "frame was dropped and before the repaint", line);
	/* (wtune keeps the mark on purpose; a command run behind a kept mark is
	 * refused and leaves it) */
	if (strncmp(line, "wtune", 5) != 0 && !held_before)
		check(!mark && acquires - releases == (refuse_mark ? acquires : 0),
		      "the mark given back once for every take", line);
	check(top_untouched(), "the cacheable 2 MB untouched", line);
	return rc;
}

#if BSP_ENABLE_LCD
static const char all_steps[] = "mark+ frame paint mark- ";
#else
static const char all_steps[] = "mark+ frame mark- ";
#endif

int main(void)
{
	static const char *const names[] = { "info", "test", "set", "mr0", "phase",
	                                     "mmapscan", "wtune", "clk" };
	const struct cli_cmd *t = NULL;
	unsigned n = 0;
	int dups = 0;
	uint32_t testable;

	mem = mmap((void *)(uintptr_t)PSRAM_BASE_ADDR, PSRAM_SIZE_BYTES,
	           PROT_READ | PROT_WRITE,
	           MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
	if (mem != (volatile uint32_t *)(uintptr_t)PSRAM_BASE_ADDR) {
		printf("FAIL: could not map the PSRAM window at 0x%08X\n", PSRAM_BASE_ADDR);
		return 1;
	}

	/* ---- the table ---- */
	for (const struct cli_cmd *s = psram_table(); s != NULL && s->name != NULL; s++, n++) {
		for (t = psram_table(); t < s; t++)
			dups += strcmp(s->name, t->name) == 0;
		check(n < 8u && strcmp(s->name, names[n]) == 0, "info, test, then the six",
		      s->name);
	}
	check(n == 8u && dups == 0, "eight subcommands, no name twice", "psram");
	check(ext_ram_table_check(&ext_ram_board, &testable) == 0 &&
	      testable == PSRAM_NC_BYTES, "a test reaches the non-cacheable 6 MB", "table");

	/* ---- info ---- */
	reset();
	check(run("info") == 0 && seq[0] == '\0' && stores == 0, "info takes nothing", "info");
	check(has("device:  APS6408 on OCTOSPI1\r\n") &&
	      has("window:  0x90000000..0x907fffff\r\n") &&
	      has("range:   0x90000000..0x905fffff non-cacheable, tested: LCD, camera\r\n") &&
	      has("range:   0x90600000..0x907fffff cacheable, not tested: nn, membench\r\n") &&
	      has("state:   up\r\n") && !has("init    :") &&
	      has("  clock   : 133000000 Hz (device)\r\n") &&
	      has("  id      : MR1=0x8D MR2=0x93\r\n") &&
	      has("  latency : read 8 / write 4 dummy cycles\r\n") &&
	      has("  dlyb    : phase 3 / units 8\r\n") &&
	      has("  mr0     : 0x24   read-flags 0x0 (b0=SSHIFT b1=DHQC)\r\n") &&
	      has("  ifmt    : 8-bit SDR official instruction\r\n") &&
	      has("  probe   : dqse 1 / refresh 2 (DCR4)\r\n"),
	      "info: the shared lines, then the board's", "info");
	reset();
	ready = 0;
	check(run("info") == 0 && has("state:   DOWN\r\n") &&
	      has("  init    : failed at MA0 pair read\r\n"),
	      "info, not ready: the init line", "info");

	/* ---- the length ---- */
	{
		static const char *const bad[] = { "test 0", "test 3", "test 0x600004",
		                                   "test 0x800000", "test 6m" };

		for (unsigned i = 0; i < 5; i++) {
			reset();
			check(run(bad[i]) == 1 && acquires == 0 && seq[0] == '\0',
			      "refused before the mark is asked for", bad[i]);
			check(has("psram: bad length (4-byte multiple, 4..6291456)\r\n"),
			      "said", bad[i]);
		}
	}
	reset();
	ready = 0;
	check(run("test") == 1 && acquires == 0 && has("psram: not ready; see `psram info`"),
	      "not ready: nothing", "test");

	/* ---- a whole test ---- */
	reset();
	check(run("test") == 0 && strcmp(seq, all_steps) == 0 && cleans == 0,
	      "mark, frame, (repaint), mark given back; no cache clean", "test");
	check(stores == 3L * (long)(PSRAM_NC_BYTES / 4u) && loads == stores,
	      "6 MB three times", "test");
	check(has("psram: DESTRUCTIVE test over 6144 KB from 0x90000000\r\n") &&
	      has("pass 1/3 (address): 6144 KB OK\r\n") &&
	      has("psram: 6144 KB tested, no errors\r\n"), "the lines", "test");
	reset();
	check(run("test 4096") == 0 && strcmp(seq, all_steps) == 0 && stores == 3L * 1024,
	      "4096 bytes", "test 4096");

	/* ---- refused, Ctrl+C, a bad word ---- */
	reset();
	refuse_mark = 1;
	check(run("test") == 1 && seq[0] == '\0' && stores == 0 && !frame_dropped &&
	      has("psram: OCTOSPI1 busy (another psram/membench command or `nn stream` "
	          "holds it, or the LCD is scanning out of it -- run `lcd off`)\r\n"),
	      "the mark refused: nothing else", "test");
	reset();
	cancel_at = 7;
	check(run("test") == 1 && strcmp(seq, all_steps) == 0 && has("cancelled"),
	      "Ctrl+C: repainted, then the mark given back", "test");
	reset();
	flip_addr = PSRAM_BASE_ADDR + 0x12340u;
	flip_load = 3;
	check(run("test") == 1 && strcmp(seq, all_steps) == 0 &&
	      has("pass 3/3 (0xAAAAAAAA): FAIL at 0x90012340: got 0x2aaaaaaa, "
	          "want 0xaaaaaaaa\r\n"),
	      "a bad word: said, repainted, the mark given back", "test");

	/* ---- the tuning subcommands still take and give the mark ---- */
	reset();
	check(run("set 8 4") == 0 && acquires == 1 && releases == 1 && sets == 1,
	      "set: under the mark", "set 8 4");
	reset();
	refuse_mark = 1;
	check(run("clk 1") == 1 && sets == 0 && has("OCTOSPI1 busy"),
	      "clk refused: nothing set", "clk 1");



	/* ---- #252: every tuning change is made under the mark, after the clean ---- */
	{
		static const char *const lines[] = { "set 8 4", "mr0 0x24", "phase 3 8",
		                                     "clk 1", "mmapscan start 3" };

		for (unsigned i = 0; i < sizeof lines / sizeof lines[0]; i++) {
			reset();
			run(lines[i]);
			check(cleans == 1 && sets == 1 && unclean_sets == 0,
			      "the D-cache cleaned under the mark, before the change", lines[i]);
		}
	}
	reset();
	check(run("info") == 0 && !has("held    :"), "info: no kept mark, no line", "info");

	/* ---- issue #252: `psram wtune` ---- */
	{
		static const struct { uint32_t off; int ok; } offs[] = {
			{ 0u, 1 }, { 0x200u, 1 }, { 0x5FFFF8u, 1 }, { 0x5FFFFCu, 0 },
			{ 0x600000u, 0 }, { 0x7FFFF8u, 0 }, { 0x202u, 0 }, { 0x201u, 0 },
			{ 0xFFFFFFFCu, 0 }, { 0xFFFFFFF8u, 0 },
		};

		for (unsigned i = 0; i < sizeof offs / sizeof offs[0]; i++)
			check(psram_wtune_off_ok(offs[i].off) == offs[i].ok,
			      "the wtune offset: aligned, all 8 bytes in the 6 MB", "pure");
		check(psram_wtune_end(PSRAM_WTUNE_OK, 1u) == PSRAM_WTUNE_END_REPAINT_GIVE &&
		      psram_wtune_end(PSRAM_WTUNE_OK, 0u) == PSRAM_WTUNE_END_KEEP &&
		      psram_wtune_end(PSRAM_WTUNE_DEAD, 0u) == PSRAM_WTUNE_END_KEEP &&
		      psram_wtune_end(PSRAM_WTUNE_DEAD, 3u) == PSRAM_WTUNE_END_KEEP &&
		      psram_wtune_end(PSRAM_WTUNE_REFUSED, 0u) == PSRAM_WTUNE_END_GIVE &&
		      psram_wtune_end(99, 1u) == PSRAM_WTUNE_END_KEEP,
		      "the end after the sweep", "pure");
	}
	{
		static const char *const bad[] = { "wtune 0x90600000", "wtune 0x905FFFFC",
		                                   "wtune 0x90000202", "wtune 0x8FFFFFF0",
		                                   "wtune 0x907FFFF8", "wtune 0" };

		for (unsigned i = 0; i < sizeof bad / sizeof bad[0]; i++) {
			reset();
			check(run(bad[i]) == 1 && acquires == 0 && wt_calls == 0 &&
			      !frame_dropped && has("psram: wtune address: a multiple of 4 in "
			                            "0x90000000..0x905FFFF8\r\n"),
			      "wtune: a bad address refused before the mark", bad[i]);
		}
		reset();
		ready = 0;
		check(run("wtune") == 1 && acquires == 0 && wt_calls == 0 &&
		      has("psram: not ready\r\n"), "wtune: not ready", "wtune");
		reset();
		refuse_mark = 1;
		check(run("wtune") == 1 && wt_calls == 0 && !frame_dropped &&
		      has("OCTOSPI1 busy"), "wtune: the mark refused, nothing else", "wtune");
	}
	reset();
	check(run("wtune 0x905FFFF8") == 0 && wt_off == 0x5FFFF8u && wt_frame_first &&
	      wt_mark_held && !mark && releases == 1 &&
	      repaints == (BSP_ENABLE_LCD ? 1 : 0) &&
	      strcmp(seq, BSP_ENABLE_LCD ? "mark+ clean frame wtune paint mark- "
	                                 : "mark+ clean frame wtune mark- ") == 0 &&
	      has("  wr dummy  4: PASS\r\n") &&
	      has("psram wtune: lowest passing dummy applied; run `psram test 4096`\r\n"),
	      "wtune passed: frame dropped first, repainted, then the mark given back",
	      "wtune");
	reset();
	wt_rc = PSRAM_WTUNE_DEAD;
	wt_mask = 0x3u;
	check(run("wtune") == 1 && mark && releases == 0 && repaints == 0 &&
	      has("psram wtune: device stopped answering; OCTOSPI1 held until `reboot`\r\n"),
	      "wtune, device dead: no repaint, the mark kept", "wtune");
	mark = 0;
	reset();
	wt_mask = 0u;
	check(run("wtune") == 1 && mark && releases == 0 && repaints == 0 &&
	      has("psram wtune: nothing round-tripped; OCTOSPI1 held until `reboot`\r\n"),
	      "wtune, nothing round-tripped: no repaint, the mark kept", "wtune");
	text_len = 0;
	text[0] = '\0';
	check(run("set 8 4") == 1 && sets == 0 && has("OCTOSPI1 busy"),
	      "after a kept mark every reconfigurer is refused", "set 8 4");
	text_len = 0;
	text[0] = '\0';
	check(run("info") == 0 &&
	      has("  held    : OCTOSPI1, since a failed wtune; `reboot`\r\n"),
	      "info says the mark is kept", "info");
	mark = 0;
	reset();
	wt_rc = PSRAM_WTUNE_REFUSED;
	check(run("wtune") == 1 && !mark && releases == 1 && repaints == 0 &&
	      has("psram: wtune refused\r\n"), "wtune refused below: the mark given back",
	      "wtune");

	printf("test_psram_board (LCD %d): %s\n", BSP_ENABLE_LCD, failures ? "FAILED" : "OK");
	return failures ? 1 : 0;
}
