/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    cmd_net_board.c
 * @brief   This board's own `net` subcommand: `net shell start|stop|status`,
 * the telnet console's controls (owhinata/wio-lite-ai#23 U4-2).
 *
 * `net` itself is the shared command (shell/cmds/cmd_net.c, issue #219), which
 * appends the table below through NET_SVC_BOARD_SUBCMDS (svc/net_svc_config.h)
 * and registers the `net` root; this file registers no root.  info / ip /
 * dhcp / ping are the shared command's, over src/net_svc_wio.c.  Issue #221
 * makes the console resident and removes these controls.
 *
 * No clock/RCC/register work -- pure marshalling + orchestration (clock-safe).
 * Clean-room design; no third-party code reused.
 */
#include "cli.h"
#include "net_shell.h"

#include <stddef.h>
#include <stdint.h>

/* ---- `net shell`: the telnet console (owhinata/wio-lite-ai#23 U4-2) --------------------- */
/*
 * The console itself lives in app/net_shell.c; these are just its controls.  Since U4-2 it
 * is a NetX socket on the HOST stack, so it needs the interface up (`wifi connect`), and
 * it arms itself when the stack takes an address (`net dhcp` / `net ip` call
 * net_shell_autoarm() through net_svc_addressed(), src/net_svc_wio.c).
 * `start` is therefore for a different port or after a `stop`.
 *
 * `stop` is no longer about freeing the eRPC UART -- the console does not touch it any
 * more.  It exists because the interface refuses to unwind while a socket on it is alive
 * (a flash session has to wait for this), and because a resident console is not always
 * wanted.
 */
#define NET_SHELL_WAIT_MS   10000u
#define NET_SHELL_POLL_MS     100u

static int cmd_net_shell_start(struct cli_instance *sh, int argc, char **argv)
{
	const char *why = NULL;
	uint32_t waited, v;
	uint16_t port = (uint16_t)NET_SHELL_PORT_DEFAULT;

	if (argc >= 2) {
		if (cli_parse_u32(argv[1], &v) != 0 || v == 0u || v > 65535u) {
			cli_error(sh, "net: bad port '%s' (1..65535)\r\n", argv[1]);
			return 1;
		}
		port = (uint16_t)v;
	}
	if (net_shell_start(port, &why) != 0) {
		cli_error(sh, "net shell: cannot start -- %s\r\n", why != NULL ? why : "?");
		net_shell_print_status(sh);
		return 1;
	}
	cli_print(sh, "net shell: arming on port %u...\r\n", (unsigned)port);
	for (waited = 0u; waited < NET_SHELL_WAIT_MS; waited += NET_SHELL_POLL_MS) {
		if (net_shell_state() != NET_SHELL_ARMING)
			break;
		if (cli_sleep(sh, NET_SHELL_POLL_MS))
			break;              /* Ctrl+C: the service carries on regardless */
	}
	net_shell_print_status(sh);
	return net_shell_armed() ? 0 : 1;
}

static int cmd_net_shell_stop(struct cli_instance *sh, int argc, char **argv)
{
	uint32_t waited;

	(void)argc; (void)argv;
	/* Stopping the console we are talking on would cut the wire under our own feet. */
	if (net_shell_guard(sh, "net shell stop"))
		return 1;
	if (net_shell_state() == NET_SHELL_OFF) {
		net_shell_stop();           /* still disables auto-arm */
		cli_print(sh, "net shell: already stopped (auto-arm off)\r\n");
		return 0;
	}
	net_shell_stop();
	cli_print(sh, "net shell: stopping...\r\n");
	for (waited = 0u; waited < NET_SHELL_WAIT_MS; waited += NET_SHELL_POLL_MS) {
		if (net_shell_state() == NET_SHELL_OFF)
			break;
		if (cli_sleep(sh, NET_SHELL_POLL_MS))
			break;
	}
	net_shell_print_status(sh);
	return net_shell_armed() ? 1 : 0;
}

static int cmd_net_shell_status(struct cli_instance *sh, int argc, char **argv)
{
	(void)argc; (void)argv;
	net_shell_print_status(sh);
	return 0;
}

/* ---- registration -------------------------------------------------------- */

/* Not CLI_SUBCMD_SET_CREATE (which makes it static): the shared command's
   table names it (NET_SVC_BOARD_DECLS / NET_SVC_BOARD_SUBCMDS). */
const struct cli_cmd net_board_shell_subcmds[] = {
	CLI_CMD_ARG_USAGE(start,  NULL, "arm the telnet console [port] (default 23)",
	                  "[port]", cmd_net_shell_start, 1, 1),
	CLI_CMD_ARG(stop,   NULL, "close the console + disable auto-arm",
	            cmd_net_shell_stop,   1, 0),
	CLI_CMD_ARG(status, NULL, "state / address / counters",
	            cmd_net_shell_status, 1, 0),
	CLI_SUBCMD_SET_END,
};
