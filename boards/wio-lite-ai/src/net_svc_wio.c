/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    net_svc_wio.c
 * @brief   The shared `net` command's adapter on Wio Lite AI (issue #219).
 *
 * Checks this board's own state -- the host stack's OFF / ARMING / UP /
 * STOPPING / FAILED -- and hands the address, DHCP and ping operations to the
 * shared implementation (shell/cmds/net_core.c, issue #220), which also puts
 * a failed static address back and says LINK with the association down.
 * Takes no `struct cli_instance`: printing, waiting and Ctrl+C are the shared
 * command's (shell/cmds/cmd_net.c).  Owns no storage.
 *
 * [!] BUSY COMES ONLY FROM THE STATE CHECK BELOW, BEFORE ANY SHARED CALL.  The
 * host stack's ARMING and STOPPING resolve by themselves; picking a backend
 * mid-transition is how a command ends up talking to the stack on its way
 * out, so the shared command waits them out and calls again.
 */
#include "net_svc.h"

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#include "fmt.h"
#include "net_core.h"
#include "net_shell.h"        /* net_shell_autoarm() */
#include "nx_net.h"

/* The board's reasons, as the per-board command printed them. */
static const char net_wio_down[] =
	"no host stack -- `wifi connect` brings it up";
static const char net_wio_arming[] = "the host stack is still coming up";
static const char net_wio_stopping[] = "the host stack is still going down";
static const char net_wio_failed[] =
	"the host stack could not be shut down cleanly -- run `wifi reset`";

/* The host stack's state, before any shared call.  1 when it is up and the
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

void net_svc_addr_get(struct net_svc_addr *out, struct net_svc_result *res)
{
	if (net_wio_ready(res))
		net_core_addr_get(out, res);
}

void net_svc_set_static(uint32_t ip, uint32_t mask, uint32_t gw,
                        struct net_svc_result *res)
{
	if (net_wio_ready(res))
		net_core_set_static(ip, mask, gw, res);
}

void net_svc_dhcp_start(struct net_svc_result *res)
{
	if (net_wio_ready(res))
		net_core_dhcp_start(res);
}

/* The echo is NetX's, resolved by our own ARP cache and relayed by the
 * module.  Asked before each probe, so a link that drops during a run stops
 * it (the shared side says LINK, owhinata/wio-lite-ai#30 B2a). */
void net_svc_ping(uint32_t ip, unsigned timeout_ms, unsigned *rtt_ms,
                  struct net_svc_result *res)
{
	if (net_wio_ready(res))
		net_core_ping(ip, timeout_ms, rtt_ms, res);
}

/* The host stack has an address: this is where the telnet console belongs
 * since U4-2, and where it is armed from (issue #221 makes it resident). */
void net_svc_addressed(void)
{
	net_shell_autoarm();
}

/* The host-stack section: why it is (not) up, then its counters.  Written in
 * every state, even OFF: its first line carries WHY it is off.  Then the shared
 * link worker's last failure, only while one is recorded: the shared side cannot
 * write this board's log (issue #220). */
static int net_wio_line(net_svc_write_fn write, void *ctx, const char *fmt, ...)
{
	char line[96];
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = fmt_vsnformat(line, sizeof line, fmt, ap);
	va_end(ap);
	return (write(ctx, line, (size_t)n) < 0) ? -1 : 0;
}

int net_svc_board_lines(net_svc_write_fn write, void *ctx)
{
	struct net_core_failure lf;

	if (nx_net_write_status(write, ctx) < 0)
		return -1;
	if (!net_core_last_failure(&lf))
		return 0;
	return net_wio_line(write, ctx,
	                    "dhcp: the link worker's %s failed (0x%02x) with the link %s\r\n",
	                    net_core_op_name(lf.op), (unsigned)lf.status,
	                    lf.link_up ? "up" : "down");
}
