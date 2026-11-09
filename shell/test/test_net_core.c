/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Host test: the one implementation of the address, DHCP and ping operations
 * and of the link notification and worker (shell/cmds/net_core.c, issue
 * #220), compiled as it is, over fake NetX / DHCP / ThreadX calls (nx_shim/,
 * shim/tx_api.h) that record every call, whether the address lock was held
 * for it, and the masked sections.  The worker's thread is not run: one pass
 * (net_core_link_react()) is called directly, and the notification is the
 * function net_core_init() registered with the fake NetX.
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
 *  - init creates the lock, the event flags, the worker (priority, stack)
 *    and registers the notification, and any one failing leaves ready clear;
 *  - the notification: NetX's interface flag, the level and the fall mark in
 *    one masked section, the wake after it; no NetX call, no lock;
 *  - one worker pass: the copy and the clearing of the mark in one section,
 *    then under the lock: a fall stops a running client (the address stays)
 *    and marks a DHCP address stale (not a static one); the link up with DHCP
 *    the source and the client stopped starts it; no source, static, or no
 *    client starts nothing; up -> down -> up stops and starts again, down ->
 *    up -> down only stops, a repeated up does nothing;
 *  - a notice that arrives after the copy leaves a new mark and a new wake,
 *    and the next pass handles it;
 *  - a bounce, then `net dhcp`: the next pass stops and restarts that client,
 *    and it ends running;
 *  - a failed stop, reinitialise or start moves the state only by what
 *    succeeded and is kept as the last failure (step, NetX's answer, the
 *    level); a later success -- a pass, `net dhcp`, `net ip` -- clears it, a
 *    pass with nothing to do does not;
 *  - the KV "source DHCP": not refused with the link down, starts nothing by
 *    itself, wakes the worker -- which starts the client if the up was
 *    already handled, or on the up;
 *  - the worker thread waits for its flag and runs one pass per wake;
 *  - the fall count (issue #221): every down notice adds one, inside the
 *    notification's masked section, before the worker runs; an up adds
 *    nothing; a pass that handled a fall calls the telnet console's
 *    net_tty_link_fell() once, after the address lock is released, and a
 *    pass without a fall does not call it.
 */
#include <setjmp.h>
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
static ULONG   fake_worker_stack[256];

struct net_core_state net_core_board_state;
const struct net_core_facts net_core_board_facts = {
	.ip          = &fake_ip,
	.dhcp        = &fake_dhcp,
	.iface       = 0u,
	.boot_source = NET_CORE_SRC_DHCP,
	.why_link    = "board: link down",
	.why_down    = "board: not up",
	.worker_stack      = fake_worker_stack,
	.worker_stack_size = sizeof fake_worker_stack,
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
	/* the event flags, the worker thread and the notification */
	int flags_fail, flags_created, thread_fail, threads, notify_fail, notifies;
	int wakes, wakes_masked, wakes_locked, waits;
	VOID (*entry)(ULONG);
	VOID *stack; ULONG stack_size; UINT prio, preempt, autostart; ULONG slice;
	VOID (*notify)(NX_IP *, UINT, UINT);
	NX_IP *notify_ip;
	/* the fall count at the open / close of the last masked section, and
	 * the telnet console's wake (net_tty_link_fell()) */
	ULONG falls_at_disable, falls_at_restore;
	int tty_wakes, tty_wakes_locked, tty_wakes_masked;
	/* run inside a DHCP stop / start, after the worker's copy */
	void (*during_stop)(void), (*during_start)(void);
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

UINT tx_event_flags_create(TX_EVENT_FLAGS_GROUP *g, CHAR *name)
{
	(void)g; (void)name;
	if (fk.flags_fail)
		return 0x06u;
	fk.flags_created++;
	return TX_SUCCESS;
}

UINT tx_event_flags_set(TX_EVENT_FLAGS_GROUP *g, ULONG flags, UINT opt)
{
	(void)g; (void)flags; (void)opt;
	fk.wakes++;
	if (fk.irq_depth != 0)
		fk.wakes_masked++;
	if (fk.held)
		fk.wakes_locked++;
	return TX_SUCCESS;
}

/* The worker's wait: the first call returns a wake, the second leaves the
 * worker's loop (the thread-body test). */
static jmp_buf worker_out;

UINT tx_event_flags_get(TX_EVENT_FLAGS_GROUP *g, ULONG req, UINT opt,
                        ULONG *got, ULONG wait)
{
	(void)g;
	fk.waits++;
	if (req != 0x1u || opt != TX_OR_CLEAR || wait != TX_WAIT_FOREVER)
		fk.waits += 1000;            /* not the worker's wait */
	if (fk.waits > 1)
		longjmp(worker_out, 1);
	*got = req;
	return TX_SUCCESS;
}

UINT tx_thread_create(TX_THREAD *t, CHAR *name, VOID (*entry)(ULONG),
                      ULONG input, VOID *stack, ULONG size, UINT prio,
                      UINT preempt, ULONG slice, UINT autostart)
{
	(void)t; (void)name; (void)input;
	if (fk.thread_fail)
		return 0x0Fu;
	fk.threads++;
	fk.entry = entry;
	fk.stack = stack;
	fk.stack_size = size;
	fk.prio = prio;
	fk.preempt = preempt;
	fk.slice = slice;
	fk.autostart = autostart;
	return TX_SUCCESS;
}

UINT nx_ip_link_status_change_notify_set(NX_IP *ip,
        VOID (*cb)(NX_IP *, UINT, UINT))
{
	if (fk.notify_fail)
		return 0x07u;
	fk.notifies++;
	fk.notify = cb;
	fk.notify_ip = ip;
	return NX_SUCCESS;
}

UINT tx_shim_disable(void)
{
	fk.irq_depth++;
	fk.irq_sections++;
	fk.falls_at_disable = net_core_board_state.link_falls;
	return 0u;
}

void tx_shim_restore(UINT saved)
{
	(void)saved;
	fk.falls_at_restore = net_core_board_state.link_falls;
	fk.irq_depth--;
}

/* The telnet console's entry the worker calls (shell/cmds/net_tty.c). */
void net_tty_link_fell(void)
{
	fk.tty_wakes++;
	if (fk.held)
		fk.tty_wakes_locked++;
	if (fk.irq_depth != 0)
		fk.tty_wakes_masked++;
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
	if (fk.during_start != NULL)
		fk.during_start();
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
	if (fk.during_stop != NULL)
		fk.during_stop();
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
	fk.wakes = 0;
	fk.wakes_masked = 0;
	fk.wakes_locked = 0;
}

/* The IP thread reports the link: the notification init registered. */
static void link(bool up)
{
	fk.notify(&fake_ip, 0u, up ? 1u : 0u);
}

/* One pass of the worker. */
static void react(void)
{
	net_core_link_react();
}

#define S (&net_core_board_state)

/* A ready core, link up (notified; the worker has not run), nothing
 * addressed, the client not running. */
static void fresh(bool dhcp_created)
{
	memset(&fk, 0, sizeof fk);
	memset(&net_core_board_state, 0, sizeof net_core_board_state);
	memset(&fake_ip, 0, sizeof fake_ip);
	fk.mask = 0u;
	(void)net_core_init(dhcp_created);
	link(true);
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
	expect("before init: the KV source is DOWN, no wake",
	       r.status == NET_SVC_DOWN && fk.nlog == 0 && fk.wakes == 0);
	react();
	expect("before init: a worker pass does nothing",
	       fk.nlog == 0 && fk.takes == 0 && fk.irq_sections == 0);

	fk.mutex_fail = 1;
	expect("init: a lock that cannot be created fails, ready stays clear",
	       net_core_init(true) == -1 && S->ready == 0);
	memset(&fk, 0, sizeof fk);
	fk.flags_fail = 1;
	expect("init: event flags that cannot be created fail, ready stays clear",
	       net_core_init(true) == -1 && S->ready == 0);
	memset(&fk, 0, sizeof fk);
	fk.thread_fail = 1;
	expect("init: a worker that cannot be created fails, ready stays clear",
	       net_core_init(true) == -1 && S->ready == 0 && fk.notifies == 0);
	memset(&fk, 0, sizeof fk);
	fk.notify_fail = 1;
	expect("init: a notification NetX refuses fails, ready stays clear",
	       net_core_init(true) == -1 && S->ready == 0);
	net_core_addr_get(&a, &r);
	expect("init: ... and the operations stay DOWN", r.status == NET_SVC_DOWN);

	memset(&fk, 0, sizeof fk);
	expect("init: ready, the lock created once, the boot source DHCP",
	       net_core_init(true) == 0 && S->ready == 1 && fk.mutex_created == 1 &&
	       S->source == NET_CORE_SRC_DHCP && S->dhcp_running == 0 &&
	       S->link_up == 0 && S->link_fell == 0 && S->dhcp_created == 1 &&
	       fk.nlog == 0 && S->last_fail.op == NET_CORE_OP_NONE);
	expect("init: the flags, the worker (priority 15, the board's stack, "
	       "auto start, no time slice) and the notification on the board's IP",
	       fk.flags_created == 1 && fk.threads == 1 &&
	       fk.prio == NET_CORE_WORKER_PRIORITY && fk.preempt == fk.prio &&
	       fk.stack == (VOID *)fake_worker_stack &&
	       fk.stack_size == sizeof fake_worker_stack &&
	       fk.autostart == TX_AUTO_START && fk.slice == TX_NO_TIME_SLICE &&
	       fk.notifies == 1 && fk.notify != NULL && fk.notify_ip == &fake_ip &&
	       fk.wakes == 0);
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

	link(false);
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
	link(false);
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
	link(false);
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
	link(false);
	react();
	expect("stale: a DHCP address under a fallen link is marked, the client "
	       "stopped, the address kept",
	       S->lease_stale == 1 && S->dhcp_running == 0 && fk.ip == IP_A);
	link(true);
	S->source = NET_CORE_SRC_DHCP;
	net_core_set_static(IP_S, MASK24, GW_A, &r);
	expect("stale: `net ip` clears it", r.status == NET_SVC_OK && S->lease_stale == 0);

	fresh(true);
	bound(IP_A, GW_A);
	link(false);
	react();
	link(true);                        /* the worker has not run for it yet */
	clear_calls();
	net_core_dhcp_start(&r);
	expect("stale: `net dhcp` acquires from scratch (no renew) and clears it",
	       r.status == NET_SVC_OK &&
	       calls_are("reinit, addr_set 0, gw_clear, start") &&
	       fk.ip == 0u && S->dhcp_running == 1 && S->lease_stale == 0);

	/* Still running with a stale lease (the worker's stop had failed). */
	fresh(true);
	bound(IP_A, GW_A);
	S->lease_stale = 1u;
	net_core_dhcp_start(&r);
	expect("stale: running and stale -> stop, reinit, drop, start, no renew",
	       r.status == NET_SVC_OK &&
	       calls_are("stop, reinit, addr_set 0, gw_clear, start") &&
	       S->dhcp_running == 1 && S->lease_stale == 0);
	fresh(true);
	bound(IP_A, GW_A);
	S->lease_stale = 1u;
	fk.fail_stop = 0x22u;
	net_core_dhcp_start(&r);
	expect("stale: a failed stop is REFUSED, the stale lease and mark kept",
	       r.status == NET_SVC_REFUSED && calls_are("stop") && fk.ip == IP_A &&
	       S->dhcp_running == 1 && S->lease_stale == 1);
}

/* ---- the notification ------------------------------------------------------- */

static void check_notify(void)
{
	fresh(true);
	expect("notify: up -> NetX's flag up, the level up, no fall mark, one wake",
	       fake_ip.nx_ip_interface[0].nx_interface_link_up == 1 &&
	       S->link_up == 1 && S->link_fell == 0);
	link(false);
	expect("notify: down -> one masked section, no NetX call, no lock",
	       fk.irq_sections == 1 && fk.irq_depth == 0 && fk.nlog == 0 &&
	       fk.takes == 0);
	expect("notify: ... NetX's flag down, the level down, the fall marked",
	       fake_ip.nx_ip_interface[0].nx_interface_link_up == 0 &&
	       S->link_up == 0 && S->link_fell == 1);
	expect("notify: ... one wake, set outside the masked section",
	       fk.wakes == 1 && fk.wakes_masked == 0 && fk.wakes_locked == 0);
	clear_calls();
	link(true);
	expect("notify: an up after a fall keeps the mark for the worker",
	       S->link_up == 1 && S->link_fell == 1 && fk.wakes == 1 &&
	       fk.nlog == 0 && fk.takes == 0);
	clear_calls();
	link(true);
	expect("notify: a repeated up wakes again (the worker decides)",
	       fk.wakes == 1 && S->link_up == 1);

	/* Another interface: NetX's own flag, nothing of ours. */
	fresh(true);
	fk.notify(&fake_ip, 1u, 0u);
	expect("notify: another interface moves only NetX's flag for it",
	       fake_ip.nx_ip_interface[1].nx_interface_link_up == 0 &&
	       S->link_up == 1 && S->link_fell == 0 && fk.wakes == 0);
}

/* ---- the worker's reactions ------------------------------------------------ */

static void check_worker(void)
{
	struct net_svc_result r;

	/* Fall under a held lease. */
	fresh(true);
	bound(IP_A, GW_A);
	link(false);
	clear_calls();
	react();
	expect("worker: a fall stops the client under the lock, keeps the address",
	       calls_are("stop, addr_get") && fk.locked[0] && fk.locked[1] &&
	       S->dhcp_running == 0 && fk.ip == IP_A && fk.gw == GW_A &&
	       S->lease_stale == 1 && S->link_fell == 0);
	expect("worker: ... one hold of the lock, one masked section (the copy)",
	       fk.takes == 1 && fk.irq_sections == 1);
	no_lock_left("worker fall");
	clear_calls();
	react();
	expect("worker: the next pass finds nothing to do", fk.nlog == 0 &&
	       S->dhcp_running == 0 && S->lease_stale == 1);

	/* Fall with a static address. */
	fresh(true);
	net_core_set_static(IP_S, MASK24, GW_A, &r);
	link(false);
	clear_calls();
	react();
	expect("worker: a fall under a static address: no stop, not stale",
	       strstr(calls(), "stop") == NULL && S->lease_stale == 0 &&
	       fk.ip == IP_S && S->source == NET_CORE_SRC_STATIC);

	/* Fall with DHCP running but no address yet. */
	fresh(true);
	net_core_dhcp_start(&r);
	link(false);
	react();
	expect("worker: a fall before any lease: stopped, nothing stale",
	       S->dhcp_running == 0 && S->lease_stale == 0);

	/* Up, the source DHCP, not running: start. */
	fresh(true);
	fk.ip = IP_A;
	S->lease_stale = 1u;
	react();
	expect("worker: up, DHCP, stopped -> reinit, drop, start under the lock",
	       calls_are("reinit, addr_set 0, gw_clear, start") && fk.locked[0] &&
	       fk.locked[3] && S->dhcp_running == 1 && fk.ip == 0u &&
	       S->lease_stale == 0 && fk.dstate == D_ACQUIRING);
	no_lock_left("worker up");

	fresh(true);
	S->source = NET_CORE_SRC_NONE;
	react();
	expect("worker: up with no source starts nothing", fk.nlog == 0 &&
	       S->dhcp_running == 0);
	fresh(true);
	S->source = NET_CORE_SRC_STATIC;
	react();
	expect("worker: up with a static source starts nothing", fk.nlog == 0);
	fresh(false);
	S->source = NET_CORE_SRC_DHCP;
	react();
	expect("worker: up with no DHCP client starts nothing", fk.nlog == 0);
	fresh(true);
	link(false);
	clear_calls();
	react();
	expect("worker: down, DHCP, stopped: nothing to stop, nothing starts",
	       strstr(calls(), "start") == NULL && strstr(calls(), "stop") == NULL &&
	       S->dhcp_running == 0);

	/* A static `net ip` that got the lock before the worker. */
	fresh(true);
	net_core_set_static(IP_S, MASK24, GW_A, &r);
	clear_calls();
	react();
	expect("worker: decided under the lock -- a static address got there first",
	       fk.nlog == 0 && S->source == NET_CORE_SRC_STATIC);

	/* After a static address, `net dhcp` while the link is up. */
	fresh(true);
	react();                           /* the boot up: DHCP starts */
	lease_arrives(IP_A, GW_A);
	clear_calls();
	link(true);                        /* speed / duplex change */
	react();
	expect("worker: a repeated up with the client running does nothing",
	       fk.nlog == 0 && S->dhcp_running == 1 && fk.ip == IP_A);
}

/* ---- folding several notices into one pass ----------------------------- */

static void check_fold(void)
{
	fresh(true);
	bound(IP_A, GW_A);
	link(false);
	link(true);
	clear_calls();
	react();
	expect("fold: up -> down -> up in one pass stops, then starts again",
	       calls_are("stop, addr_get, reinit, addr_set 0, gw_clear, start") &&
	       S->dhcp_running == 1 && fk.dstate == D_ACQUIRING &&
	       S->lease_stale == 0 && fk.ip == 0u);

	fresh(true);
	bound(IP_A, GW_A);
	link(false);
	link(true);
	link(false);
	clear_calls();
	react();
	expect("fold: down -> up -> down in one pass only stops",
	       calls_are("stop, addr_get") && S->dhcp_running == 0 &&
	       S->lease_stale == 1 && fk.ip == IP_A);

	fresh(true);
	bound(IP_A, GW_A);
	link(true);
	link(true);
	clear_calls();
	react();
	expect("fold: repeated ups with the client running do nothing",
	       fk.nlog == 0 && S->dhcp_running == 1);
}

/* ---- a notice after the worker's copy --------------------------------------- */

static void up_now(void)   { fk.during_stop = NULL; fk.during_start = NULL; link(true); }
static void down_now(void) { fk.during_stop = NULL; fk.during_start = NULL; link(false); }

static void check_after_copy(void)
{
	/* The link comes back while the worker is stopping the client. */
	fresh(true);
	bound(IP_A, GW_A);
	link(false);
	clear_calls();
	fk.during_stop = up_now;
	react();
	expect("after copy: an up during the stop is not acted on by this pass",
	       S->dhcp_running == 0 && strstr(calls(), "start") == NULL &&
	       S->link_up == 1 && S->link_fell == 0);
	expect("after copy: ... it woke the worker again", fk.wakes == 1);
	clear_calls();
	react();
	expect("after copy: ... and the next pass starts the client",
	       calls_are("reinit, addr_set 0, gw_clear, start") &&
	       S->dhcp_running == 1);

	/* The link falls while the worker is starting the client. */
	fresh(true);
	clear_calls();
	fk.during_start = down_now;
	react();
	expect("after copy: a fall during the start leaves a new mark and a wake",
	       S->dhcp_running == 1 && S->link_fell == 1 && S->link_up == 0 &&
	       fk.wakes == 1);
	clear_calls();
	react();
	expect("after copy: ... and the next pass stops the client",
	       strstr(calls(), "stop") != NULL && S->dhcp_running == 0 &&
	       S->link_fell == 0);
}

/* ---- a bounce the worker has not handled, then `net dhcp` ------------------- */

static void check_bounce_then_dhcp(void)
{
	struct net_svc_result r;

	/* Not running before (static), the operator asks for DHCP. */
	fresh(true);
	net_core_set_static(IP_S, MASK24, GW_A, &r);
	react();
	link(false);
	link(true);
	net_core_dhcp_start(&r);
	expect("bounce: `net dhcp` after an unhandled down -> up starts (OK)",
	       r.status == NET_SVC_OK && S->dhcp_running == 1);
	clear_calls();
	react();
	expect("bounce: the pending fall stops that client and the pass starts it "
	       "again -- it ends running",
	       calls_are("stop, addr_get, reinit, addr_set 0, gw_clear, start") &&
	       S->dhcp_running == 1 && fk.dstate == D_ACQUIRING &&
	       S->source == NET_CORE_SRC_DHCP && S->lease_stale == 0);

	/* A held lease, renewed by the operator between the bounce and the pass. */
	fresh(true);
	bound(IP_A, GW_A);
	link(false);
	link(true);
	net_core_dhcp_start(&r);
	expect("bounce: a held lease is renewed by `net dhcp`",
	       r.status == NET_SVC_RENEWED && S->dhcp_running == 1);
	react();
	expect("bounce: ... the pass still stops it and acquires again (running)",
	       S->dhcp_running == 1 && fk.dstate == D_ACQUIRING && fk.ip == 0u &&
	       S->lease_stale == 0 && S->link_fell == 0);
}

/* ---- failures and the last-failure record ----------------------------------- */

static void check_failures(void)
{
	struct net_svc_result r;
	struct net_core_failure lf;

	fresh(true);
	expect("fail: none recorded -> the getter says so",
	       !net_core_last_failure(&lf) && fk.takes == 1);

	/* The stop fails: still running, nothing started even with the link up. */
	fresh(true);
	bound(IP_A, GW_A);
	link(false);
	link(true);
	fk.fail_stop = 0x22u;
	clear_calls();
	react();
	expect("fail: a failed stop keeps the client running and starts nothing",
	       calls_are("stop, addr_get") && S->dhcp_running == 1 &&
	       fk.ip == IP_A && S->lease_stale == 1);
	expect("fail: ... recorded as the stop, NetX's answer, the level up",
	       net_core_last_failure(&lf) && lf.op == NET_CORE_OP_STOP &&
	       lf.status == 0x22u && lf.link_up == 1 &&
	       strcmp(net_core_op_name(lf.op), "stop") == 0);
	no_lock_left("fail stop");
	clear_calls();
	link(true);
	react();
	expect("fail: a pass with nothing to do keeps the record",
	       net_core_last_failure(&lf) && lf.op == NET_CORE_OP_STOP);

	/* The stop fails with the link down: the level recorded is down. */
	fresh(true);
	bound(IP_A, GW_A);
	link(false);
	fk.fail_stop = 0x22u;
	react();
	expect("fail: a failed stop with the link down records the level down",
	       net_core_last_failure(&lf) && lf.op == NET_CORE_OP_STOP &&
	       lf.link_up == 0);
	/* The next fall, with the stop working again, clears it. */
	fk.fail_stop = 0u;
	link(false);
	react();
	expect("fail: a later pass whose steps succeed clears the record",
	       !net_core_last_failure(&lf) && S->dhcp_running == 0);

	/* The reinitialise fails: not started, the address kept. */
	fresh(true);
	fk.ip = IP_A;
	fk.fail_reinit = 0x24u;
	react();
	expect("fail: a failed reinit on the up starts nothing, keeps the address",
	       calls_are("reinit") && S->dhcp_running == 0 && fk.ip == IP_A);
	expect("fail: ... recorded as the reinitialise with the level up",
	       net_core_last_failure(&lf) && lf.op == NET_CORE_OP_REINIT &&
	       lf.status == 0x24u && lf.link_up == 1 &&
	       strcmp(net_core_op_name(lf.op), "reinitialise") == 0);

	/* The start fails: the address dropped, the source DHCP, not running. */
	fresh(true);
	fk.ip = IP_A;
	S->lease_stale = 1u;
	fk.fail_start = 0x23u;
	react();
	expect("fail: a failed start: address dropped, DHCP, not running, not stale",
	       fk.ip == 0u && S->source == NET_CORE_SRC_DHCP &&
	       S->dhcp_running == 0 && S->lease_stale == 0);
	expect("fail: ... recorded as the start",
	       net_core_last_failure(&lf) && lf.op == NET_CORE_OP_START &&
	       lf.status == 0x23u && lf.link_up == 1 &&
	       strcmp(net_core_op_name(lf.op), "start") == 0);
	/* No retry: another wake with nothing new does the same once, no loop. */
	fk.fail_start = 0u;
	clear_calls();
	react();
	expect("fail: the next pass (a later wake) starts it, and clears the record",
	       S->dhcp_running == 1 && !net_core_last_failure(&lf));

	/* `net dhcp` and `net ip` successes clear it too. */
	fresh(true);
	fk.fail_start = 0x23u;
	react();
	fk.fail_start = 0u;
	net_core_dhcp_start(&r);
	expect("fail: a successful `net dhcp` clears the record",
	       r.status == NET_SVC_OK && !net_core_last_failure(&lf));
	fresh(true);
	fk.fail_start = 0x23u;
	react();
	net_core_set_static(IP_S, MASK24, GW_A, &r);
	expect("fail: a successful `net ip` clears the record",
	       r.status == NET_SVC_OK && !net_core_last_failure(&lf));
	fresh(true);
	fk.fail_start = 0x23u;
	react();
	net_core_dhcp_start(&r);
	expect("fail: a refused `net dhcp` keeps it",
	       r.status == NET_SVC_REFUSED && net_core_last_failure(&lf) &&
	       lf.op == NET_CORE_OP_START);
	no_lock_left("fail");
}

/* ---- the KV source -------------------------------------------------------- */

static void check_kv(void)
{
	struct net_svc_result r;

	/* The up was already handled (no source then): the KV wakes the worker. */
	fresh(true);
	S->source = NET_CORE_SRC_NONE;
	react();
	clear_calls();
	net_core_dhcp_source(&r);
	expect("kv: link up and handled -> source DHCP, no DHCP call, a wake "
	       "after the lock",
	       r.status == NET_SVC_OK && S->source == NET_CORE_SRC_DHCP &&
	       fk.nlog == 0 && fk.wakes == 1 && fk.wakes_locked == 0 &&
	       S->dhcp_running == 0);
	react();
	expect("kv: ... the woken pass starts the client",
	       calls_are("reinit, addr_set 0, gw_clear, start") &&
	       S->dhcp_running == 1);

	/* The link still down: not refused, the up starts it. */
	fresh(true);
	S->source = NET_CORE_SRC_NONE;
	link(false);
	react();
	clear_calls();
	net_core_dhcp_source(&r);
	expect("kv: the link down does not refuse it", r.status == NET_SVC_OK &&
	       S->source == NET_CORE_SRC_DHCP && fk.wakes == 1);
	react();
	expect("kv: ... the pass with the link down starts nothing",
	       strstr(calls(), "start") == NULL && S->dhcp_running == 0);
	link(true);
	react();
	expect("kv: ... the up starts it", S->dhcp_running == 1);

	fresh(false);
	net_core_dhcp_source(&r);
	expect("kv: no client is REFUSED, no wake",
	       r.status == NET_SVC_REFUSED && fk.nlog == 0 && fk.wakes == 0);
	no_lock_left("kv");
}

/* ---- the worker thread --------------------------------------------------- */

static void check_thread(void)
{
	fresh(true);
	if (setjmp(worker_out) == 0)
		fk.entry(0u);
	expect("thread: waits for its flag (or-clear, forever) and runs one pass "
	       "per wake",
	       fk.waits == 2 && S->dhcp_running == 1 &&
	       calls_are("reinit, addr_set 0, gw_clear, start"));
	fk.held = 0;                       /* left the loop between passes */
}

/* ---- the fall count and the telnet console's wake (issue #221) ------------- */

static void check_fall_count(void)
{
	fresh(true);
	expect("falls: an up counts nothing", S->link_falls == 0u);
	clear_calls();
	link(false);
	expect("falls: a down counts one, inside the notification's masked section",
	       S->link_falls == 1u && fk.irq_sections == 1 &&
	       fk.falls_at_disable == 0u && fk.falls_at_restore == 1u);
	expect("falls: ... before the worker ran (the count is the notice's)",
	       fk.tty_wakes == 0 && S->link_fell == 1);
	link(true);
	link(false);
	expect("falls: a second down counts again, the up between nothing",
	       S->link_falls == 2u);
	react();
	expect("falls: the worker does not count", S->link_falls == 2u);
	expect("falls: a pass that handled a fall wakes the telnet console once, "
	       "with the address lock released and interrupts on",
	       fk.tty_wakes == 1 && fk.tty_wakes_locked == 0 &&
	       fk.tty_wakes_masked == 0 && !fk.held);
	link(true);
	react();
	expect("falls: a pass without a fall does not wake it", fk.tty_wakes == 1);

	/* A fall under a running client whose stop fails still wakes it. */
	fresh(true);
	bound(IP_A, GW_A);
	fk.fail_stop = 0x22u;
	link(false);
	react();
	expect("falls: a fall whose DHCP stop failed still wakes the console",
	       fk.tty_wakes == 1 && fk.tty_wakes_locked == 0);
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
	check_notify();
	check_worker();
	check_fold();
	check_after_copy();
	check_bounce_then_dhcp();
	check_failures();
	check_kv();
	check_thread();
	check_fall_count();
	if (failures != 0) {
		printf("test_net_core: %d failure(s)\n", failures);
		return 1;
	}
	printf("test_net_core: all passed\n");
	return 0;
}
