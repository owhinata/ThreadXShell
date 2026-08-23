/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    cli_registry.c
 * @brief   The decisions taken over the thread->instance registry
 *          (issue #28's console scan, issue #81's slot rules).
 *
 * `struct cli_instance` has carried rx_dropped / tx_dropped since the shell core
 * was written -- the core zeroes them and the backends increment them -- but no
 * board ever had a general way to READ them.  The shared `console` command
 * (shell/cmds/cmd_console.c) is that readout, and this file is the part of it
 * that decides which instances it describes.
 *
 * Split out of cli_core.c for one reason: cli_core.c reaches for ThreadX services
 * and for an MRS on IPSR, so it cannot be compiled on the host, while the
 * filtering and the truncation accounting here are exactly what a host test can
 * pin.  Same split, and the same motive, as cli_session.c vs cli_core.c.
 *
 * Clean-room design; no third-party code reused.
 */
#include "cli_registry.h"

enum cli_reg_status cli_reg_add(struct cli_thread_map *map, size_t map_len,
                                TX_THREAD *t, struct cli_instance *sh)
{
	size_t i;
	size_t free_slot = map_len;   /* map_len == "none seen" */
	int    dup_thread = 0, dup_instance = 0;

	if (t == NULL || sh == NULL)
		return CLI_REG_ERR_ARG;

	/* One pass, no early exit: the reason must not depend on which entry came
	 * first (see the header).  Collect the first free slot and both duplicate
	 * flags, then decide below.
	 *
	 * [!] OCCUPANCY IS `sh`, NOT `thread` -- the same rule the table has always
	 * used (`sh` is published last and retracted first, so a slot advertising a
	 * NULL `sh` is one nobody has finished claiming).  A slot with `sh == NULL`
	 * is therefore FREE whatever its `thread` holds, and registers nothing, so
	 * its stale thread is not a duplicate of anything.  cli_reg_remove() clears
	 * both fields, so only corruption can leave that pair -- and reusing the
	 * slot HEALS it.  Refusing instead would keep the damage and fail
	 * cli_start() for ever, which on these boards means the console never comes
	 * up: the worst outcome available for a table that exists to route printf. */
	for (i = 0; i < map_len; i++) {
		if (map[i].sh == NULL) {
			if (free_slot == map_len)
				free_slot = i;
			continue;            /* free: claims nothing, duplicates nothing */
		}
		if (map[i].thread == t)
			dup_thread = 1;
		if (map[i].sh == sh)
			dup_instance = 1;
	}

	if (dup_thread)
		return CLI_REG_ERR_DUP_THREAD;
	if (dup_instance)
		return CLI_REG_ERR_DUP_INSTANCE;
	if (free_slot == map_len)
		return CLI_REG_ERR_FULL;

	map[free_slot].thread = t;
	map[free_slot].sh     = sh;   /* publish last */
	return CLI_REG_OK;
}

const char *cli_reg_strerror(enum cli_reg_status st)
{
	switch (st) {
	case CLI_REG_OK:               return "registered";
	case CLI_REG_ERR_ARG:          return "invalid argument";
	case CLI_REG_ERR_DUP_THREAD:   return "that thread is already registered";
	case CLI_REG_ERR_DUP_INSTANCE: return "that shell is already registered";
	case CLI_REG_ERR_FULL:         return "the thread registry is full";
	}
	return "unknown registry error";
}

size_t cli_reg_remove(struct cli_thread_map *map, size_t map_len,
                      const TX_THREAD *t)
{
	size_t i, n = 0;

	if (t == NULL)
		return 0;

	/* Whole table, no break: the postcondition is "no entry has .thread == t",
	 * which must hold even if something put two there. */
	for (i = 0; i < map_len; i++) {
		if (map[i].thread != t)
			continue;
		map[i].sh     = NULL;   /* retract first */
		map[i].thread = NULL;
		n++;
	}
	return n;
}

size_t cli_console_collect(const struct cli_thread_map *map, size_t map_len,
                           struct cli_console_stat *out, size_t cap,
                           size_t *found)
{
	size_t i, n = 0, seen = 0;

	for (i = 0; i < map_len; i++) {
		const struct cli_instance *sh = map[i].sh;
		size_t k;

		if (sh == NULL)
			continue;          /* free slot */
		if (sh->fg != NULL)
			continue;          /* background-job worker -- `jobs` lists those */

		seen++;
		if (n >= cap)
			continue;          /* keep counting: the caller reports truncation */

		/* Byte-for-byte over the whole fixed-size field rather than a string
		 * copy: this runs inside the caller's interrupt-disabled section, and a
		 * bounded plain copy is the only kind of work that belongs there.  The
		 * last byte is forced to NUL so a row is printable even if the source
		 * prompt were ever unterminated. */
		for (k = 0; k < CLI_PROMPT_BUFFER_SIZE; k++)
			out[n].prompt[k] = sh->prompt[k];
		out[n].prompt[CLI_PROMPT_BUFFER_SIZE - 1] = '\0';

		out[n].rx_dropped = sh->rx_dropped;
		out[n].tx_dropped = sh->tx_dropped;
		n++;
	}

	if (found != NULL)
		*found = seen;
	return n;
}
