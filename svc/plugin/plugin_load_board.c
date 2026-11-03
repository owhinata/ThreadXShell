/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    plugin_load_board.c
 * @brief   The firmware's entry to the container parse: the board's policy, by
 *          name (issue #165).  See plugin_load.h.
 *
 * [!] THE POLICY IS NOT AN ARGUMENT.  plugin_parse() reads the one object named
 * plugin_board_policy, which the board defines and the linker binds; there is
 * no call by which a firmware could hand the parse another one.  That is what
 * makes the build's read-back (cmake/check_policy_probe.py, which reads that
 * same symbol out of shell.elf) a statement about the policy containers are
 * admitted by, rather than about an object that merely looks like it.
 *
 * [!] ONLY FIRMWARE LINKS THIS FILE, AND NO FIRMWARE LINKS plugin_load_host.c.
 * The host's argument-taking entry is a separate TU over the same body, not a
 * macro here: a -D could turn a macro back on.  cmake/check_policy_entry.py
 * reads every input of the firmware link and refuses one that defines or
 * references the host entry.
 */
#include "plugin_load_body.h"

enum plugin_result plugin_parse(const void *buf, size_t len,
                                struct plugin_view *out)
{
	return parse_checked(buf, len, &plugin_board_policy, out);
}

/*
 * The layout of the policy, for the build's read-back (issue #111).  A link
 * root (veneer_cost_gate.cmake passes --require-defined for it), since nothing
 * in the image reads it.
 */
__attribute__((used)) const struct plugin_policy_probe plugin_policy_probe = {
	PLUGIN_POLICY_PROBE_MAGIC,
	(uint32_t)sizeof(struct plugin_policy),
	(uint32_t)offsetof(struct plugin_policy, veneer_cost),
	(uint32_t)offsetof(struct plugin_policy, stack_accounting),
	(uint32_t)offsetof(struct plugin_policy, stack_limit),
};
