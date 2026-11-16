/* shared-command: required */
/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    nor_console.c
 * @brief   Where an erase or a write of the NOR may run (issue #250; see
 *          nor_console.h).
 *
 * The question was wio-lite-ai's (cmds/cmd_nor.c, issue #249) and had no board
 * fact in it; grove-vision-ai-v2 asked only its background half (issue #203)
 * and f746g-disco neither.  It is asked here once for all three: by the shared
 * `nor` (shell/cmds/cmd_nor.c) for erase, write and test, and by
 * grove-vision-ai-v2's own `nor cycle`.
 *
 * The order is the telnet console first (a job launched from it answers for
 * its launcher, net_tty_is_console()), then any other background job
 * (sh->fg is its launcher; NULL on an interactive instance,
 * shell/include/cli_instance.h).  So the telnet console and its jobs are
 * named as such, a job of the local console as a job, and only the local
 * console's foreground goes ahead.
 *
 * The reason given is only the rule.  net_tty_guard()'s sentence ("it would
 * destroy this session") is not used: an erase does not end the telnet
 * session, so it would not be true here (issue #249).
 *
 * [!] A SHARED TRANSLATION UNIT OWNS NO MUTABLE STORAGE
 * (cmake/check_no_mutable_storage.py, per board).
 */
#include "nor_console.h"

#include "cli.h"
#include "cli_instance.h"    /* sh->fg: is this a background job */

#ifndef SHELL_HAS_NET_TTY
#error "SHELL_HAS_NET_TTY is defined from BOARD_HAS_NETX by the top-level CMakeLists.txt"
#endif

#if SHELL_HAS_NET_TTY
#include "net_tty.h"         /* net_tty_is_console, the local console's name */
#endif

int nor_console_refuse(struct cli_instance *sh)
{
#if SHELL_HAS_NET_TTY
	const char *here;

	if (net_tty_is_console(sh))
		here = (sh->fg != NULL) ? "a telnet job" : "telnet";
	else if (sh->fg != NULL)
		here = "a job";
	else
		return 0;
	cli_error(sh, "nor: erase/write only in the %s foreground, not %s\r\n",
	          net_tty_board_facts.other_console, here);
	return -1;
#else
	if (sh->fg == NULL)
		return 0;
	cli_error(sh, "nor: erase/write only in the foreground, not a job\r\n");
	return -1;
#endif
}
