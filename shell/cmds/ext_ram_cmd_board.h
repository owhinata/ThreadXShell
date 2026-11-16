/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    ext_ram_cmd_board.h
 * @brief   The contract of the shared external RAM command (issue #251, epic
 *          #213): `sdram` on f746g-disco, `psram` on wio-lite-ai.
 *
 * shell/cmds/cmd_ext_ram.c holds the grammar, the length, which bytes a test
 * touches, the three passes, Ctrl+C and every line the command prints.  A
 * board with external RAM it can address (BOARD_HAS_EXT_RAM) defines the one
 * table below, ext_ram_board, under that fixed name, and says the command's
 * name, its help and its own subcommands in its ext_ram_config.h.
 *
 * THE RANGES.  The table splits the board's window into ranges, in address
 * order, one after another from the window's start to its end.  Each range
 * says whether the MPU maps it write-back cacheable and, in a few words, what
 * lives in it.  [!] A TEST NEVER REACHES A CACHEABLE RANGE: the bytes a test
 * may touch end where the first cacheable range starts, whatever the board
 * asks (a write and its read-back through a write-back cache can be answered
 * by the cache while the device is not answering at all).  The mark is the
 * board's word for what its MPU setup does; nothing here reads the MPU back
 * (the user's decision on issue #251).
 *
 * THE STEPS.  Before the first write the shared command takes the board's
 * steps in order (step k's take()), and after the last read it gives them back
 * in the reverse order (give()).  A take that refuses stops there: the steps
 * already taken are given back in reverse, and nothing is written.  Every way
 * a test ends -- passed, a mismatch, Ctrl+C, a skip the board got wrong --
 * gives back every taken step exactly once.  The steps are held for the whole
 * test and not given back in between.  Any take or give may be NULL (nothing
 * to do on that side).
 *
 * A take may also ask that this one test leave out a run of bytes
 * (struct ext_ram_claim's skip: the Ethernet bank on f746g-disco while the
 * network is up), and say why.
 *
 * [!] AN ADAPTER TAKES NO `struct cli_instance`.  The words a take or a give
 * returns (why, skip_why, a warning) are string literals or live as long as
 * the image; the shared command prints them after "<name>: ".
 *
 * Freestanding: no HAL, no tx_api.h.
 */
#ifndef EXT_RAM_CMD_BOARD_H
#define EXT_RAM_CMD_BOARD_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** At most this many ranges and steps (the shared command checks). */
#define EXT_RAM_RANGES_MAX 8u
#define EXT_RAM_STEPS_MAX  8u

/** One range of the window, as an offset from its start. */
struct ext_ram_range {
	uint32_t off;
	uint32_t len;
	/** 1: the MPU maps it write-back cacheable; never tested. */
	uint8_t cached;
	/** What lives in it, a few words ("LCD, camera"). */
	const char *what;
};

/** What a take says.  The caller zeroes it before the first take; a take
 *  sets only what it has to say. */
struct ext_ram_claim {
	/** A refusal: why, in the board's words. */
	const char *why;
	/** With span_valid, the refusal also names [lo, hi] (addresses). */
	uint8_t  span_valid;
	uint32_t lo, hi;
	/** Leave [skip_off, skip_off + skip_len) out of this test (offsets,
	 *  multiples of 4), and why.  skip_len 0: nothing left out. */
	uint32_t skip_off, skip_len;
	const char *skip_why;
};

struct ext_ram_step {
	/** 0: taken.  Otherwise refused, with c->why set. */
	int (*take)(struct ext_ram_claim *c);
	/** NULL: given back.  Otherwise a warning to print (the step is still
	 *  given back as far as the board could). */
	const char *(*give)(void);
};

/** One whole line of the board's own `info` lines ("\r\n" included).
 *  @return the bytes taken, or negative. */
typedef int (*ext_ram_write_fn)(void *ctx, const char *s, size_t len);

struct ext_ram_board {
	/** The part, for `info` ("MT48LC4M32B2, 16-bit bus"). */
	const char *part;
	/** The window: its first address and its size in bytes. */
	uintptr_t base;
	uint32_t  size;
	const struct ext_ram_range *ranges;
	uint8_t n_ranges;
	const struct ext_ram_step *steps;
	uint8_t n_steps;
	/** Non-zero while the RAM can be read and written. */
	int (*ready)(void);
	/** `info`: write the board's own lines, after the shared ones; NULL for
	 *  none.  @return 0, or negative after a write failed. */
	int (*info)(ext_ram_write_fn write, void *ctx);
};

/** The board's one table (boards/<board>/cmds/). */
extern const struct ext_ram_board ext_ram_board;

/* ---- pure functions of cmd_ext_ram.c, for the host tests ------------------ */

/** A span of the window a test writes and reads (offsets). */
struct ext_ram_span {
	uint32_t off, len;
};

/**
 * Check @p b's ranges: 1 .. EXT_RAM_RANGES_MAX of them, each of a non-zero
 * multiple of 4 bytes starting on a multiple of 4, one after another from 0
 * to b->size; at most EXT_RAM_STEPS_MAX steps.  On success *testable is where
 * the first cacheable range starts (b->size when there is none), and it is not
 * 0.  @return 0, or -1 with *testable 0.
 */
int ext_ram_table_check(const struct ext_ram_board *b, uint32_t *testable);

/**
 * The length of `test [bytes]`: @p arg NULL is all of @p testable; otherwise
 * a number that is not 0, is a multiple of 4 and is at most @p testable.
 * @return 0, or -1 (*len untouched).
 */
int ext_ram_test_len(const char *arg, uint32_t testable, uint32_t *len);

/**
 * [0, len) without [skip_off, skip_off + skip_len), as at most two spans in
 * address order.  @return how many (0 when the skip covers all of it), or -1
 * when the skip is not on multiples of 4.
 */
int ext_ram_spans(uint32_t len, uint32_t skip_off, uint32_t skip_len,
                  struct ext_ram_span out[2]);

#ifdef __cplusplus
}
#endif

#endif /* EXT_RAM_CMD_BOARD_H */
