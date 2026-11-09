/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Host test: the `net` subcommand table this board actually builds -- the
 * shared command (shell/cmds/cmd_net.c) against this board's REAL
 * svc/net_svc_config.h, over the stub adapter (shell/test/net_svc_stub.c)
 * (issue #219).  This board has no subcommand of its own since issue #221
 * (the telnet console's `net shell` went with it).
 *
 *  - [!] NO NAME TWICE: NET_SVC_BOARD_SUBCMDS is appended to the shared
 *    entries and the parser takes the first match, so a board name that
 *    reuses a shared one would be silently unreachable.
 *  - the shared info / ip / dhcp / ping and nothing else: no `shell`.
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
	expect("info ip dhcp ping, then the end (no `shell`)",
	       net_table_index(t, "info") == 0 && net_table_index(t, "ip") == 1 &&
	       net_table_index(t, "dhcp") == 2 && net_table_index(t, "ping") == 3 &&
	       net_table_index(t, "shell") < 0 && t[4].name == NULL);

	net_stub_reset();
	run("net shell status\r");
	expect("`net shell status` is not a command any more",
	       strstr(cli_dummy_output_str(&tr0), "shell") != NULL &&
	       strstr(cli_dummy_output_str(&tr0), "unknown") != NULL);

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
