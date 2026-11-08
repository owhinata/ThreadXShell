/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    cmd_eth.c
 * @brief   `eth` shell command: the on-board Ethernet link layer, ETH MAC +
 * LAN8742A PHY over RMII (issue #217).
 *
 *   eth info   MAC address, PHY address / id, link state and speed / duplex
 *   eth link   re-run auto-negotiation and report the link state
 *
 * This is the board's command to its own link layer (port/eth, eth_link.h),
 * the f746g-disco counterpart of wio-lite-ai's `wifi`.  It moved out of `net`
 * (issue #217, child of #212): `net` keeps only what sits on NetX Duo, and
 * this command never includes nx_glue.h or nx_api.h.
 *
 * Clean-room design; no third-party code reused.
 */
#include "cli.h"
#include "eth_link.h"

#include <stddef.h>   /* NULL */
#include <stdint.h>

static const char *speed_name(enum eth_speed s)
{
	return (s == ETH_LINK_SPEED_100M) ? "100M" : (s == ETH_LINK_SPEED_10M) ? "10M" : "?";
}

static const char *duplex_name(enum eth_duplex d)
{
	return (d == ETH_LINK_DUPLEX_FULL) ? "full" : (d == ETH_LINK_DUPLEX_HALF) ? "half" : "?";
}

/* Guard: the link driver must be initialised (eth_init() at boot, gated on the
   SDRAM being up). */
static int eth_ready(struct cli_instance *sh)
{
	if (!eth_is_initialized()) {
		cli_error(sh, "eth: not initialized\r\n");
		return 0;
	}
	return 1;
}

static void eth_print_link(struct cli_instance *sh, const struct eth_link_info *li)
{
	if (li->up)
		cli_print(sh, "link:  up (%s %s)\r\n",
		          speed_name(li->speed), duplex_name(li->duplex));
	else
		cli_print(sh, "link:  down\r\n");
}

static int cmd_eth_info(struct cli_instance *sh, int argc, char **argv)
{
	struct eth_link_info li;

	(void)argc;
	(void)argv;
	if (!eth_ready(sh))
		return 1;
	if (eth_link_get(&li) != ETH_OK) {
		cli_error(sh, "eth: link query failed\r\n");
		return 1;
	}

	cli_print(sh, "mac:   %02x:%02x:%02x:%02x:%02x:%02x\r\n",
	          li.mac[0], li.mac[1], li.mac[2], li.mac[3], li.mac[4], li.mac[5]);
	cli_print(sh, "phy:   addr %lu, id %04x:%04x (LAN8742)\r\n",
	          (unsigned long)li.phy_addr, li.phy_id1, li.phy_id2);
	eth_print_link(sh, &li);
	return 0;
}

static int cmd_eth_link(struct cli_instance *sh, int argc, char **argv)
{
	struct eth_link_info li;
	int rc;

	(void)argc;
	(void)argv;
	if (!eth_ready(sh))
		return 1;

	if (eth_link_renegotiate() != ETH_OK) {
		cli_error(sh, "eth: auto-negotiation restart failed\r\n");
		return 1;
	}
	cli_print(sh, "eth: auto-negotiating (up to 5 s, Ctrl+C to stop)...\r\n");

	for (int i = 0; i < 50; i++) {
		if (cli_sleep(sh, 100) != 0)
			return 1;
		if (eth_link_get(&li) == ETH_OK && li.up) {
			eth_print_link(sh, &li);
			return 0;
		}
	}
	rc = eth_link_get(&li);
	if (rc == ETH_OK)
		eth_print_link(sh, &li);
	cli_warn(sh, "eth: link not up after 5 s (cable? peer?)\r\n");
	return 0;
}

CLI_SUBCMD_SET_CREATE(eth_subcmds,
	CLI_CMD(info, NULL, "MAC / PHY address and id / link state", cmd_eth_info),
	CLI_CMD(link, NULL, "restart auto-negotiation and report link state",
	        cmd_eth_link),
	CLI_SUBCMD_SET_END);

CLI_CMD_REGISTER(eth, eth_subcmds,
                 "Ethernet MAC / PHY (LAN8742) link: info / link",
                 NULL, 1, 0);
