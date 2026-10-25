/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * The two checks on the `camera` subcommand table that only a built table can
 * answer (issue #143), shared by shell/test/test_camera_cmd.c and the boards'
 * table tests:
 *
 *  - [!] NO NAME TWICE.  The shared command appends a board's subcommands
 *    (CAMERA_SVC_BOARD_SUBCMDS) to its own; the parser takes the first match,
 *    so a board entry that reuses a shared name is silently unreachable --
 *    or, put before, would silently replace the shared one.
 *  - a subcommand is in the table exactly when its capability says so.
 *
 * The table is found the way the dispatcher finds it, through the root-command
 * section, so what is checked is what a console would walk.
 */
#ifndef CAMERA_TABLE_CHECK_H
#define CAMERA_TABLE_CHECK_H

#include <stddef.h>
#include <string.h>

#include "cli.h"

static const struct cli_cmd *camera_table(void)
{
	const struct cli_cmd *c;

	for (c = __cli_root_cmds_start; c < __cli_root_cmds_end; c++)
		if (c->name != NULL && strcmp(c->name, "camera") == 0)
			return c->subcmds;
	return NULL;
}

/* How many entries share a name with an EARLIER entry. */
static int camera_table_dups(const struct cli_cmd *set)
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

static int camera_table_has(const struct cli_cmd *set, const char *name)
{
	for (const struct cli_cmd *c = set; c != NULL && c->name != NULL; c++)
		if (strcmp(c->name, name) == 0)
			return 1;
	return 0;
}

/* Position of @p name in the table, or -1. */
static int camera_table_index(const struct cli_cmd *set, const char *name)
{
	int i = 0;

	for (const struct cli_cmd *c = set; c != NULL && c->name != NULL; c++, i++)
		if (strcmp(c->name, name) == 0)
			return i;
	return -1;
}

#endif /* CAMERA_TABLE_CHECK_H */
