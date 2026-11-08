/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * The check on the `net` subcommand table that only a built table can answer
 * (issue #219), shared by shell/test/test_net_cmd.c and the boards' table
 * tests: [!] NO NAME TWICE.  The shared command appends a board's subcommands
 * (NET_SVC_BOARD_SUBCMDS) to its own and the parser takes the first match, so
 * a board entry that reuses a shared name is silently unreachable.
 *
 * The table is found the way the dispatcher finds it, through the root-command
 * section, so what is checked is what a console would walk.
 */
#ifndef NET_TABLE_CHECK_H
#define NET_TABLE_CHECK_H

#include <stddef.h>
#include <string.h>

#include "cli.h"

static const struct cli_cmd *net_table(void)
{
	const struct cli_cmd *c;

	for (c = __cli_root_cmds_start; c < __cli_root_cmds_end; c++)
		if (c->name != NULL && strcmp(c->name, "net") == 0)
			return c->subcmds;
	return NULL;
}

/* How many entries share a name with an EARLIER entry. */
static int net_table_dups(const struct cli_cmd *set)
{
	int dups = 0;

	for (const struct cli_cmd *a = set; a != NULL && a->name != NULL; a++)
		for (const struct cli_cmd *b = set; b < a; b++)
			if (strcmp(a->name, b->name) == 0) {
				dups++;
				break;
			}
	return dups;
}

/* Position of @p name in the table, or -1. */
static int net_table_index(const struct cli_cmd *set, const char *name)
{
	int i = 0;

	for (const struct cli_cmd *c = set; c != NULL && c->name != NULL; c++, i++)
		if (strcmp(c->name, name) == 0)
			return i;
	return -1;
}

#endif /* NET_TABLE_CHECK_H */
