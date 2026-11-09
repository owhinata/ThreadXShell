/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Host test: the one implementation of the address, DHCP and ping operations
 * (shell/cmds/net_core.c, issue #220 step 1), compiled as it is, over fake
 * NetX / DHCP / ThreadX calls (nx_shim/, shim/tx_api.h) that record every
 * call, whether the address lock was held for it, and the masked sections.
 *
 * The fake DHCP record has three states (not started / acquiring / bound)
 * and refuses a start after a stop until it is reinitialised, as NetX
 * documents (the real one only checks the state).  Its force renew answers
 * NOT_BOUND in every state short of bound, as the real one does.
 *
 * What is pinned:
 *  - before init every operation is DOWN in the board's words, calling
 *    nothing; init sets the ready mark last, and not at all on a failure;
 *  - the address is read in ONE hold of the address lock and no DHCP call
 *    (the DHCP client's mutex is never reached from here);
 *  - `net ip`: an off-subnet gateway is refused before the lock or NetX; a
 *    stop that fails changes nothing; an address or gateway failure puts the
 *    address, the gateway, the source and the client back, the reinitialise
 *    BEFORE the address is restored; a client that cannot be restarted is
 *    left "DHCP, not running"; the link down does not refuse it;
 *  - `net dhcp`: LINK with the link down and nothing called; REFUSED without
 *    a client; the three cases; every start preceded by a reinitialise; a
 *    failed stop, reinitialise or start moves the state only by what
 *    succeeded;
 *  - `net ping`: LINK (nothing called), TIMEOUT, OK with the round trip, no
 *    lock;
 *  - `net ip` and `net dhcp` clear the stale mark, and `net dhcp` on a stale
 *    lease acquires from scratch instead of renewing it;
 *  - the temporary link entries: the start after a link-up only with the
 *    link up, the source DHCP and the client stopped; the link-lost entry
 *    records the link down, stops when asked, marks a DHCP address stale
 *    only, and keeps a client NetX did not stop as running;
 *  - the KV "source DHCP" starts even with the link recorded down;
 *  - the link note is one masked store, no NetX call, no lock.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "tx_api.h"
#include "nx_api.h"
#include "nxd_dhcp_client.h"
#include "net_core.h"

/* ---- the board's objects, as a board defines them ------------------------- */

static NX_IP   fake_ip;
static NX_DHCP fake_dhcp;

struct net_core_state net_core_board_state;
const struct net_core_facts net_core_board_facts = {
	.ip          = &fake_ip,
	.dhcp        = &fake_dhcp,
	.iface       = 0u,
	.boot_source = NET_CORE_SRC_DHCP,
	.why_link    = "board: link down",
	.why_down    = "board: not up",
};

/* ---- the fakes --------------------------------------------------------------- */

#define FAKE_NEEDS_INIT 0xE1u   /* the fake's "started after a stop" refusal */
#define LOG_MAX 64

enum dhcp_fake_state { D_IDLE, D_ACQUIRING, D_BOUND };

struct fake {
	/* the interface */
	ULONG ip, mask, gw;
	/* the DHCP record */
	enum dhcp_fake_state dstate;
	int needs_init;               /* stopped since the last reinitialise */
	ULONG lease_ip;               /* what the client set, 0 none         */
	/* failures to inject: 0 none */
	UINT fail_stop, fail_reinit, fail_start, fail_renew;
	int fail_addr_set_n;          /* fail the Nth address set (1-based)  */
	int fail_gw_set_n;            /* fail the Nth gateway set            */
	int addr_sets, gw_sets;
	/* ping */
	UINT ping_rc;
	ULONG ping_ms;
	/* the clock */
	ULONG now;
	/* the lock and the masked sections */
	int mutex_fail, mutex_created;
	int held, takes, irq_depth, irq_sections;
	/* what was called, in order, and whether the lock was held */
	char log[LOG_MAX][32];
	int locked[LOG_MAX];
	int nlog;
	int unlocked_netx;            /* an address / DHCP call without the lock */
	int masked_netx;              /* any NetX call inside a masked section   */
};

static struct fake fk;

static void note(const char *what, int needs_lock)
{
	if (fk.irq_depth != 0)
		fk.masked_netx++;
	if (needs_lock && !fk.held)
		fk.unlocked_netx++;
	if (fk.nlog < LOG_MAX) {
		snprintf(fk.log[fk.nlog], sizeof fk.log[0], "%s", what);
		fk.locked[fk.nlog] = fk.held;
		fk.nlog++;
	}
}

ULONG tx_time_get(void) { return fk.now; }

UINT tx_mutex_create(TX_MUTEX *m, CHAR *name, UINT inherit)
{
	(void)m; (void)name; (void)inherit;
	if (fk.mutex_fail)
		return 0x1Cu;
	fk.mutex_created++;
	return TX_SUCCESS;
}

UINT tx_mutex_get(TX_MUTEX *m, ULONG wait)
{
	(void)m; (void)wait;
	if (fk.held)
		fk.unlocked_netx += 1000;    /* a nested take: never expected */
	fk.held = 1;
	fk.takes++;
	return TX_SUCCESS;
}

UINT tx_mutex_put(TX_MUTEX *m)
{
	(void)m;
	fk.held = 0;
	return TX_SUCCESS;
}

UINT tx_shim_disable(void)
{
	fk.irq_depth++;
	fk.irq_sections++;
	return 0u;
}

void tx_shim_restore(UINT saved)
{
	(void)saved;
	fk.irq_depth--;
}

UINT nx_ip_interface_address_get(NX_IP *ip, UINT i, ULONG *a, ULONG *m)
{
	(void)ip; (void)i;
	note("addr_get", 1);
	*a = fk.ip;
	*m = fk.mask;
	return NX_SUCCESS;
}

UINT nx_ip_interface_address_set(NX_IP *ip, UINT i, ULONG a, ULONG m)
{
	char b[32];

	(void)ip; (void)i;
	snprintf(b, sizeof b, "addr_set %lx", a);
	note(b, 1);
	fk.addr_sets++;
	if (fk.fail_addr_set_n != 0 && fk.addr_sets == fk.fail_addr_set_n)
		return 0x21u;
	fk.ip = a;
	fk.mask = m;
	return NX_SUCCESS;
}

UINT nx_ip_gateway_address_get(NX_IP *ip, ULONG *g)
{
	(void)ip;
	note("gw_get", 1);
	*g = fk.gw;
	return (fk.gw != 0u) ? NX_SUCCESS : 0x4Eu;   /* NX_NOT_FOUND-like */
}

UINT nx_ip_gateway_address_set(NX_IP *ip, ULONG g)
{
	char b[32];

	(void)ip;
	snprintf(b, sizeof b, "gw_set %lx", g);
	note(b, 1);
	fk.gw_sets++;
	if (fk.fail_gw_set_n != 0 && fk.gw_sets == fk.fail_gw_set_n)
		return 0x21u;
	fk.gw = g;
	return NX_SUCCESS;
}

UINT nx_ip_gateway_address_clear(NX_IP *ip)
{
	(void)ip;
	note("gw_clear", 1);
	fk.gw = 0u;
	return NX_SUCCESS;
}

UINT nx_icmp_ping(NX_IP *ip, ULONG a, CHAR *d, ULONG n, NX_PACKET **r, ULONG w)
{
	static NX_PACKET pkt;

	(void)ip; (void)a; (void)d; (void)n; (void)w;
	note("ping", 0);
	fk.now += fk.ping_ms;
	*r = (fk.ping_rc == NX_SUCCESS) ? &pkt : NX_NULL;
	return fk.ping_rc;
}

UINT nx_packet_release(NX_PACKET *p)
{
	(void)p;
	note("release", 0);
	return NX_SUCCESS;
}

UINT nx_dhcp_interface_start(NX_DHCP *d, UINT i)
{
	(void)d; (void)i;
	note("start", 1);
	if (fk.fail_start)
		return fk.fail_start;
	if (fk.needs_init)
		return FAKE_NEEDS_INIT;
	if (fk.dstate != D_IDLE)
		return NX_DHCP_ALREADY_STARTED;
	fk.dstate = D_ACQUIRING;
	return NX_SUCCESS;
}

UINT nx_dhcp_interface_stop(NX_DHCP *d, UINT i)
{
	(void)d; (void)i;
	note("stop", 1);
	if (fk.fail_stop)
		return fk.fail_stop;
	if (fk.dstate == D_IDLE)
		return NX_DHCP_NOT_STARTED;
	fk.dstate = D_IDLE;
	fk.needs_init = 1;
	return NX_SUCCESS;
}

UINT nx_dhcp_interface_reinitialize(NX_DHCP *d, UINT i)
{
	(void)d; (void)i;
	note("reinit", 1);
	if (fk.fail_reinit)
		return fk.fail_reinit;
	/* NetX clears an address only when it is the one the client set. */
	if (fk.lease_ip != 0u && fk.ip == fk.lease_ip) {
		fk.ip = 0u;
		fk.mask = 0u;
		fk.gw = 0u;
	}
	fk.lease_ip = 0u;
	fk.dstate = D_IDLE;
	fk.needs_init = 0;
	return NX_SUCCESS;
}

UINT nx_dhcp_interface_force_renew(NX_DHCP *d, UINT i)
{
	(void)d; (void)i;
	note("renew", 1);
	if (fk.fail_renew)
		return fk.fail_renew;
	if (fk.dstate == D_BOUND)
		return NX_SUCCESS;
	return NX_DHCP_NOT_BOUND;     /* NetX: every state short of BOUND */
}

/* ---- harness --------------------------------------------------------------- */

static int failures;

static void expect(const char *what, int ok)
{
	printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
	if (!ok) {
		int i;

		printf("       calls:");
		for (i = 0; i < fk.nlog; i++)
			printf(" [%s%s]", fk.log[i], fk.locked[i] ? "" : " UNLOCKED");
		printf("\n");
		failures++;
	}
}

/* The calls since the last clear, joined by ", ". */
static const char *calls(void)
{
	static char b[1024];
	size_t n = 0;
	int i;

	b[0] = '\0';
	for (i = 0; i < fk.nlog && n < sizeof b - 40; i++)
		n += (size_t)snprintf(b + n, sizeof b - n, "%s%s", i ? ", " : "",
		                      fk.log[i]);
	return b;
}

static int calls_are(const char *want)
{
	return strcmp(calls(), want) == 0;
}

static void clear_calls(void)
{
	fk.nlog = 0;
	fk.takes = 0;
	fk.irq_sections = 0;
	fk.unlocked_netx = 0;
	fk.masked_netx = 0;
	fk.addr_sets = 0;              /* the Nth-call failures count from here */
	fk.gw_sets = 0;
}

#define S (&net_core_board_state)

/* A ready core, link up, nothing addressed. */
static void fresh(bool dhcp_created)
{
	memset(&fk, 0, sizeof fk);
	memset(&net_core_board_state, 0, sizeof net_core_board_state);
	fk.mask = 0u;
	(void)net_core_init(dhcp_created);
	net_core_link_note(true);
	clear_calls();
}

/* The client got a lease (its own thread). */
static void lease_arrives(ULONG ip, ULONG gw)
{
	fk.ip = ip;
	fk.mask = 0xFFFFFF00u;
	fk.gw = gw;
	fk.lease_ip = ip;
	fk.dstate = D_BOUND;
}

/* A running DHCP client holding @p ip / @p gw, through `net dhcp` itself. */
static void bound(ULONG ip, ULONG gw)
{
	struct net_svc_result r;

	net_core_dhcp_start(&r);
	lease_arrives(ip, gw);
	clear_calls();
}

static void no_lock_left(const char *what)
{
	char b[128];

	snprintf(b, sizeof b, "%s: the lock is released and every mask restored", what);
	expect(b, fk.held == 0 && fk.irq_depth == 0 && fk.masked_netx == 0 &&
	       fk.unlocked_netx == 0);
}

#define IP_A   0xC0A80B2Cu   /* 192.168.11.44  */
#define GW_A   0xC0A80B01u   /* 192.168.11.1   */
#define IP_S   0xC0A80B63u   /* 192.168.11.99  */
#define MASK24 0xFFFFFF00u

/* ---- init ------------------------------------------------------------------- */

static void check_init(void)
{
	struct net_svc_result r;
	struct net_svc_addr a;
	unsigned rtt = 0;

	memset(&fk, 0, sizeof fk);
	memset(&net_core_board_state, 0, sizeof net_core_board_state);
	net_core_addr_get(&a, &r);
	expect("before init: addr is DOWN in the board's words, nothing called",
	       r.status == NET_SVC_DOWN && strcmp(r.why, "board: not up") == 0 &&
	       fk.nlog == 0 && fk.takes == 0);
	net_core_set_static(IP_S, MASK24, GW_A, &r);
	expect("before init: `net ip` is DOWN", r.status == NET_SVC_DOWN && fk.nlog == 0);
	net_core_dhcp_start(&r);
	expect("before init: `net dhcp` is DOWN", r.status == NET_SVC_DOWN && fk.nlog == 0);
	net_core_ping(GW_A, 1000u, &rtt, &r);
	expect("before init: ping is DOWN", r.status == NET_SVC_DOWN && fk.nlog == 0);
	net_core_dhcp_source(&r);
	expect("before init: the KV source is DOWN", r.status == NET_SVC_DOWN && fk.nlog == 0);
	expect("before init: the link entries do nothing",
	       net_core_link_up_start() == NX_SUCCESS &&
	       net_core_link_lost(true) == NX_SUCCESS && fk.nlog == 0 && fk.takes == 0);

	fk.mutex_fail = 1;
	expect("init: a lock that cannot be created fails, ready stays clear",
	       net_core_init(true) == -1 && S->ready == 0);

	memset(&fk, 0, sizeof fk);
	expect("init: ready, the lock created once, the boot source DHCP",
	       net_core_init(true) == 0 && S->ready == 1 && fk.mutex_created == 1 &&
	       S->source == NET_CORE_SRC_DHCP && S->dhcp_running == 0 &&
	       S->link_up == 0 && S->dhcp_created == 1 && fk.nlog == 0);
	memset(&fk, 0, sizeof fk);
	expect("init: boots on DHCP without a client -> no source",
	       net_core_init(false) == 0 && S->source == NET_CORE_SRC_NONE &&
	       S->dhcp_created == 0);
}

/* ---- the address ------------------------------------------------------------- */

static void check_addr(void)
{
	struct net_svc_result r;
	struct net_svc_addr a;

	fresh(true);
	bound(IP_A, GW_A);
	S->lease_stale = 1u;
	net_core_addr_get(&a, &r);
	expect("addr: one hold of the lock, ip / mask / gw read inside it, no DHCP call",
	       r.status == NET_SVC_OK && fk.takes == 1 &&
	       calls_are("addr_get, gw_get") && fk.locked[0] && fk.locked[1]);
	expect("addr: the values, DHCP mode and the stale mark",
	       a.ip_valid && a.ip == IP_A && a.mask == MASK24 && a.gw == GW_A &&
	       a.dhcp_mode && a.lease_stale);
	no_lock_left("addr");

	fresh(true);
	S->source = NET_CORE_SRC_NONE;
	net_core_addr_get(&a, &r);
	expect("addr: no source, no address -> not valid, not DHCP, gw 0",
	       r.status == NET_SVC_OK && !a.ip_valid && !a.dhcp_mode && a.gw == 0u &&
	       !a.lease_stale);
}

/* ---- net ip ------------------------------------------------------------------- */

static void check_static(void)
{
	struct net_svc_result r;

	fresh(true);
	net_core_set_static(IP_S, MASK24, 0x0A000001u, &r);
	expect("ip: a gateway off the subnet is refused before the lock or NetX",
	       r.status == NET_SVC_REFUSED && fk.nlog == 0 && fk.takes == 0);

	fresh(true);
	bound(IP_A, GW_A);
	S->lease_stale = 1u;
	net_core_set_static(IP_S, MASK24, GW_A, &r);
	expect("ip: from DHCP -- stop, address, gateway; static, stopped, not stale",
	       r.status == NET_SVC_OK &&
	       calls_are("addr_get, gw_get, stop, addr_set c0a80b63, gw_set c0a80b01") &&
	       fk.ip == IP_S && fk.gw == GW_A && S->source == NET_CORE_SRC_STATIC &&
	       S->dhcp_running == 0 && S->lease_stale == 0);
	no_lock_left("ip");

	fresh(true);
	S->source = NET_CORE_SRC_STATIC;
	fk.gw = GW_A;
	net_core_set_static(IP_S, MASK24, 0u, &r);
	expect("ip: gw 0 clears the gateway, no DHCP call when it is not running",
	       r.status == NET_SVC_OK &&
	       calls_are("addr_get, gw_get, addr_set c0a80b63, gw_clear") && fk.gw == 0u);

	net_core_link_note(false);
	clear_calls();
	net_core_set_static(IP_S, MASK24, GW_A, &r);
	expect("ip: allowed with the link down", r.status == NET_SVC_OK && fk.gw == GW_A);

	/* A stop NetX refuses: nothing changed. */
	fresh(true);
	bound(IP_A, GW_A);
	fk.fail_stop = 0x22u;
	net_core_set_static(IP_S, MASK24, GW_A, &r);
	expect("ip: a failed stop changes nothing (no address set, still running)",
	       r.status == NET_SVC_REFUSED && calls_are("addr_get, gw_get, stop") &&
	       fk.ip == IP_A && S->dhcp_running == 1 && S->source == NET_CORE_SRC_DHCP);
	no_lock_left("ip stop failure");

	/* The address fails: everything goes back, reinitialise first. */
	fresh(true);
	bound(IP_A, GW_A);
	fk.fail_addr_set_n = 1;
	net_core_set_static(IP_S, MASK24, GW_A, &r);
	expect("ip: an address failure puts back reinit -> address -> gw -> start",
	       r.status == NET_SVC_REFUSED &&
	       calls_are("addr_get, gw_get, stop, addr_set c0a80b63, reinit, "
	                 "addr_set c0a80b2c, gw_set c0a80b01, start"));
	expect("ip: ... the address, gateway, source and running client are back",
	       fk.ip == IP_A && fk.mask == MASK24 && fk.gw == GW_A &&
	       S->source == NET_CORE_SRC_DHCP && S->dhcp_running == 1 &&
	       fk.dstate == D_ACQUIRING);
	no_lock_left("ip rollback");

	/* The gateway fails after the address moved. */
	fresh(true);
	bound(IP_A, GW_A);
	fk.fail_gw_set_n = 1;
	net_core_set_static(IP_S, MASK24, GW_A, &r);
	expect("ip: a gateway failure puts the old address back too",
	       r.status == NET_SVC_REFUSED &&
	       calls_are("addr_get, gw_get, stop, addr_set c0a80b63, gw_set c0a80b01, "
	                 "reinit, addr_set c0a80b2c, gw_set c0a80b01, start") &&
	       fk.ip == IP_A && fk.gw == GW_A && S->dhcp_running == 1);

	/* From static, the gateway fails: no DHCP call at all. */
	fresh(true);
	S->source = NET_CORE_SRC_STATIC;
	fk.ip = IP_S;
	fk.mask = MASK24;
	fk.gw = GW_A;
	fk.fail_gw_set_n = 1;
	net_core_set_static(0xC0A80C05u, MASK24, 0xC0A80C01u, &r);
	expect("ip: from static, a failure restores the static address, no DHCP",
	       r.status == NET_SVC_REFUSED && fk.ip == IP_S && fk.gw == GW_A &&
	       S->source == NET_CORE_SRC_STATIC && S->dhcp_running == 0 &&
	       strstr(calls(), "start") == NULL && strstr(calls(), "reinit") == NULL);

	/* The client cannot be put back. */
	fresh(true);
	bound(IP_A, GW_A);
	fk.fail_addr_set_n = 1;
	fk.fail_start = 0x23u;
	net_core_set_static(IP_S, MASK24, GW_A, &r);
	expect("ip: a restart that fails leaves `DHCP, not running`",
	       r.status == NET_SVC_REFUSED && fk.ip == IP_A &&
	       S->source == NET_CORE_SRC_DHCP && S->dhcp_running == 0);
	fresh(true);
	bound(IP_A, GW_A);
	fk.fail_addr_set_n = 1;
	fk.fail_reinit = 0x24u;
	net_core_set_static(IP_S, MASK24, GW_A, &r);
	expect("ip: a reinitialise that fails is not followed by a start",
	       r.status == NET_SVC_REFUSED && strstr(calls(), "start") == NULL &&
	       fk.ip == IP_A && S->source == NET_CORE_SRC_DHCP && S->dhcp_running == 0);
	no_lock_left("ip restart failure");
}

/* ---- net dhcp -------------------------------------------------------------- */

static void check_dhcp(void)
{
	struct net_svc_result r;

	fresh(true);
	net_core_link_note(false);
	clear_calls();
	fk.ip = IP_S;
	S->source = NET_CORE_SRC_STATIC;
	net_core_dhcp_start(&r);
	expect("dhcp: the link down is LINK in the board's words, nothing called",
	       r.status == NET_SVC_LINK && strcmp(r.why, "board: link down") == 0 &&
	       fk.nlog == 0 && S->source == NET_CORE_SRC_STATIC && fk.ip == IP_S &&
	       S->dhcp_running == 0);
	no_lock_left("dhcp link");

	fresh(false);
	net_core_dhcp_start(&r);
	expect("dhcp: no client is REFUSED, nothing called",
	       r.status == NET_SVC_REFUSED && fk.nlog == 0);

	/* Not running: reinit, drop, start. */
	fresh(true);
	fk.ip = IP_S;
	fk.mask = MASK24;
	fk.gw = GW_A;
	S->source = NET_CORE_SRC_STATIC;
	S->lease_stale = 1u;
	net_core_dhcp_start(&r);
	expect("dhcp: not running -> reinit, drop the address, start (OK)",
	       r.status == NET_SVC_OK &&
	       calls_are("reinit, addr_set 0, gw_clear, start") && fk.ip == 0u &&
	       fk.gw == 0u && S->dhcp_running == 1 && S->source == NET_CORE_SRC_DHCP &&
	       S->lease_stale == 0);
	no_lock_left("dhcp start");

	/* Bound: renew only. */
	fresh(true);
	bound(IP_A, GW_A);
	net_core_dhcp_start(&r);
	expect("dhcp: a held lease that is not stale is renewed in place (RENEWED)",
	       r.status == NET_SVC_RENEWED && calls_are("renew") && fk.ip == IP_A &&
	       S->dhcp_running == 1 && S->lease_stale == 0);

	/* Running without a lease: stop, reinit, drop, start. */
	fresh(true);
	net_core_dhcp_start(&r);
	clear_calls();
	net_core_dhcp_start(&r);
	expect("dhcp: running without a lease -> stop, reinit, drop, start (OK)",
	       r.status == NET_SVC_OK &&
	       calls_are("renew, stop, reinit, addr_set 0, gw_clear, start") &&
	       S->dhcp_running == 1 && fk.dstate == D_ACQUIRING);

	/* After `net ip` stopped the client, `net dhcp` must reinitialise. */
	fresh(true);
	bound(IP_A, GW_A);
	net_core_set_static(IP_S, MASK24, GW_A, &r);
	clear_calls();
	net_core_dhcp_start(&r);
	expect("dhcp: after `net ip` the client is reinitialised before it starts",
	       r.status == NET_SVC_OK && S->dhcp_running == 1 &&
	       fk.dstate == D_ACQUIRING && S->source == NET_CORE_SRC_DHCP);

	/* Failures move the state only by what succeeded. */
	fresh(true);
	net_core_dhcp_start(&r);           /* acquiring */
	fk.ip = IP_A;                      /* an address that is not a lease */
	fk.fail_stop = 0x22u;
	clear_calls();
	net_core_dhcp_start(&r);
	expect("dhcp: a failed stop is REFUSED and the client stays running",
	       r.status == NET_SVC_REFUSED && calls_are("renew, stop") &&
	       S->dhcp_running == 1 && fk.ip == IP_A);

	fresh(true);
	net_core_dhcp_start(&r);
	S->source = NET_CORE_SRC_DHCP;
	fk.ip = IP_A;
	fk.fail_reinit = 0x24u;
	clear_calls();
	net_core_dhcp_start(&r);
	expect("dhcp: a failed reinit after the stop: stopped, address kept",
	       r.status == NET_SVC_REFUSED && calls_are("renew, stop, reinit") &&
	       S->dhcp_running == 0 && fk.ip == IP_A);

	fresh(true);
	fk.ip = IP_S;
	S->source = NET_CORE_SRC_STATIC;
	fk.fail_reinit = 0x24u;
	net_core_dhcp_start(&r);
	expect("dhcp: a failed reinit from stopped changes nothing",
	       r.status == NET_SVC_REFUSED && calls_are("reinit") && fk.ip == IP_S &&
	       S->source == NET_CORE_SRC_STATIC && S->dhcp_running == 0);

	fresh(true);
	fk.ip = IP_S;
	S->source = NET_CORE_SRC_STATIC;
	S->lease_stale = 1u;
	fk.fail_start = 0x23u;
	net_core_dhcp_start(&r);
	expect("dhcp: a failed start: address dropped, source DHCP, not running",
	       r.status == NET_SVC_REFUSED && fk.ip == 0u &&
	       S->source == NET_CORE_SRC_DHCP && S->dhcp_running == 0 &&
	       S->lease_stale == 0);

	/* Recorded running while NetX has the client stopped: recovered. */
	fresh(true);
	net_core_dhcp_start(&r);
	fk.dstate = D_IDLE;
	fk.needs_init = 1;
	clear_calls();
	net_core_dhcp_start(&r);
	expect("dhcp: recorded running but stopped in NetX -> renew, stop "
	       "(not started), reinit, drop, start",
	       r.status == NET_SVC_OK &&
	       calls_are("renew, stop, reinit, addr_set 0, gw_clear, start") &&
	       S->dhcp_running == 1 && fk.dstate == D_ACQUIRING);

	fresh(true);
	bound(IP_A, GW_A);
	fk.fail_renew = 0x25u;
	net_core_dhcp_start(&r);
	expect("dhcp: a renew refused for another reason changes nothing",
	       r.status == NET_SVC_REFUSED && calls_are("renew") &&
	       S->dhcp_running == 1 && fk.ip == IP_A);
	no_lock_left("dhcp failures");
}

/* ---- net ping ---------------------------------------------------------------- */

static void check_ping(void)
{
	struct net_svc_result r;
	unsigned rtt = 0;

	fresh(true);
	net_core_link_note(false);
	clear_calls();
	net_core_ping(GW_A, 1000u, &rtt, &r);
	expect("ping: the link down is LINK, no probe sent",
	       r.status == NET_SVC_LINK && strcmp(r.why, "board: link down") == 0 &&
	       fk.nlog == 0);

	fresh(true);
	fk.ping_rc = NX_NO_RESPONSE;
	fk.ping_ms = 1000u;
	net_core_ping(GW_A, 1000u, &rtt, &r);
	expect("ping: no reply is TIMEOUT, without the lock",
	       r.status == NET_SVC_TIMEOUT && calls_are("ping") && fk.takes == 0);

	fresh(true);
	fk.ping_rc = NX_SUCCESS;
	fk.ping_ms = 3u;
	rtt = 0;
	net_core_ping(GW_A, 1000u, &rtt, &r);
	expect("ping: a reply is OK with its round trip, the packet released",
	       r.status == NET_SVC_OK && rtt == 3u && calls_are("ping, release") &&
	       fk.takes == 0);

	fresh(true);
	fk.ping_rc = 0x01u;
	net_core_ping(GW_A, 1000u, &rtt, &r);
	expect("ping: any other NetX answer is REFUSED", r.status == NET_SVC_REFUSED);
	no_lock_left("ping");
}

/* ---- the stale mark ------------------------------------------------------- */

static void check_stale(void)
{
	struct net_svc_result r;

	fresh(true);
	bound(IP_A, GW_A);
	(void)net_core_link_lost(false);
	expect("stale: a DHCP address under a lost link is marked", S->lease_stale == 1);
	net_core_link_note(true);
	net_core_set_static(IP_S, MASK24, GW_A, &r);
	expect("stale: `net ip` clears it", r.status == NET_SVC_OK && S->lease_stale == 0);

	fresh(true);
	bound(IP_A, GW_A);
	(void)net_core_link_lost(false);
	net_core_link_note(true);
	clear_calls();
	net_core_dhcp_start(&r);
	expect("stale: `net dhcp` acquires from scratch (no renew) and clears it",
	       r.status == NET_SVC_OK &&
	       calls_are("stop, reinit, addr_set 0, gw_clear, start") &&
	       fk.ip == 0u && S->dhcp_running == 1 && S->lease_stale == 0);
	fresh(true);
	bound(IP_A, GW_A);
	S->lease_stale = 1u;
	fk.fail_stop = 0x22u;
	net_core_dhcp_start(&r);
	expect("stale: a failed stop is REFUSED, the stale lease and mark kept",
	       r.status == NET_SVC_REFUSED && calls_are("stop") && fk.ip == IP_A &&
	       S->dhcp_running == 1 && S->lease_stale == 1);
}

/* ---- the temporary link entries and the KV source --------------------------- */

static void check_link_entries(void)
{
	struct net_svc_result r;
	UINT st;

	/* The note: one masked store, nothing else. */
	fresh(true);
	net_core_link_note(false);
	expect("note: one masked section, no NetX call, no lock",
	       fk.irq_sections == 1 && fk.irq_depth == 0 && fk.nlog == 0 &&
	       fk.takes == 0 && S->link_up == 0);
	net_core_link_note(true);
	expect("note: up", S->link_up == 1);

	/* f746: start after a link-up. */
	fresh(true);
	fk.ip = IP_A;
	st = net_core_link_up_start();
	expect("up-start: up, DHCP, stopped -> reinit, drop, start",
	       st == NX_SUCCESS && calls_are("reinit, addr_set 0, gw_clear, start") &&
	       S->dhcp_running == 1);
	clear_calls();
	st = net_core_link_up_start();
	expect("up-start: already running -> nothing", st == NX_SUCCESS && fk.nlog == 0);

	fresh(true);
	S->source = NET_CORE_SRC_STATIC;
	st = net_core_link_up_start();
	expect("up-start: static -> nothing", st == NX_SUCCESS && fk.nlog == 0 &&
	       S->dhcp_running == 0);
	fresh(true);
	S->source = NET_CORE_SRC_NONE;
	(void)net_core_link_up_start();
	expect("up-start: no source -> nothing", fk.nlog == 0);
	fresh(true);
	net_core_link_note(false);
	clear_calls();
	(void)net_core_link_up_start();
	expect("up-start: link down -> nothing", fk.nlog == 0);
	fresh(false);
	(void)net_core_link_up_start();
	expect("up-start: no client -> nothing", fk.nlog == 0 && fk.takes == 0);
	fresh(true);
	fk.fail_start = 0x23u;
	st = net_core_link_up_start();
	expect("up-start: a failed start returns NetX's status, not running",
	       st == 0x23u && S->dhcp_running == 0);
	no_lock_left("up-start");

	/* wio: the link is lost. */
	fresh(true);
	bound(IP_A, GW_A);
	st = net_core_link_lost(true);
	expect("lost: records down, stops, marks a DHCP address stale",
	       st == NX_SUCCESS && S->link_up == 0 && S->dhcp_running == 0 &&
	       S->lease_stale == 1 && fk.ip == IP_A && fk.takes == 1 &&
	       strstr(calls(), "stop") != NULL);
	fresh(true);
	bound(IP_A, GW_A);
	(void)net_core_link_lost(false);
	expect("lost without stop: the client keeps running, still stale",
	       S->dhcp_running == 1 && S->lease_stale == 1 &&
	       strstr(calls(), "stop") == NULL && S->link_up == 0);
	fresh(true);
	S->source = NET_CORE_SRC_STATIC;
	fk.ip = IP_S;
	(void)net_core_link_lost(true);
	expect("lost: a static address is not stale", S->lease_stale == 0);
	fresh(true);
	net_core_dhcp_start(&r);          /* running, no address yet */
	(void)net_core_link_lost(true);
	expect("lost: no address, nothing stale", S->lease_stale == 0 &&
	       S->dhcp_running == 0);
	fresh(true);
	bound(IP_A, GW_A);
	fk.fail_stop = 0x22u;
	st = net_core_link_lost(true);
	expect("lost: a stop NetX refused keeps the client running, status returned",
	       st == 0x22u && S->dhcp_running == 1);
	no_lock_left("lost");

	/* A `net dhcp` after the link was lost is LINK even before the IP thread's
	   callback ran. */
	net_core_dhcp_start(&r);
	expect("lost: a later `net dhcp` is LINK", r.status == NET_SVC_LINK);

	/* KV: source DHCP even with the link recorded down. */
	fresh(true);
	net_core_link_note(false);
	clear_calls();
	net_core_dhcp_source(&r);
	expect("kv: the link down does not refuse it; it starts the client",
	       r.status == NET_SVC_OK && S->source == NET_CORE_SRC_DHCP &&
	       S->dhcp_running == 1 && calls_are("reinit, addr_set 0, gw_clear, start"));
	clear_calls();
	net_core_dhcp_source(&r);
	expect("kv: already running -> nothing more", r.status == NET_SVC_OK &&
	       fk.nlog == 0);
	fresh(false);
	net_core_dhcp_source(&r);
	expect("kv: no client is REFUSED", r.status == NET_SVC_REFUSED && fk.nlog == 0);
	no_lock_left("kv");
}

int main(void)
{
	printf("test_net_core:\n");
	check_init();
	check_addr();
	check_static();
	check_dhcp();
	check_ping();
	check_stale();
	check_link_entries();
	if (failures != 0) {
		printf("test_net_core: %d failure(s)\n", failures);
		return 1;
	}
	printf("test_net_core: all passed\n");
	return 0;
}
