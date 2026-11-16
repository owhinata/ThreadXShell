/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Issue #251 -- the shared `sdram` on this board: shell/cmds/cmd_ext_ram.c and
 * this board's table (cmds/cmd_sdram.c), compiled unchanged with the board's
 * real headers (port/sdram/sdram.h, port/camera/camera.h,
 * port/ltdc/ltdc_display.h, port/netxduo/nx_glue.h, svc/ext_ram_config.h) and
 * reached through the root command table, over 8 MB mapped at 0xC0000000 and
 * counted stand-ins for the camera claim, the display hold and the network.
 * Built twice: with `.sdram.eth` inside bank2 (-Wl,--defsym, as the linker
 * places it) and, -DSDRAM_TEST_ETH_OUTSIDE, outside it.
 *
 * What it asks:
 *   - [!] every word stored or loaded (EXT_RAM_TEST_HOOKS) is in banks 0..2,
 *     never in bank3 (the cacheable nn bank), never in bank2 while the
 *     network is up, and made while the camera claim, the SDRAM owner word
 *     (as `sdram test`) and the display hold are all held (issue #249's
 *     lesson); the window outside what was tested keeps its fill;
 *   - the length: none is 6 MB; 8 MB (accepted until issue #251), 6 MB + 4, 0
 *     and 3 are refused before anything is taken; 6 MB is taken;
 *   - the steps: camera, owner word, network, display, given back display,
 *     owner word, camera; a refusal at each gives back exactly what was taken
 *     before it, in reverse, with the board's words;
 *   - `membench` and `sdram test` refuse each other through the one word: a
 *     test refuses while `membench` holds it, and during a test `membench`'s
 *     take sees the test;
 *   - with the network up, bank2 is skipped and said; with `.sdram.eth`
 *     outside bank2, refused with where it is, before the display is held;
 *   - the display's give repaints (one unhold per test), and a failed one is
 *     said while the rest is still given back;
 *   - Ctrl+C and a bad word give everything back;
 *   - `sdram info` says the three banks as the MPU maps them (bank3
 *     cacheable, not tested), the state and the controller line;
 *   - no name twice in the table.
 * Nothing of nn is defined here: a table that still took nn's claim would not
 * link.
 */
#include "cli.h"
#include "cli_instance.h"
#include "ext_ram_cmd_board.h"
#include "sdram_owner.h"
#include "sdram.h"
#include "camera.h"
#include "ltdc_display.h"
#include "nx_glue.h"
#include "stm32f7xx_hal.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#define WIN_WORDS (SDRAM_SIZE_BYTES / 4u)
#define BANK3     (SDRAM_BASE_ADDR + 3u * SDRAM_BANK_BYTES)
#define FILL      0xEEEEEEEEu

static volatile uint32_t *mem;   /* mapped at SDRAM_BASE_ADDR */

/* ---- the stand-ins -------------------------------------------------------- */

static char seq[128];
static int cam_refuse, cam_held, ltdc_rc, unhold_rc, ltdc_held, net_up, up = 1;
static int unholds, primask_depth;
static long stores, loads, bad_access, membench_saw_test, membench_took;
static long flip_load = -1, flip_seen;
static uint32_t flip_addr;

static void say(const char *s) { strncat(seq, s, sizeof seq - strlen(seq) - 1u); }

int camera_scratch_claim(void)
{
	if (cam_refuse)
		return -1;
	cam_held = 1;
	say("cam+ ");
	return 0;
}
void camera_scratch_release(void) { cam_held = 0; say("cam- "); }
int ltdc_scanout_hold(void)
{
	if (ltdc_rc != LTDC_OK)
		return ltdc_rc;
	ltdc_held = 1;
	say("ltdc+ ");
	return LTDC_OK;
}
int ltdc_scanout_unhold(void)
{
	ltdc_held = 0;
	unholds++;
	say("ltdc- ");
	return unhold_rc;
}
bool nx_net_is_up(void) { return net_up != 0; }
int sdram_is_up(void) { return up; }

uint32_t __get_PRIMASK(void) { return 0u; }
void __disable_irq(void) { primask_depth++; }
void __set_PRIMASK(uint32_t pm) { (void)pm; primask_depth--; }

/* Is the owner word the test's?  Asked the only way the board offers: a take
 * as `membench`, which must see the test -- and is given back if it did not. */
static int owner_is_test(void)
{
	enum sdram_owner h = sdram_owner_take(SDRAM_OWNER_MEMBENCH);

	if (h == SDRAM_OWNER_NONE) {
		membench_took++;
		sdram_owner_give();
		return 0;
	}
	if (h == SDRAM_OWNER_TEST)
		membench_saw_test++;
	return h == SDRAM_OWNER_TEST;
}

static void check_access(volatile uint32_t *p)
{
	uint32_t a = (uint32_t)(uintptr_t)p;

	if (a < SDRAM_BASE_ADDR || a >= SDRAM_BASE_ADDR + SDRAM_SIZE_BYTES) {
		printf("FAIL: an access outside the SDRAM window\n");
		exit(1);
	}
	if (a >= BANK3 || (net_up && a >= SDRAM_ETH_BANK_ADDR) ||
	    !cam_held || !ltdc_held)
		bad_access++;
	/* the owner word, on the first access and on every 64 KB boundary */
	if ((a & 0xFFFFu) == 0u && !owner_is_test())
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
		v ^= 0x00000100u;
	return v;
}

/* ---- the console ---------------------------------------------------------- */

static struct cli_instance vcp;
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

static const struct cli_cmd *sdram_sub(const char *name)
{
	CLI_ROOT_CMD_FOREACH(c)
		if (strcmp(c->name, "sdram") == 0)
			for (const struct cli_cmd *s = c->subcmds; s->name != NULL; s++)
				if (strcmp(s->name, name) == 0)
					return s;
	return NULL;
}

static void reset(void)
{
	seq[0] = '\0';
	cam_refuse = cam_held = ltdc_held = net_up = unholds = 0;
	ltdc_rc = unhold_rc = LTDC_OK;
	up = 1;
	stores = loads = bad_access = membench_saw_test = membench_took = 0;
	flip_load = -1;
	flip_seen = 0;
	flip_addr = 0u;
	cancel_at = cancel_asks = 0;
	text_len = 0;
	text[0] = '\0';
	for (uint32_t i = 0; i < WIN_WORDS; i++)
		mem[i] = FILL;
}

static int has(const char *s) { return strstr(text, s) != NULL; }

/* Words from @p from_off on still hold the fill (what was not tested). */
static int fill_from(uint32_t from_off, uint32_t to_off)
{
	for (uint32_t i = from_off / 4u; i < to_off / 4u; i++)
		if (mem[i] != FILL)
			return 0;
	return 1;
}

static int run(const char *line)
{
	static char buf[64];
	char *argv[4];
	int argc = 0, rc;
	const struct cli_cmd *s;

	snprintf(buf, sizeof buf, "%s", line);
	for (char *p = strtok(buf, " "); p != NULL && argc < 4; p = strtok(NULL, " "))
		argv[argc++] = p;
	s = sdram_sub(argv[0]);
	if (s == NULL)
		return -100;
	rc = s->handler(&vcp, argc, argv);
	check(bad_access == 0, "every access in banks 0..2, outside a skipped bank2, "
	      "with the camera, the owner word and the display held", line);
	check(membench_took == 0, "membench never got the word during a test", line);
	check(!cam_held && !ltdc_held, "camera and display given back", line);
	check(fill_from(3u * SDRAM_BANK_BYTES, SDRAM_SIZE_BYTES), "bank3 untouched", line);
	check(primask_depth == 0, "PRIMASK restored", line);
	/* the owner word is free again: membench can take it */
	check(sdram_owner_take(SDRAM_OWNER_MEMBENCH) == SDRAM_OWNER_NONE,
	      "the owner word given back", line);
	sdram_owner_give();
	return rc;
}

#ifndef SDRAM_TEST_ETH_OUTSIDE
static const char all_steps[] = "cam+ ltdc+ ltdc- cam- ";
#endif

int main(void)
{
	const struct cli_cmd *s;
	uint32_t t;
	int dups = 0;

	mem = mmap((void *)(uintptr_t)SDRAM_BASE_ADDR, SDRAM_SIZE_BYTES,
	           PROT_READ | PROT_WRITE,
	           MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
	if (mem != (volatile uint32_t *)(uintptr_t)SDRAM_BASE_ADDR) {
		printf("FAIL: could not map the SDRAM window at 0x%08X\n", SDRAM_BASE_ADDR);
		return 1;
	}

	CLI_ROOT_CMD_FOREACH(c)
		if (strcmp(c->name, "sdram") == 0)
			for (s = c->subcmds; s->name != NULL; s++)
				for (const struct cli_cmd *u = c->subcmds; u < s; u++)
					dups += strcmp(s->name, u->name) == 0;
	check(sdram_sub("info") != NULL && sdram_sub("test") != NULL && dups == 0,
	      "info and test, no name twice", "sdram");
	check(ext_ram_table_check(&ext_ram_board, &t) == 0 && t == 6u * 1024u * 1024u,
	      "the table: a test reaches 6 MB, where the cacheable bank3 starts", "table");

#ifdef SDRAM_TEST_ETH_OUTSIDE
	reset();
	net_up = 1;
	check(run("test") == 1 && strcmp(seq, "cam+ cam- ") == 0 && stores == 0,
	      ".sdram.eth outside bank2: refused before the display, nothing written",
	      "test");
	check(has("sdram: .sdram.eth is not inside bank2 (0xc0300000..0xc0500000)\r\n"),
	      "where it is, said", "test");
	reset();
	check(run("test 0x1000") == 0, "network down: the place does not matter", "test");
	printf("test_sdram_board (.sdram.eth outside bank2): %s\n",
	       failures ? "FAILED" : "OK");
	return failures ? 1 : 0;
#else
	/* ---- info ---- */
	reset();
	check(run("info") == 0 && seq[0] == '\0' && stores == 0 && loads == 0,
	      "info takes nothing, touches nothing", "info");
	check(has("device:  MT48LC4M32B2 (128 Mbit, 16-bit bus -> 8 MB usable)\r\n") &&
	      has("window:  0xc0000000..0xc07fffff\r\n") &&
	      has("range:   0xc0000000..0xc03fffff non-cacheable, tested: banks 0-1") &&
	      has("range:   0xc0400000..0xc05fffff non-cacheable, tested: bank2") &&
	      has("range:   0xc0600000..0xc07fffff cacheable, not tested: bank3: nn\r\n") &&
	      has("state:   up\r\n") && has("config:  SDCLK 108 MHz"),
	      "info: the banks as the MPU maps them, the state, the controller", "info");

	/* ---- the length ---- */
	{
		static const char *const bad[] = { "test 0x800000", "test 0x600004",
		                                   "test 0", "test 3" };

		for (unsigned i = 0; i < 4; i++) {
			reset();
			check(run(bad[i]) == 1 && seq[0] == '\0' && stores == 0,
			      "refused before anything is taken", bad[i]);
			check(has("sdram: bad length (4-byte multiple, 4..6291456)\r\n"),
			      "said", bad[i]);
		}
	}
	reset();
	up = 0;
	check(run("test") == 1 && seq[0] == '\0' && has("sdram: not ready"),
	      "SDRAM down: nothing taken", "test");

	/* ---- a whole test, network down ---- */
	reset();
	check(run("test") == 0 && strcmp(seq, all_steps) == 0 && unholds == 1,
	      "every step taken and given back once, the display repainted", "test");
	check(stores == 3L * (6L << 20) / 4 && loads == stores && membench_saw_test > 0,
	      "6 MB three times; membench saw the test meanwhile", "test");
	check(has("sdram: DESTRUCTIVE test over 6144 KB from 0xc0000000\r\n") &&
	      has("pass 3/3 (0xAAAAAAAA): 6144 KB OK\r\n") &&
	      has("sdram: 6144 KB tested, no errors\r\n") && !has("skipped"),
	      "the lines", "test");
	reset();
	check(run("test 0x600000") == 0, "6 MB given as a number", "test");

	/* ---- network up: bank2 skipped ---- */
	reset();
	net_up = 1;
	check(run("test") == 0 && strcmp(seq, all_steps) == 0 &&
	      stores == 3L * (4L << 20) / 4 &&
	      fill_from(2u * SDRAM_BANK_BYTES, SDRAM_SIZE_BYTES),
	      "network up: banks 0..1 only, bank2 keeps its contents", "test");
	check(has("sdram: 0xc0400000..0xc05fffff skipped: the network is up (ETH DMA)\r\n") &&
	      has("sdram: 4096 KB tested, no errors\r\n"), "the skip, said", "test");
	reset();
	net_up = 1;
	check(run("test 0x100000") == 0 && !has("skipped"),
	      "network up, a test below bank2: nothing to say", "test");

	/* ---- refusals at each step ---- */
	reset();
	cam_refuse = 1;
	check(run("test") == 1 && seq[0] == '\0' && stores == 0 &&
	      has("sdram: the camera is streaming, or another test holds it; "
	          "`nn stream stop`, then `camera stream stop`\r\n"),
	      "camera refused: nothing taken", "test");
	reset();
	check(sdram_owner_take(SDRAM_OWNER_MEMBENCH) == SDRAM_OWNER_NONE, "membench runs",
	      "membench");
	{
		static char line[] = "test";
		char *argv[1] = { line };
		/* not run(): it would want the word free afterwards */
		int rc = sdram_sub("test")->handler(&vcp, 1, argv);

		check(rc == 1 && strcmp(seq, "cam+ cam- ") == 0 && stores == 0 &&
		      has("sdram: `membench` is running\r\n") && !cam_held,
		      "membench running: refused, the camera given back", "test");
		check(sdram_owner_take(SDRAM_OWNER_TEST) == SDRAM_OWNER_MEMBENCH,
		      "membench still holds the word", "test");
	}
	sdram_owner_give();
	/* Another test holds the word (the camera claim lets a second test
	 * through when the camera never came up): named as such, not membench. */
	reset();
	check(sdram_owner_take(SDRAM_OWNER_TEST) == SDRAM_OWNER_NONE, "a test runs",
	      "test");
	{
		static char line[] = "test";
		char *argv[1] = { line };
		int rc = sdram_sub("test")->handler(&vcp, 1, argv);

		check(rc == 1 && strcmp(seq, "cam+ cam- ") == 0 && stores == 0 &&
		      has("sdram: another `sdram test` is running\r\n") &&
		      !has("membench"), "a second test: refused as such", "test");
	}
	sdram_owner_give();
	reset();
	ltdc_rc = LTDC_ERR_STATE;
	check(run("test") == 1 && strcmp(seq, "cam+ cam- ") == 0 && stores == 0 &&
	      has("sdram: GUIX owns the display; `gui stop` first\r\n"),
	      "GUIX: refused, owner and camera given back", "test");
	reset();
	ltdc_rc = -1;
	check(run("test") == 1 && strcmp(seq, "cam+ cam- ") == 0 &&
	      has("sdram: could not hold the display\r\n"), "display: refused", "test");

	/* ---- the display's give fails; Ctrl+C; a bad word ---- */
	reset();
	unhold_rc = LTDC_ERR_STATE;
	check(run("test 0x1000") == 0 && strcmp(seq, all_steps) == 0 &&
	      has("sdram: display restore failed; `lcd on`\r\n"),
	      "a failed repaint is said, the rest given back", "test");
	reset();
	cancel_at = 5;
	check(run("test") == 1 && strcmp(seq, all_steps) == 0 && has("cancelled") &&
	      unholds == 1, "Ctrl+C: everything given back, repainted", "test");
	reset();
	flip_addr = SDRAM_BASE_ADDR + 0x2C0000u;
	flip_load = 2;
	check(run("test") == 1 && strcmp(seq, all_steps) == 0 &&
	      has("pass 2/3 (0x55555555): FAIL at 0xc02c0000: got 0x55555455, "
	          "want 0x55555555\r\n"),
	      "a bad word: said, everything given back", "test");

	printf("test_sdram_board: %s\n", failures ? "FAILED" : "OK");
	return failures ? 1 : 0;
#endif
}
