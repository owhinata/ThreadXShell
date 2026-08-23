/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Host unit test for the console-counter snapshot scan (issue #28,
 * shell/core/cli_registry.c).  cli_console_snapshot() itself is a TX_DISABLE
 * wrapper in cli_core.c (firmware only) and the `console` command is not linked
 * into this harness, so what is testable -- and what actually decides what the
 * command shows -- is cli_console_collect():
 *   1. an empty registry yields nothing,
 *   2. one console, values copied verbatim,
 *   3. free slots skipped and background-job workers (fg != NULL) excluded,
 *      with registry order preserved,
 *   4. truncation: rows stop at cap, `found` keeps counting, nothing is written
 *      past cap,
 *   5. counting-only call (out == NULL, cap == 0) and a NULL `found`,
 *   6. rows are COPIES: mutating the instance afterwards does not change them,
 *   7. duplicate registry entries are NOT folded (a damaged registry stays visible),
 *   8. a prompt that fills its field is still returned NUL-terminated.
 */
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cli_registry.h"

#define SLOTS 8

static TX_THREAD           threads[SLOTS];
static struct cli_instance insts[SLOTS];

/* Shape one instance the way the core does: everything zero except the fields
 * the scan reads. */
static void mk(struct cli_instance *sh, const char *prompt,
               struct cli_instance *fg, uint32_t rx, uint32_t tx)
{
	memset(sh, 0, sizeof *sh);
	snprintf(sh->prompt, sizeof sh->prompt, "%s", prompt);
	sh->fg         = fg;
	sh->rx_dropped = rx;
	sh->tx_dropped = tx;
}

int main(void)
{
	struct cli_thread_map   map[SLOTS];
	struct cli_console_stat out[SLOTS];
	size_t found, n;

	/* 1. empty registry: no rows, nothing found */
	memset(map, 0, sizeof map);
	found = 12345;
	n = cli_console_collect(map, SLOTS, out, SLOTS, &found);
	assert(n == 0 && found == 0);

	/* 2. a single interactive console: label + both counters copied */
	mk(&insts[0], "sh> ", NULL, 7, 9);
	map[0].thread = &threads[0];
	map[0].sh     = &insts[0];
	n = cli_console_collect(map, SLOTS, out, SLOTS, &found);
	assert(n == 1 && found == 1);
	assert(strcmp(out[0].prompt, "sh> ") == 0);
	assert(out[0].rx_dropped == 7 && out[0].tx_dropped == 9);

	/* 3. free slots skipped, workers excluded, order preserved.
	 *    Layout: [0]=sh>  [1]=worker of sh>  [2]=free  [3]=net>  [4]=worker  ... */
	memset(map, 0, sizeof map);
	mk(&insts[0], "sh> ",  NULL,       1, 2);
	mk(&insts[1], "",      &insts[0], 99, 99);   /* bg worker: empty prompt, fg set */
	mk(&insts[3], "net> ", NULL,       3, 4);
	mk(&insts[4], "",      &insts[3], 99, 99);
	map[0].thread = &threads[0]; map[0].sh = &insts[0];
	map[1].thread = &threads[1]; map[1].sh = &insts[1];
	map[3].thread = &threads[3]; map[3].sh = &insts[3];
	map[4].thread = &threads[4]; map[4].sh = &insts[4];
	n = cli_console_collect(map, SLOTS, out, SLOTS, &found);
	assert(n == 2 && found == 2);
	assert(strcmp(out[0].prompt, "sh> ")  == 0 && out[0].rx_dropped == 1);
	assert(strcmp(out[1].prompt, "net> ") == 0 && out[1].rx_dropped == 3);

	/* A stale `thread` with a NULL `sh` is a free slot (unregister retracts sh
	 * first), and must not be mistaken for an entry. */
	memset(map, 0, sizeof map);
	map[2].thread = &threads[2];
	n = cli_console_collect(map, SLOTS, out, SLOTS, &found);
	assert(n == 0 && found == 0);

	/* 4. truncation: cap rows written, `found` counts every match, and the
	 *    caller's array is untouched past cap. */
	memset(map, 0, sizeof map);
	mk(&insts[0], "a> ", NULL, 10, 20);
	mk(&insts[1], "b> ", NULL, 30, 40);
	mk(&insts[2], "c> ", NULL, 50, 60);
	map[0].sh = &insts[0];
	map[1].sh = &insts[1];
	map[2].sh = &insts[2];
	memset(out, 0xA5, sizeof out);
	n = cli_console_collect(map, SLOTS, out, 2, &found);
	assert(n == 2 && found == 3);                  /* found > n == truncated */
	assert(strcmp(out[0].prompt, "a> ") == 0);
	assert(strcmp(out[1].prompt, "b> ") == 0);
	assert(out[2].prompt[0] == (char)0xA5);        /* row 3 never written */
	assert(out[2].rx_dropped == 0xA5A5A5A5u);

	/* 5. count-only (no output array at all), and a caller that does not want
	 *    the total. */
	n = cli_console_collect(map, SLOTS, NULL, 0, &found);
	assert(n == 0 && found == 3);
	n = cli_console_collect(map, SLOTS, out, SLOTS, NULL);
	assert(n == 3);

	/* 6. rows are values: a later change to the instance cannot reach them. */
	n = cli_console_collect(map, SLOTS, out, SLOTS, &found);
	assert(n == 3);
	insts[0].rx_dropped = 999;
	snprintf(insts[0].prompt, sizeof insts[0].prompt, "gone> ");
	assert(out[0].rx_dropped == 10);
	assert(strcmp(out[0].prompt, "a> ") == 0);

	/* 7. the same instance registered twice yields two rows -- folding them
	 *    would hide exactly the registry damage worth seeing. */
	memset(map, 0, sizeof map);
	mk(&insts[0], "dup> ", NULL, 5, 6);
	map[0].thread = &threads[0]; map[0].sh = &insts[0];
	map[1].thread = &threads[0]; map[1].sh = &insts[0];
	n = cli_console_collect(map, SLOTS, out, SLOTS, &found);
	assert(n == 2 && found == 2);
	assert(strcmp(out[0].prompt, "dup> ") == 0);
	assert(strcmp(out[1].prompt, "dup> ") == 0);

	/* 8. a prompt filling the whole field (no terminator in the source) still
	 *    comes back terminated, so the command can print it. */
	memset(map, 0, sizeof map);
	memset(&insts[0], 0, sizeof insts[0]);
	memset(insts[0].prompt, 'x', sizeof insts[0].prompt);
	map[0].sh = &insts[0];
	n = cli_console_collect(map, SLOTS, out, SLOTS, &found);
	assert(n == 1 && found == 1);
	assert(strlen(out[0].prompt) == CLI_PROMPT_BUFFER_SIZE - 1);

	printf("test_console: all assertions passed\n");
	return 0;
}
