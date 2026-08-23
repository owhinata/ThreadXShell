/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    cli_console.c
 * @brief   The pure part of the console-counter snapshot (issue #28).
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
#include "cli_console.h"

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
