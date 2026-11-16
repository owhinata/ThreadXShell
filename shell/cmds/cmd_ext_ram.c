/* shared-command: optional BOARD_HAS_EXT_RAM */
/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    cmd_ext_ram.c
 * @brief   The one external RAM command for every board that has some (issue
 *          #251, epic #213; the contract is ext_ram_cmd_board.h): `sdram` on
 *          f746g-disco, `psram` on wio-lite-ai.
 *
 *   <name> info           the part, the window, its ranges, the state, the
 *                         board's lines
 *   <name> test [bytes]   DESTRUCTIVE write / read-back test from the window's
 *                         start (default: every byte a test may touch)
 *   ...                   the board's own (EXT_RAM_BOARD_SUBCMDS)
 *
 * The name, the help and the board's subcommands come from the board's
 * ext_ram_config.h.  Until issue #251 each board had its own command
 * (f746g-disco's cmds/cmd_sdram.c, wio-lite-ai's cmds/cmd_psram.c), with two
 * lengths, two outputs and two ways to stop.
 *
 * WHAT A TEST TOUCHES.  [0, len) of the window, len a multiple of 4, not 0
 * and not past where the first cacheable range starts (the board's table,
 * ext_ram_cmd_board.h); minus the run a take asked to skip.  Nothing is
 * rounded or clamped: a length outside that is refused before anything of the
 * board is called.
 *
 * Three passes, word by word: (1) each word holds its own address (stuck,
 * shorted or aliased address lines, which one repeated value would miss on a
 * 16-bit bus), (2) 0x55555555, (3) 0xAAAAAAAA (stuck or coupled data lines).
 * Each pass writes every span, then reads every span back, so the read-back
 * comes a whole pass after the write (open rows pushed out, refresh crossed).
 * Ctrl+C is asked after each 64 KB written and each 64 KB read.
 *
 * Every line is one call of the output (a background job's line cannot land
 * in the middle of one): a pass says its result when it is over.
 *
 * [!] A SHARED TRANSLATION UNIT OWNS NO MUTABLE STORAGE
 * (cmake/check_no_mutable_storage.py, per board): the claim and the spans are
 * on the caller's stack.
 *
 * Clean-room design; no third-party code reused.
 */
#include "cli.h"
#include "ext_ram_cmd_board.h"
#include "ext_ram_config.h"   /* the board's name, help and subcommands */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#if !defined(EXT_RAM_CMD) || !defined(EXT_RAM_HELP) || \
    !defined(EXT_RAM_BOARD_DECLS) || !defined(EXT_RAM_BOARD_SUBCMDS)
#error "ext_ram_config.h must define EXT_RAM_CMD, EXT_RAM_HELP, EXT_RAM_BOARD_DECLS and EXT_RAM_BOARD_SUBCMDS"
#endif

/* "<name>: ", glued onto every message at compile time. */
#define EXT_RAM_STR_(x) #x
#define EXT_RAM_STR(x)  EXT_RAM_STR_(x)
#define EXT_RAM_PFX     EXT_RAM_STR(EXT_RAM_CMD) ": "

/* The board's own subcommand handlers (see EXT_RAM_BOARD_SUBCMDS). */
EXT_RAM_BOARD_DECLS

/* Ctrl+C is asked after this many words (64 KB). */
#define EXT_RAM_CHUNK_WORDS (16u * 1024u)

/*
 * The word accesses.  A host test defines EXT_RAM_TEST_HOOKS and its own
 * pair, to count and check every access; the firmware's are plain volatile
 * loads and stores.
 */
#ifdef EXT_RAM_TEST_HOOKS
void ext_ram_test_store(volatile uint32_t *p, uint32_t v);
uint32_t ext_ram_test_load(volatile uint32_t *p);
#define EXT_RAM_STORE(p, v) ext_ram_test_store((p), (v))
#define EXT_RAM_LOAD(p)     ext_ram_test_load(p)
#else
#define EXT_RAM_STORE(p, v) (*(p) = (v))
#define EXT_RAM_LOAD(p)     (*(p))
#endif

/* ---- the pure functions (ext_ram_cmd_board.h) ----------------------------- */

int ext_ram_table_check(const struct ext_ram_board *b, uint32_t *testable)
{
	uint32_t at = 0u, first_cached;
	unsigned k;

	*testable = 0u;
	if (b->ranges == NULL || b->n_ranges == 0u ||
	    b->n_ranges > EXT_RAM_RANGES_MAX || b->n_steps > EXT_RAM_STEPS_MAX ||
	    (b->n_steps != 0u && b->steps == NULL) || b->ready == NULL)
		return -1;
	first_cached = b->size;
	for (k = 0u; k < b->n_ranges; k++) {
		const struct ext_ram_range *r = &b->ranges[k];

		if (r->off != at || r->len == 0u || ((r->off | r->len) & 3u) != 0u ||
		    r->len > b->size - at)
			return -1;
		if (r->cached && first_cached == b->size)
			first_cached = r->off;
		at += r->len;
	}
	if (at != b->size || first_cached == 0u)
		return -1;
	*testable = first_cached;
	return 0;
}

int ext_ram_test_len(const char *arg, uint32_t testable, uint32_t *len)
{
	uint32_t n;

	if (arg == NULL) {
		*len = testable;
		return 0;
	}
	if (cli_parse_u32(arg, &n) != 0 || n == 0u || (n & 3u) != 0u ||
	    n > testable)
		return -1;
	*len = n;
	return 0;
}

int ext_ram_spans(uint32_t len, uint32_t skip_off, uint32_t skip_len,
                  struct ext_ram_span out[2])
{
	uint32_t skip_end;
	int n = 0;

	if (((skip_off | skip_len) & 3u) != 0u)
		return -1;
	/* No skip, or one at or past the end: all of it. */
	if (skip_len == 0u || skip_off >= len) {
		out[0].off = 0u;
		out[0].len = len;
		return 1;
	}
	skip_end = (skip_len > len - skip_off) ? len : skip_off + skip_len;
	if (skip_off > 0u) {
		out[n].off = 0u;
		out[n].len = skip_off;
		n++;
	}
	if (skip_end < len) {
		out[n].off = skip_end;
		out[n].len = len - skip_end;
		n++;
	}
	return n;
}

/* ---- the words ------------------------------------------------------------ */

static unsigned long ext_ram_addr(uint32_t off)
{
	return (unsigned long)(ext_ram_board.base + off);
}

/* A byte count in KB, or in B when it is not whole KB. */
#define EXT_RAM_KB(n)      (unsigned long)(((n) & 1023u) ? (n) : (n) >> 10)
#define EXT_RAM_KB_UNIT(n) (((n) & 1023u) ? "B" : "KB")

/* One range of `info`: where, whether cacheable, whether a test touches it,
 * what is in it.  (A test does not repeat the ranges it leaves alone.) */
static void ext_ram_range_line(struct cli_instance *sh,
                               const struct ext_ram_range *r, uint32_t testable)
{
	cli_print(sh, "range:   0x%08lx..0x%08lx %s, %s: %s\r\n",
	          ext_ram_addr(r->off), ext_ram_addr(r->off + r->len - 1u),
	          r->cached ? "cacheable" : "non-cacheable",
	          r->off < testable ? "tested" : "not tested", r->what);
}

static int ext_ram_bad_table(struct cli_instance *sh)
{
	cli_error(sh, EXT_RAM_PFX "bad range table\r\n");
	return 1;
}

/* ---- info ----------------------------------------------------------------- */

static int ext_ram_info_write(void *ctx, const char *s, size_t len)
{
	struct cli_instance *sh = (struct cli_instance *)ctx;

	if (s == NULL && len != 0u)
		return -1;
	if (len == 0u)
		return 0;
	return (cli_write(sh, s, len) < 0) ? -1 : (int)len;
}

static int cmd_ext_ram_info(struct cli_instance *sh, int argc, char **argv)
{
	const struct ext_ram_board *b = &ext_ram_board;
	uint32_t testable;
	unsigned k;

	(void)argc;
	(void)argv;
	if (ext_ram_table_check(b, &testable) != 0)
		return ext_ram_bad_table(sh);
	cli_print(sh, "device:  %s\r\n", b->part);
	cli_print(sh, "window:  0x%08lx..0x%08lx\r\n", ext_ram_addr(0u),
	          ext_ram_addr(b->size - 1u));
	for (k = 0u; k < b->n_ranges; k++)
		ext_ram_range_line(sh, &b->ranges[k], testable);
	cli_print(sh, "state:   %s\r\n", b->ready() ? "up" : "DOWN");
	if (b->info != NULL && b->info(ext_ram_info_write, sh) < 0)
		return 1;
	return 0;
}

/* ---- test ----------------------------------------------------------------- */

/*
 * One pass: write every span, then read every span back.  pattern 0 is the
 * address pattern.  @return 0; 1 with the first bad word in *bad / *got; or
 * -1 on Ctrl+C.
 */
static int ext_ram_pass(struct cli_instance *sh, const struct ext_ram_span *sp,
                        int n, uint32_t pattern, uint32_t *bad, uint32_t *got)
{
	int phase, k;

	for (phase = 0; phase < 2; phase++) {
		for (k = 0; k < n; k++) {
			volatile uint32_t *w = (volatile uint32_t *)(ext_ram_board.base +
			                                             sp[k].off);
			uint32_t words = sp[k].len / 4u, i, j, end;

			for (i = 0u; i < words; i = end) {
				end = (words - i > EXT_RAM_CHUNK_WORDS)
				      ? i + EXT_RAM_CHUNK_WORDS : words;
				for (j = i; j < end; j++) {
					uint32_t want = pattern ? pattern
					                        : (uint32_t)(uintptr_t)&w[j];

					if (phase == 0) {
						EXT_RAM_STORE(&w[j], want);
					} else {
						uint32_t v = EXT_RAM_LOAD(&w[j]);

						if (v != want) {
							*bad = (uint32_t)(uintptr_t)&w[j];
							*got = v;
							return 1;
						}
					}
				}
				if (cli_cancel_requested(sh))
					return -1;
			}
		}
	}
	return 0;
}

/* Give back steps [0, taken) in the reverse order, each once; a warning is
 * said and the rest are still given back. */
static void ext_ram_give(struct cli_instance *sh, unsigned taken)
{
	while (taken > 0u) {
		const struct ext_ram_step *s = &ext_ram_board.steps[--taken];
		const char *warn = (s->give != NULL) ? s->give() : NULL;

		if (warn != NULL)
			cli_warn(sh, EXT_RAM_PFX "%s\r\n", warn);
	}
}

static int ext_ram_run(struct cli_instance *sh, uint32_t len,
                       const struct ext_ram_claim *c)
{
	static const uint32_t patterns[3] = { 0u, 0x55555555u, 0xAAAAAAAAu };
	static const char *const names[3] = { "address", "0x55555555",
	                                      "0xAAAAAAAA" };
	struct ext_ram_span sp[2];
	uint32_t tested = 0u, bad = 0u, got = 0u;
	unsigned p, k;
	int n, rc;

	n = ext_ram_spans(len, c->skip_off, c->skip_len, sp);
	if (n <= 0) {
		cli_error(sh, EXT_RAM_PFX "%s\r\n", n < 0 ? "bad skip" : "nothing to test");
		return 1;
	}
	for (k = 0u; k < (unsigned)n; k++)
		tested += sp[k].len;

	cli_warn(sh, EXT_RAM_PFX "DESTRUCTIVE test over %lu %s from 0x%08lx\r\n",
	         EXT_RAM_KB(tested), EXT_RAM_KB_UNIT(tested), ext_ram_addr(0u));
	if (tested != len) {
		uint32_t end = (c->skip_len > len - c->skip_off)
		               ? len : c->skip_off + c->skip_len;

		cli_print(sh, EXT_RAM_PFX "0x%08lx..0x%08lx skipped: %s\r\n",
		          ext_ram_addr(c->skip_off), ext_ram_addr(end - 1u),
		          c->skip_why != NULL ? c->skip_why : "the board asked");
	}

	for (p = 0u; p < 3u; p++) {
		rc = ext_ram_pass(sh, sp, n, patterns[p], &bad, &got);
		if (rc < 0) {
			cli_print(sh, "pass %u/3 (%s): cancelled\r\n", p + 1u, names[p]);
			return 1;
		}
		if (rc > 0) {
			cli_error(sh, "pass %u/3 (%s): FAIL at 0x%08lx: got 0x%08lx, "
			              "want 0x%08lx\r\n", p + 1u, names[p],
			          (unsigned long)bad, (unsigned long)got,
			          (unsigned long)(patterns[p] ? patterns[p] : bad));
			return 1;
		}
		cli_print(sh, "pass %u/3 (%s): %lu %s OK\r\n", p + 1u, names[p],
		          EXT_RAM_KB(tested), EXT_RAM_KB_UNIT(tested));
	}
	cli_print(sh, EXT_RAM_PFX "%lu %s tested, no errors\r\n",
	          EXT_RAM_KB(tested), EXT_RAM_KB_UNIT(tested));
	return 0;
}

static int cmd_ext_ram_test(struct cli_instance *sh, int argc, char **argv)
{
	const struct ext_ram_board *b = &ext_ram_board;
	struct ext_ram_claim c;
	uint32_t testable, len;
	unsigned taken;
	int ret;

	if (ext_ram_table_check(b, &testable) != 0)
		return ext_ram_bad_table(sh);
	if (ext_ram_test_len(argc > 1 ? argv[1] : NULL, testable, &len) != 0) {
		cli_error(sh, EXT_RAM_PFX "bad length (4-byte multiple, 4..%lu)\r\n",
		          (unsigned long)testable);
		return 1;
	}
	if (!b->ready()) {
		cli_error(sh, EXT_RAM_PFX "not ready; see `" EXT_RAM_STR(EXT_RAM_CMD)
		              " info`\r\n");
		return 1;
	}

	memset(&c, 0, sizeof c);
	for (taken = 0u; taken < b->n_steps; taken++) {
		const struct ext_ram_step *s = &b->steps[taken];

		if (s->take != NULL && s->take(&c) != 0) {
			if (c.span_valid)
				cli_error(sh, EXT_RAM_PFX "%s (0x%08lx..0x%08lx)\r\n",
				          c.why != NULL ? c.why : "refused",
				          (unsigned long)c.lo, (unsigned long)c.hi);
			else
				cli_error(sh, EXT_RAM_PFX "%s\r\n",
				          c.why != NULL ? c.why : "refused");
			ext_ram_give(sh, taken);
			return 1;
		}
	}
	ret = ext_ram_run(sh, len, &c);
	ext_ram_give(sh, taken);
	return ret;
}

CLI_SUBCMD_SET_CREATE(ext_ram_subcmds,
	CLI_CMD_ARG(info, NULL, "part, window, ranges, state", cmd_ext_ram_info,
	            1, 0),
	CLI_CMD_ARG_USAGE(test, NULL, "DESTRUCTIVE write/read-back test",
	                  "[bytes]", cmd_ext_ram_test, 1, 1),
	EXT_RAM_BOARD_SUBCMDS
	CLI_SUBCMD_SET_END);

CLI_CMD_REGISTER(EXT_RAM_CMD, ext_ram_subcmds, EXT_RAM_HELP, NULL, 1, 0);
