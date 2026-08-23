/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    cmd_console.c
 * @brief   `console` built-in (issue #28): the per-instance rx_dropped /
 *          tx_dropped counters, one line per running interactive console.
 *
 * The counters existed from the start -- the core zeroes them, the backends
 * increment them -- and nothing general ever printed them, which is how issue
 * #25 ended up with an acceptance criterion ("rx_dropped does not move across a
 * heavy paste") that could not be checked.
 *
 * Why a command of its own rather than a line in an existing one: the counters
 * are per INSTANCE, and a board can run several consoles at once (the STM32
 * boards do -- VCP + telnet, USB CDC + telnet).  Issue #26 put a board-local
 * `Console:` line into the Grove's `version` and #27 took it out again for
 * exactly that reason: `version` describes the build, and a single-console shape
 * does not generalise.  `thread` is the other neighbour, and it is about ThreadX
 * threads, stacks and cpu%, not about transports.
 *
 * Deliberately absent:
 *   - tx_failed.  It is not a cumulative counter but a per-dispatch flag that is
 *     cleared at the start of every command -- and printing this very table is
 *     one of the things that can set it.
 *   - any reset / clear.  A counter that a reader can zero stops being evidence.
 *   - the backends' own private FIELDS.  What they count is not necessarily
 *     hidden: a ring overflow bumps sh->rx_dropped too, so it shows up here.
 *     But a backend's private mirror, and anything with no shared counterpart at
 *     all (the Grove's err_events), stay board-owned -- forcing them into this
 *     shared shape is what #27 undid.
 *
 * Linked into the firmware only (like cmd_thread.c), so the host suite tests
 * cli_console_collect() instead -- shell/test/test_console.c.
 * Clean-room design; no third-party code reused.
 */
#include <stddef.h>

#include "cli.h"
#include "cli_instance.h"   /* cli_console_snapshot / struct cli_console_stat */

static int cmd_console(struct cli_instance *sh, int argc, char **argv)
{
	struct cli_console_stat rows[CLI_MAX_INSTANCES];
	size_t found = 0;
	size_t n, i;

	(void)argc;
	(void)argv;

	n = cli_console_snapshot(rows, sizeof rows / sizeof rows[0], &found);

	cli_print(sh, "%-12s %10s %10s\r\n", "console", "rx_drop", "tx_drop");

	/* Unreachable through a console (this command runs on one, and rows[] always
	 * has room for at least one -- CLI_MAX_INSTANCES >= 1 is a _Static_assert).
	 * If it ever prints, the registry, not the console count, is the news. */
	if (found == 0) {
		cli_print(sh, "(no consoles registered)\r\n");
		return 0;
	}

	for (i = 0; i < n; i++)
		cli_print(sh, "%-12s %10lu %10lu\r\n",
		          rows[i].prompt,
		          (unsigned long)rows[i].rx_dropped,
		          (unsigned long)rows[i].tx_dropped);

	/* CLI_MAX_INSTANCES rows are enough only while every board keeps its own
	 * instance-count invariant; the registry does not enforce it (it accepts
	 * foreground entries up to CLI_THREAD_MAP_MAX).  So say so, and fail the
	 * command -- a partial table must not read as a complete one, and a caller
	 * that only sees the exit status must not read it as success either. */
	if (found > n) {
		cli_error(sh, "console: %lu of %lu shown (table holds %lu)\r\n",
		          (unsigned long)n, (unsigned long)found,
		          (unsigned long)(sizeof rows / sizeof rows[0]));
		return 1;
	}

	return 0;
}

CLI_CMD_REGISTER(console, NULL, "per-console RX/TX drop counters", cmd_console, 1, 0);
