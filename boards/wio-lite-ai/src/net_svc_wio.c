/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    net_svc_wio.c
 * @brief   The shared `net` command's adapter on Wio Lite AI (issue #219).
 *
 * Answers the calls of svc/net/net_svc.h from the host stack (src/nx_net.h)
 * and reads its NXN_* results as the contract's statuses.  Takes no
 * `struct cli_instance`: printing, waiting and Ctrl+C are the shared
 * command's (shell/cmds/cmd_net.c).  Owns no storage.
 *
 * [!] BUSY COMES ONLY FROM THE STATE CHECK BELOW, BEFORE ANY GLUE CALL.  The
 * host stack's ARMING and STOPPING resolve by themselves; picking a backend
 * mid-transition is how a command ends up talking to the stack on its way
 * out, so the shared command waits them out and calls again.  A refusal the
 * glue gives later (the state moved under its lock) is DOWN, never BUSY: by
 * then the glue has been called.
 *
 * The glue does not put a static address or a DHCP start back when NetX
 * refuses part way (src/nx_net.c, until issue #220), which REFUSED allows.
 */
#include "net_svc.h"

#include <stddef.h>
#include <stdint.h>

#include "net_shell.h"        /* net_shell_autoarm() */
#include "nx_link_driver.h"   /* the link state for `net ping` (issue #30 B2a) */
#include "nx_net.h"

/* The board's reasons, as the per-board command printed them. */
static const char net_wio_down[] =
	"no host stack -- `wifi connect` brings it up";
static const char net_wio_arming[] = "the host stack is still coming up";
static const char net_wio_stopping[] = "the host stack is still going down";
static const char net_wio_failed[] =
	"the host stack could not be shut down cleanly -- run `wifi reset`";
static const char net_wio_link[] =
	"the link is down (not associated) -- `wifi connect` first";

/* The host stack's state, before any glue call.  1 when it is up and the
 * caller may go on; 0 with @p res filled otherwise. */
static int net_wio_ready(struct net_svc_result *res)
{
	res->why = NULL;
	switch (nx_net_state()) {
	case NX_NET_UP:
		res->status = NET_SVC_OK;
		return 1;
	case NX_NET_ARMING:
		res->status = NET_SVC_BUSY;
		res->why = net_wio_arming;
		return 0;
	case NX_NET_STOPPING:
		res->status = NET_SVC_BUSY;
		res->why = net_wio_stopping;
		return 0;
	case NX_NET_FAILED:
		res->status = NET_SVC_FAILED;
		res->why = net_wio_failed;
		return 0;
	default:                       /* NX_NET_OFF, or a state this file does not know */
		res->status = NET_SVC_DOWN;
		res->why = net_wio_down;
		return 0;
	}
}

/* An NXN_* result as the contract's status. */
static void net_wio_result(int rc, struct net_svc_result *res)
{
	res->why = NULL;
	switch (rc) {
	case NXN_OK:      res->status = NET_SVC_OK;      break;
	case NXN_TIMEOUT: res->status = NET_SVC_TIMEOUT; break;
	case NXN_ERR_STATE:
		/* The state moved between the check and the glue's own, under its
		 * lock: the stack is on its way out (or never initialised). */
		res->status = NET_SVC_DOWN;
		res->why = net_wio_down;
		break;
	default:          res->status = NET_SVC_REFUSED; break;   /* NXN_ERR, unknown */
	}
}

void net_svc_addr_get(struct net_svc_addr *out, struct net_svc_result *res)
{
	struct nx_net_info ni;
	int rc;

	if (!net_wio_ready(res))
		return;
	rc = nx_net_info_get(&ni);
	net_wio_result(rc, res);
	if (rc != NXN_OK)
		return;
	out->ip_valid = ni.ip_valid;
	out->dhcp_mode = ni.dhcp_mode;
	out->lease_stale = ni.lease_stale;   /* NET_SVC_HAS_LEASE_STALE 1 */
	out->ip = ni.ip;
	out->mask = ni.mask;
	out->gw = ni.gw;
}

void net_svc_set_static(uint32_t ip, uint32_t mask, uint32_t gw,
                        struct net_svc_result *res)
{
	if (!net_wio_ready(res))
		return;
	net_wio_result(nx_net_set_static(ip, mask, gw), res);
}

void net_svc_dhcp_start(struct net_svc_result *res)
{
	if (!net_wio_ready(res))
		return;
	net_wio_result(nx_net_dhcp_renew(), res);
}

/*
 * Say WHY rather than letting NetX refuse every probe in turn: with the link
 * down (owhinata/wio-lite-ai#30 B2a) the interface still has an address and
 * looks configured.  Asked before each probe, so a link that drops during a
 * run stops it.  The echo itself is NetX's, resolved by our own ARP cache and
 * relayed by the module; the RTT is measured across nx_icmp_ping() alone.
 */
void net_svc_ping(uint32_t ip, unsigned timeout_ms, unsigned *rtt_ms,
                  struct net_svc_result *res)
{
	if (!net_wio_ready(res))
		return;
	if (!nx_link_driver_link_up()) {
		res->status = NET_SVC_LINK;
		res->why = net_wio_link;
		return;
	}
	net_wio_result(nx_net_ping(ip, timeout_ms, rtt_ms), res);
}

/* The host stack has an address: this is where the telnet console belongs
 * since U4-2, and where it is armed from (issue #221 makes it resident). */
void net_svc_addressed(void)
{
	net_shell_autoarm();
}

/* The host-stack section: why it is (not) up, then its counters.  Written in
 * every state, even OFF: its first line carries WHY it is off. */
int net_svc_board_lines(net_svc_write_fn write, void *ctx)
{
	return nx_net_write_status(write, ctx);
}
