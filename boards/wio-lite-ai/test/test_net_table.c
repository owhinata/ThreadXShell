/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Host test: the `net` subcommand table this board actually builds -- the
 * shared command (shell/cmds/cmd_net.c) against this board's REAL
 * svc/net_svc_config.h, over the stub adapter (shell/test/net_svc_stub.c) and
 * a stub table for the board's own subcommand (issue #219).
 *
 *  - [!] NO NAME TWICE: NET_SVC_BOARD_SUBCMDS is appended to the shared
 *    entries and the parser takes the first match, so a board name that
 *    reuses a shared one would be silently unreachable.
 *  - the shared info / ip / dhcp / ping come first and the board's `shell`
 *    after them, reaching its own table through the dispatcher.
 *  - the gw line carries the STALE mark (every board since issue #220).
 */
#include <stdio.h>
#include <string.h>

#include "cli_instance.h"
#include "cli_internal.h"
#include "cli_backend_dummy.h"
#include "host_glue.h"
#include "net_svc_config.h"
#include "net_svc_stub.h"
#include "net_table_check.h"

static int board_calls;
static const char *board_last;

static int stub_board(struct cli_instance *sh, int argc, char **argv)
{
	(void)sh;
	(void)argc;
	board_calls++;
	board_last = argv[0];
	return 0;
}

/* The board's table, as NET_SVC_BOARD_DECLS names it. */
const struct cli_cmd net_board_shell_subcmds[] = {
	CLI_CMD(status, NULL, "stub leaf", stub_board),
	CLI_SUBCMD_SET_END,
};

CLI_BACKEND_DUMMY_DEFINE(tr0);
static struct cli_instance sh0;

static void run(const char *line)
{
	memset(&sh0, 0, sizeof sh0);
	sh0.tr = &tr0;
	tr0.sh = &sh0;
	strcpy(sh0.prompt, "> ");
	cli_dummy_clear_output(&tr0);
	cli_dummy_clear_rx(&tr0);
	cli_dummy_reset_stats(&tr0);
	cli_dummy_set_tx_fail(&tr0, 0);
	cli_dummy_set_tx_cap(&tr0, 0);
	cli_test_set_tx_wait_hook(NULL, NULL);
	cli_test_set_sleep_hook(net_stub_sleep_hook, NULL);
	net_stub_set_transport(&tr0);
	board_calls = 0;
	board_last = NULL;
	cli_dummy_inject(&tr0, line, strlen(line));
	cli_test_pump(&sh0);
}

static int failures;

static void expect(const char *what, int ok)
{
	printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
	if (!ok) {
		printf("       output was: \"%s\"\n", cli_dummy_output_str(&tr0));
		failures++;
	}
}

int main(void)
{
	const struct cli_cmd *t = net_table();

	printf("test_net_table (wio-lite-ai):\n");
	expect("the `net` root is registered", t != NULL);
	if (t == NULL)
		return 1;
	expect("no subcommand name appears twice", net_table_dups(t) == 0);
	expect("info ip dhcp ping, then the board's shell, then the end",
	       net_table_index(t, "info") == 0 && net_table_index(t, "ip") == 1 &&
	       net_table_index(t, "dhcp") == 2 && net_table_index(t, "ping") == 3 &&
	       net_table_index(t, "shell") == 4 && t[5].name == NULL);

	net_stub_reset();
	run("net shell status\r");
	expect("`net shell status` reaches the board", board_calls == 1 &&
	       strcmp(board_last, "status") == 0);

	net_stub_reset();
	net_stub.address.lease_stale = true;
	run("net info\r");
	expect("the gw line marks a stale lease",
	       strstr(cli_dummy_output_str(&tr0), "<-- STALE") != NULL);

	if (failures != 0) {
		printf("test_net_table: %d failure(s)\n", failures);
		return 1;
	}
	printf("test_net_table: all passed\n");
	return 0;
}
