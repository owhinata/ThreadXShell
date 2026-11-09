/* shared-command: optional BOARD_HAS_NETX */
/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    cmd_net.c
 * @brief   The one `net` command, shared by every board with a NetX Duo IP
 *          stack (issue #219, epic #212).
 *
 *   net info                    ip / gw, then the board's own lines
 *   net ip <a.b.c.d/mask> [gw]  set a static address
 *   net dhcp                    (re)acquire an address via DHCP, wait <= 30 s
 *   net ping <a.b.c.d> [count]  ICMP echo, count 1..100 (default 4)
 *   ...                         the board's own subcommands, appended
 *                               (NET_SVC_BOARD_SUBCMDS)
 *
 * The same layering as `camera` (shell/cmds/cmd_camera.c): this file owns the
 * grammar, the usage, the refusals, the waits, the ping loop and its totals
 * and every line it prints, and speaks only svc/net/net_svc.h.  Each board puts
 * its IP stack behind that contract (f746g-disco port/netxduo/net_svc_f746.c,
 * wio-lite-ai src/net_svc_wio.c) and says what it has in its
 * svc/net_svc_config.h.
 *
 * [!] THIS FILE OWNS NO STORAGE.  Results, addresses, totals and deadlines are
 * on the caller's stack (cmake/check_no_mutable_storage.py, per board).
 *
 * [!] NO BOARD IN HERE: no `#ifdef <BOARD>`, no board header.  What differs is
 * a property in net_svc_config.h, and a board's words come back in the
 * result's `why`.
 *
 * [!] EVERY WAIT IS MEASURED IN THREADX TICKS, AS THE DIFFERENCE FROM ITS START
 * (wrap-safe), never by counting sleeps: the adapter's calls and the output
 * take time between the sleeps.  A deadline only stops the next call; a glue
 * call already running (a probe's 1 s, a DHCP start) is not cut short.
 */
#include "cli.h"
#include "tx_api.h"   /* tx_time_get(), TX_TIMER_TICKS_PER_SECOND */

#include <stddef.h>
#include <stdint.h>

#include "net_svc.h"
#include "net_svc_config.h"   /* the board says what it has */

/*
 * [!] EVERY PROPERTY IS DECLARED BY THE BOARD.  A missing one is a build error,
 * so a typo cannot silently change what `net` prints.
 */
#if !defined(NET_SVC_HELP) || !defined(NET_SVC_BOARD_DECLS) || \
    !defined(NET_SVC_BOARD_SUBCMDS)
#error "net_svc_config.h must define NET_SVC_HELP, NET_SVC_BOARD_DECLS and NET_SVC_BOARD_SUBCMDS"
#endif

/* The waits below are milliseconds read off the ThreadX tick, the same
 * premise as `watch` and `sleep`. */
_Static_assert(TX_TIMER_TICKS_PER_SECOND == 1000,
               "cmd_net.c measures its waits in ThreadX ticks of 1 ms");

/* The board's own subcommand handlers (see NET_SVC_BOARD_SUBCMDS). */
NET_SVC_BOARD_DECLS

#define NET_CMD_POLL_MS        100u     /* between two calls of a wait      */
#define NET_CMD_BUSY_WAIT_MS   20000u   /* the whole BUSY wait, one deadline */
#define NET_CMD_DHCP_WAIT_S    30u      /* the whole wait for a lease        */
#define NET_CMD_PROBE_WAIT_MS  1000u    /* one probe's reply wait            */
#define NET_CMD_PROBE_GAP_MS   1000u    /* between two probes                */
#define NET_CMD_PING_DEFAULT   4u
#define NET_CMD_PING_MAX       100u

/* A board marks a DHCP lease its link layer saw the link go down under
 * (issue #220). */
#define NET_CMD_STALE(a) ((a)->lease_stale \
	? "  <-- STALE (link went down; DHCP restarts when it returns)" : "")

/* Ticks since @p start (1 tick = 1 ms, asserted above); wrap-safe. */
static ULONG net_elapsed(ULONG start)
{
	return (ULONG)(tx_time_get() - start);
}

/* ---- saying what happened ------------------------------------------------ */

/* The board's words, or ours for a result that came without any. */
static const char *net_why(const struct net_svc_result *r)
{
	if (r->why != NULL)
		return r->why;
	switch (r->status) {
	case NET_SVC_DOWN:   return "the IP stack is not up";
	case NET_SVC_BUSY:   return "the IP stack is still changing state";
	case NET_SVC_FAILED: return "the IP stack has failed";
	case NET_SVC_LINK:   return "the link is down";
	default:             return "NetX refused";
	}
}

/* A result that says the IP stack cannot do this now: the operation stops
 * with the board's reason. */
static int net_is_stop(enum net_svc_status s)
{
	return s == NET_SVC_DOWN || s == NET_SVC_BUSY || s == NET_SVC_FAILED ||
	       s == NET_SVC_LINK;
}

/* A failed `net ip` / `net dhcp`: the board's reason, or @p refused when NetX
 * said no (or the status is not one of the reasons). */
static int net_fail(struct cli_instance *sh, const struct net_svc_result *r,
                    const char *refused)
{
	if (net_is_stop(r->status))
		cli_error(sh, "net: %s\r\n", net_why(r));
	else
		cli_error(sh, "net: %s\r\n", refused);
	return 1;
}

/* ip / gw, two lines (N4), or the one `ip: none (...)` line. */
static void net_print_addr(struct cli_instance *sh,
                           const struct net_svc_result *r,
                           const struct net_svc_addr *a)
{
	const char *mode;

	if (r->status != NET_SVC_OK) {
		cli_print(sh, "ip:    none (%s)\r\n", net_why(r));
		return;
	}
	if (!a->ip_valid) {
		cli_print(sh, "ip:    none (%s)\r\n",
		          a->dhcp_mode ? "dhcp, no lease" : "unset");
		return;
	}
	mode = a->dhcp_mode ? "dhcp" : "static";
	cli_print(sh, "ip:    %lu.%lu.%lu.%lu/%u\r\n",
	          (unsigned long)((a->ip >> 24) & 0xFFu),
	          (unsigned long)((a->ip >> 16) & 0xFFu),
	          (unsigned long)((a->ip >> 8) & 0xFFu),
	          (unsigned long)(a->ip & 0xFFu), cli_ipv4_mask_bits(a->mask));
	/* gw 0 is "no gateway", not an address (issue #19): 0.0.0.0 would read
	   as a configured one. */
	if (a->gw == 0u)
		cli_print(sh, "gw:    none (%s)%s\r\n", mode, NET_CMD_STALE(a));
	else
		cli_print(sh, "gw:    %lu.%lu.%lu.%lu (%s)%s\r\n",
		          (unsigned long)((a->gw >> 24) & 0xFFu),
		          (unsigned long)((a->gw >> 16) & 0xFFu),
		          (unsigned long)((a->gw >> 8) & 0xFFu),
		          (unsigned long)(a->gw & 0xFFu), mode, NET_CMD_STALE(a));
}

/* Read the address and print it. */
static void net_show_addr(struct cli_instance *sh)
{
	struct net_svc_addr a;
	struct net_svc_result r;

	net_svc_addr_get(&a, &r);
	net_print_addr(sh, &r, &a);
}

/* The board's lines go out one whole line per write (svc/net/net_svc.h). */
static int net_write(void *ctx, const char *s, size_t len)
{
	struct cli_instance *sh = (struct cli_instance *)ctx;

	if (sh == NULL || (s == NULL && len != 0u))
		return -1;
	if (len == 0u)
		return 0;
	return (cli_write(sh, s, len) < 0) ? -1 : (int)len;
}

/* ---- calling an operation through BUSY ----------------------------------- */

enum net_call_kind { NET_CALL_STATIC, NET_CALL_DHCP, NET_CALL_PING };

struct net_call {
	enum net_call_kind kind;
	uint32_t ip, mask, gw;
	unsigned *rtt_ms;
};

static void net_call_once(const struct net_call *c, struct net_svc_result *res)
{
	switch (c->kind) {
	case NET_CALL_STATIC:
		net_svc_set_static(c->ip, c->mask, c->gw, res);
		break;
	case NET_CALL_DHCP:
		net_svc_dhcp_start(res);
		break;
	default:
		net_svc_ping(c->ip, NET_CMD_PROBE_WAIT_MS, c->rtt_ms, res);
		break;
	}
}

enum net_wait { NET_WAIT_DONE, NET_WAIT_EXPIRED, NET_WAIT_CANCELLED };

/*
 * Call @p c until it answers something other than BUSY.  BUSY changed nothing
 * (svc/net/net_svc.h), so calling again is safe.  ONE deadline for the whole
 * wait, counted from before the first call: a slow adapter does not stretch
 * it, and a retry does not restart it.  On EXPIRED @p res is the last BUSY
 * (its `why` is what the command prints); on CANCELLED nothing was called
 * after the Ctrl+C.
 */
static enum net_wait net_call_settled(struct cli_instance *sh,
                                      const struct net_call *c,
                                      struct net_svc_result *res)
{
	ULONG start = tx_time_get();

	for (;;) {
		net_call_once(c, res);
		if (res->status != NET_SVC_BUSY)
			return NET_WAIT_DONE;
		if (net_elapsed(start) >= NET_CMD_BUSY_WAIT_MS)
			return NET_WAIT_EXPIRED;
		if (cli_sleep(sh, NET_CMD_POLL_MS) != 0)
			return NET_WAIT_CANCELLED;
		if (net_elapsed(start) >= NET_CMD_BUSY_WAIT_MS)
			return NET_WAIT_EXPIRED;
	}
}

/* ---- subcommands ---------------------------------------------------------- */

/* Answers in every state (BUSY is not waited for): the address line says why
 * there is none, and the board's lines follow whatever it was. */
static int cmd_net_info(struct cli_instance *sh, int argc, char **argv)
{
	(void)argc;
	(void)argv;
	net_show_addr(sh);
	return (net_svc_board_lines(net_write, sh) < 0) ? 1 : 0;
}

static int cmd_net_ip(struct cli_instance *sh, int argc, char **argv)
{
	struct net_call c = { .kind = NET_CALL_STATIC };
	struct net_svc_result r;
	enum net_wait w;

	/* Every argument is read before anything is called. */
	if (cli_parse_ipv4_cidr(argv[1], &c.ip, &c.mask) != 0) {
		cli_error(sh, "net: bad address '%s' (use a.b.c.d/mask)\r\n", argv[1]);
		return 1;
	}
	if (argc >= 3 && cli_parse_ipv4(argv[2], &c.gw) != 0) {
		cli_error(sh, "net: bad gateway '%s'\r\n", argv[2]);
		return 1;
	}
	w = net_call_settled(sh, &c, &r);
	if (w == NET_WAIT_CANCELLED)
		return 1;
	if (w == NET_WAIT_EXPIRED || r.status != NET_SVC_OK)
		return net_fail(sh, &r, "NetX refused the static address");
	cli_print(sh, "net: static address set\r\n");
	net_show_addr(sh);
	return 0;
}

static int cmd_net_dhcp(struct cli_instance *sh, int argc, char **argv)
{
	struct net_call c = { .kind = NET_CALL_DHCP };
	struct net_svc_result r;
	struct net_svc_addr a;
	enum net_wait w;
	ULONG start;

	(void)argc;
	(void)argv;
	w = net_call_settled(sh, &c, &r);
	if (w == NET_WAIT_CANCELLED)
		return 1;
	if (w == NET_WAIT_EXPIRED ||
	    (r.status != NET_SVC_OK && r.status != NET_SVC_RENEWED))
		return net_fail(sh, &r, "NetX DHCP client would not start");
	if (r.status == NET_SVC_RENEWED)
		cli_print(sh, "net: renewing the existing lease (kept meanwhile)...\r\n");
	else
		cli_print(sh, "net: acquiring an address via DHCP (up to %u s, "
		              "Ctrl+C to stop)...\r\n", NET_CMD_DHCP_WAIT_S);

	/* Ctrl+C ends the wait, not the DHCP client, and does not count as the
	 * operator having given the interface an address. */
	start = tx_time_get();
	for (;;) {
		net_svc_addr_get(&a, &r);
		if (r.status == NET_SVC_OK && a.ip_valid) {
			net_print_addr(sh, &r, &a);
			return 0;
		}
		/* The stack went away under the wait: no lease can arrive. */
		if (r.status != NET_SVC_OK && r.status != NET_SVC_BUSY) {
			cli_error(sh, "net: %s\r\n", net_why(&r));
			return 1;
		}
		if (net_elapsed(start) >= NET_CMD_DHCP_WAIT_S * 1000u) {
			cli_error(sh, "net: no DHCP lease after %u s\r\n",
			          NET_CMD_DHCP_WAIT_S);
			return 1;
		}
		if (cli_sleep(sh, NET_CMD_POLL_MS) != 0)
			return 1;
	}
}

/* One probe's line: a reply, a timeout, or NetX's refusal. */
static unsigned net_probe_line(struct cli_instance *sh, unsigned n,
                               const struct net_svc_result *r, unsigned rtt)
{
	if (r->status == NET_SVC_OK) {
		cli_print(sh, "  reply %u: %u ms\r\n", n, rtt);
		return 1u;
	}
	if (r->status == NET_SVC_TIMEOUT)
		cli_print(sh, "  probe %u: timeout\r\n", n);
	else
		cli_error(sh, "  probe %u: NetX error\r\n", n);
	return 0u;
}

static int cmd_net_ping(struct cli_instance *sh, int argc, char **argv)
{
	unsigned rtt = 0, rmin = 0xFFFFFFFFu, rmax = 0, rsum = 0;
	unsigned count = NET_CMD_PING_DEFAULT, sent = 0, ok = 0;
	struct net_call c = { .kind = NET_CALL_PING, .rtt_ms = &rtt };
	struct net_svc_result r;
	enum net_wait w;
	int stopped = 0;

	if (cli_parse_ipv4(argv[1], &c.ip) != 0) {
		cli_error(sh, "net: bad address '%s'\r\n", argv[1]);
		return 1;
	}
	if (argc >= 3) {                     /* an explicit count must be valid (N1) */
		uint32_t n;

		if (cli_parse_u32(argv[2], &n) != 0 || n == 0u ||
		    n > NET_CMD_PING_MAX) {
			cli_error(sh, "net: bad count '%s' (1..%u)\r\n", argv[2],
			          NET_CMD_PING_MAX);
			return 1;
		}
		count = (unsigned)n;
	}

	/* The first probe waits out BUSY; a reason to stop there means no probe
	 * went out, so there is no heading and no totals. */
	w = net_call_settled(sh, &c, &r);
	if (w == NET_WAIT_CANCELLED)
		return 1;
	if (w == NET_WAIT_EXPIRED || net_is_stop(r.status)) {
		cli_error(sh, "net: %s\r\n", net_why(&r));
		return 1;
	}
	cli_print(sh, "PING %s, %u probes:\r\n", argv[1], count);
	for (;;) {
		unsigned got;

		sent++;
		got = net_probe_line(sh, sent, &r, rtt);
		if (got) {
			ok++;
			rsum += rtt;
			if (rtt < rmin)
				rmin = rtt;
			if (rtt > rmax)
				rmax = rtt;
		}
		if (sent >= count)
			break;
		if (cli_sleep(sh, NET_CMD_PROBE_GAP_MS) != 0)
			break;                   /* Ctrl+C between probes */
		rtt = 0;
		net_svc_ping(c.ip, NET_CMD_PROBE_WAIT_MS, &rtt, &r);
		if (net_is_stop(r.status)) {
			cli_error(sh, "net: %s\r\n", net_why(&r));
			stopped = 1;
			break;
		}
	}
	if (ok > 0u)
		cli_print(sh, "%u/%u received, rtt min/avg/max %u/%u/%u ms\r\n",
		          ok, sent, rmin, rsum / ok, rmax);
	else
		cli_print(sh, "%u/%u received\r\n", ok, sent);
	return (stopped || ok == 0u) ? 1 : 0;     /* no reply at all is a failure (N2) */
}

/* ---- registration ---------------------------------------------------------- */

CLI_SUBCMD_SET_CREATE(net_subcmds,
	CLI_CMD_ARG(info, NULL, "IP / mask / gateway, then the board's state and counters",
	            cmd_net_info, 1, 0),
	CLI_CMD_ARG_USAGE(ip, NULL, "set static <a.b.c.d/mask> [gw]",
	                  "<a.b.c.d/mask> [gw]", cmd_net_ip, 2, 1),
	CLI_CMD_ARG(dhcp, NULL, "(re)acquire an address via DHCP", cmd_net_dhcp, 1, 0),
	CLI_CMD_ARG_USAGE(ping, NULL, "ICMP echo <a.b.c.d> [count]",
	                  "<a.b.c.d> [count]", cmd_net_ping, 2, 1),
	NET_SVC_BOARD_SUBCMDS
	CLI_SUBCMD_SET_END);

CLI_CMD_REGISTER(net, net_subcmds, NET_SVC_HELP, NULL, 1, 0);
