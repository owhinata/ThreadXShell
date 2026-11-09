/* shared-command: optional BOARD_HAS_NETX */
/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    net_core.c
 * @brief   The one implementation of the address, DHCP and ping operations
 *          over NetX Duo (issue #220, epic #212).  See net_core.h.
 *
 * [!] THIS FILE OWNS NO STORAGE: the state and the facts are the board's
 * (net_core_board_state / net_core_board_facts), reached by name.
 *
 * [!] NO BOARD IN HERE: no `#ifdef <BOARD>`, no board header, no HAL.  What
 * differs between boards is in the facts, and a board's words come back in
 * the result's `why`.
 *
 * Every DHCP call is the INTERFACE-scoped variant with the board's index: the
 * global nx_dhcp_stop() / nx_dhcp_reinitialize() / nx_dhcp_force_renew() loop
 * over the records, discard each status and return NX_SUCCESS
 * (nxd_dhcp_client.c), so their answer means nothing.
 */
#include "net_core.h"

#include <stddef.h>
#include <stdint.h>

#include "cli_config.h"       /* CLI_INSTANCE_PRIORITY */
#include "net_tty.h"          /* net_tty_link_fell() (issue #221) */

/* The worker reacts to the link while a console sits in a command: it must
 * run ahead of every console (ThreadX: lower number, higher priority). */
CLI_STATIC_ASSERT(NET_CORE_WORKER_PRIORITY < CLI_INSTANCE_PRIORITY,
                  "the net link worker must outrank the consoles");

/* The worker's one event flag: "look at the pair again". */
#define NET_CORE_EVT_WAKE ((ULONG)0x1u)

/* The echo's payload.  The probe's wait goes to NetX as ticks, which are
 * milliseconds on both boards (NX_IP_PERIODIC_RATE 1000). */
#define NET_CORE_PING_DATA "threadx-ping"

/* ---- small pieces ----------------------------------------------------------- */

static void net_core_say(struct net_svc_result *res, enum net_svc_status st,
                         const char *why)
{
	res->status = st;
	res->why = why;
}

static void net_core_lock(void)
{
	(void)tx_mutex_get(&net_core_board_state.addr_lock, TX_WAIT_FOREVER);
}

static void net_core_unlock(void)
{
	(void)tx_mutex_put(&net_core_board_state.addr_lock);
}

/* The recorded link level, read in the same masked section it is written in. */
static bool net_core_link_is_up(void)
{
	TX_INTERRUPT_SAVE_AREA
	bool up;

	TX_DISABLE
	up = (net_core_board_state.link_up != 0u);
	TX_RESTORE
	return up;
}

/* Not initialised: every operation says so in the board's words. */
static int net_core_not_ready(struct net_svc_result *res)
{
	if (net_core_board_state.ready)
		return 0;
	net_core_say(res, NET_SVC_DOWN, net_core_board_facts.why_down);
	return 1;
}

/*
 * A gateway, where 0 means "no gateway" (issue #19): NetX refuses 0.0.0.0 as a
 * gateway address (nxe_ip_gateway_address_set.c), so the two cases are
 * different calls.
 */
static UINT net_core_gw_apply(uint32_t gw)
{
	return (gw != 0u)
		? nx_ip_gateway_address_set(net_core_board_facts.ip, (ULONG)gw)
		: nx_ip_gateway_address_clear(net_core_board_facts.ip);
}

/*
 * Give up the address BEFORE the client starts, so that "the interface has an
 * address" means "a lease arrived" and nothing else (issue #20): the client
 * zeroes it itself on the way into INIT, but on its own thread, so a caller
 * watching for an address could read the previous one.  Never on the renew
 * path, whose point is that the lease stays.
 */
static void net_core_drop_address(void)
{
	const struct net_core_facts *f = &net_core_board_facts;

	(void)nx_ip_interface_address_set(f->ip, f->iface, 0u, 0u);
	(void)nx_ip_gateway_address_clear(f->ip);
}

/*
 * Start the client from "not running": reinitialise (mandatory before a
 * restart, and harmless on a client that never ran), drop the address, start.
 * The state moves with each step that succeeded: once the address is dropped
 * the source is DHCP and nothing is stale, whatever the start then answers.
 * ADDRESS LOCK HELD, client not running.  @return the first failure, or
 * NX_SUCCESS; on a failure @p op (when not NULL) says which step.
 */
static UINT net_core_dhcp_begin(uint8_t *op)
{
	struct net_core_state *s = &net_core_board_state;
	const struct net_core_facts *f = &net_core_board_facts;
	UINT st;

	st = nx_dhcp_interface_reinitialize(f->dhcp, f->iface);
	if (st != NX_SUCCESS) {
		if (op != NULL)
			*op = NET_CORE_OP_REINIT;
		return st;
	}
	net_core_drop_address();
	s->source = NET_CORE_SRC_DHCP;
	s->lease_stale = 0u;
	st = nx_dhcp_interface_start(f->dhcp, f->iface);
	if (st != NX_SUCCESS) {
		if (op != NULL)
			*op = NET_CORE_OP_START;
		return st;
	}
	s->dhcp_running = 1u;
	return NX_SUCCESS;
}

/*
 * Stop a running client.  Recorded as stopped only when NetX stopped it, or
 * answered that it was not started; any other answer leaves it running.
 * ADDRESS LOCK HELD.
 */
static UINT net_core_dhcp_stop(void)
{
	const struct net_core_facts *f = &net_core_board_facts;
	UINT st = nx_dhcp_interface_stop(f->dhcp, f->iface);

	if (st != NX_SUCCESS && st != NX_DHCP_NOT_STARTED)
		return st;
	net_core_board_state.dhcp_running = 0u;
	return NX_SUCCESS;
}

/* The DHCP state moved as asked: an older worker failure no longer describes
 * it.  ADDRESS LOCK HELD. */
static void net_core_fail_clear(void)
{
	struct net_core_failure *lf = &net_core_board_state.last_fail;

	lf->op = NET_CORE_OP_NONE;
	lf->link_up = 0u;
	lf->status = NX_SUCCESS;
}

/* ADDRESS LOCK HELD. */
static void net_core_fail_note(uint8_t op, UINT status, uint8_t link_up)
{
	struct net_core_failure *lf = &net_core_board_state.last_fail;

	lf->op = op;
	lf->link_up = link_up;
	lf->status = status;
}

/* ---- the link: notification and worker -------------------------------------- */

/*
 * NetX's link-status callback -- the IP thread, WITH nx_ip_protection HELD
 * (nx_ip_thread_entry.c), for every change the driver flags, including an up
 * that repeats (f746g-disco reports speed and duplex changes this way).
 *
 * Everything here must be non-blocking and NetX-free: the DHCP client takes
 * its own mutex and then nx_ip_protection, so a DHCP call from here closes a
 * lock-order cycle (issue #13), and the address lock would put a NetX mutex
 * above it.  So it only publishes:
 *  - NetX's own interface flag, on the thread NetX itself would write it
 *    from: upstream's deferred link-status processing never assigns it, and
 *    route lookup reads it outside the IP mutex;
 *  - the level and, on a down, the fall mark and the fall count --
 *    together, in one masked section, which is what the worker's copy pairs
 *    with and what the telnet console compares (issue #221);
 *  - the wake, after the section (an event-flag set never blocks; the worker
 *    is below the IP thread, so it does not run until this returns).
 */
static VOID net_core_link_notify(NX_IP *ip, UINT iface, UINT up)
{
	TX_INTERRUPT_SAVE_AREA
	struct net_core_state *s = &net_core_board_state;

	ip->nx_ip_interface[iface].nx_interface_link_up = (UCHAR)(up != 0u);
	if (iface != net_core_board_facts.iface)
		return;
	TX_DISABLE
	s->link_up = (up != 0u) ? 1u : 0u;
	if (up == 0u) {
		s->link_fell = 1u;
		s->link_falls++;
	}
	TX_RESTORE
	(void)tx_event_flags_set(&s->wake, NET_CORE_EVT_WAKE, TX_OR);
}

void net_core_link_react(void)
{
	TX_INTERRUPT_SAVE_AREA
	struct net_core_state *s = &net_core_board_state;
	const struct net_core_facts *f = &net_core_board_facts;
	ULONG ip = 0u, mask = 0u;
	uint8_t up, fell, op = NET_CORE_OP_NONE;
	bool acted = false, failed = false;
	UINT st;

	if (!s->ready)
		return;
	net_core_lock();
	/* The pair, and the mark cleared, in one section: a notice after this
	 * leaves a new mark and a new wake for the next pass. */
	TX_DISABLE
	up = s->link_up;
	fell = s->link_fell;
	s->link_fell = 0u;
	TX_RESTORE

	/*
	 * Decided from the state of THIS moment, under the lock: a `net ip` that
	 * got in first made the source static and nothing starts; a `net dhcp`
	 * that started a client after an unhandled fall sees it stopped here and
	 * started again below (the link is up), losing one acquisition only.
	 */
	if (fell) {
		if (s->dhcp_running) {
			acted = true;
			st = net_core_dhcp_stop();
			if (st != NX_SUCCESS) {
				/* Still recorded running: nothing starts below. */
				failed = true;
				net_core_fail_note(NET_CORE_OP_STOP, st, up);
			}
		}
		(void)nx_ip_interface_address_get(f->ip, f->iface, &ip, &mask);
		if (s->source == NET_CORE_SRC_DHCP && ip != 0u)
			s->lease_stale = 1u;
	}
	if (up && s->dhcp_created && s->source == NET_CORE_SRC_DHCP &&
	    !s->dhcp_running) {
		acted = true;
		st = net_core_dhcp_begin(&op);
		if (st != NX_SUCCESS) {
			failed = true;
			net_core_fail_note(op, st, up);
		}
	}
	if (acted && !failed)
		net_core_fail_clear();
	net_core_unlock();
	/* Outside the lock, and only for a fall: the telnet listener compares
	 * the count, this only wakes it (issue #221). */
	if (fell)
		net_tty_link_fell();
}

/* The worker: wait, react, forever.  Every wake gets a full pass; an extra
 * wake is a pass that finds nothing to do. */
static VOID net_core_worker_entry(ULONG arg)
{
	ULONG got;

	(void)arg;
	for (;;) {
		if (tx_event_flags_get(&net_core_board_state.wake, NET_CORE_EVT_WAKE,
		                       TX_OR_CLEAR, &got, TX_WAIT_FOREVER) == TX_SUCCESS)
			net_core_link_react();
	}
}

/* Wake the worker from a command thread. */
static void net_core_wake(void)
{
	(void)tx_event_flags_set(&net_core_board_state.wake, NET_CORE_EVT_WAKE,
	                         TX_OR);
}

/* ---- init -------------------------------------------------------------------- */

int net_core_init(bool dhcp_created)
{
	struct net_core_state *s = &net_core_board_state;
	const struct net_core_facts *f = &net_core_board_facts;

	s->ready = 0u;
	s->dhcp_created = dhcp_created ? 1u : 0u;
	s->link_up = 0u;
	s->link_fell = 0u;
	s->link_falls = 0u;
	/* A board that boots on DHCP but could not create the client has no
	   source until someone sets a static address. */
	s->source = f->boot_source;
	if (s->source == NET_CORE_SRC_DHCP && !dhcp_created)
		s->source = NET_CORE_SRC_NONE;
	s->dhcp_running = 0u;
	s->lease_stale = 0u;
	net_core_fail_clear();

	/* Nothing below runs before the scheduler starts, and the first link
	   notice comes after it: by then all of it is in place, or ready is
	   clear and every operation says DOWN. */
	if (tx_mutex_create(&s->addr_lock, "net-addr", TX_INHERIT) != TX_SUCCESS)
		return -1;
	if (tx_event_flags_create(&s->wake, "net-link") != TX_SUCCESS)
		return -1;
	if (tx_thread_create(&s->worker, "net-link", net_core_worker_entry, 0u,
	                     f->worker_stack, f->worker_stack_size,
	                     NET_CORE_WORKER_PRIORITY, NET_CORE_WORKER_PRIORITY,
	                     TX_NO_TIME_SLICE, TX_AUTO_START) != TX_SUCCESS)
		return -1;
	if (nx_ip_link_status_change_notify_set(f->ip, net_core_link_notify)
	    != NX_SUCCESS)
		return -1;
	s->ready = 1u;              /* last: everything above is in place */
	return 0;
}

/* ---- the operations ------------------------------------------------------------ */

void net_core_addr_get(struct net_svc_addr *out, struct net_svc_result *res)
{
	struct net_core_state *s = &net_core_board_state;
	const struct net_core_facts *f = &net_core_board_facts;
	ULONG ip = 0u, mask = 0u, gw = 0u;

	if (net_core_not_ready(res))
		return;
	net_core_lock();
	(void)nx_ip_interface_address_get(f->ip, f->iface, &ip, &mask);
	if (nx_ip_gateway_address_get(f->ip, &gw) != NX_SUCCESS)
		gw = 0u;
	out->dhcp_mode = (s->source == NET_CORE_SRC_DHCP);
	out->lease_stale = (s->lease_stale != 0u);
	net_core_unlock();
	out->ip = (uint32_t)ip;
	out->mask = (uint32_t)mask;
	out->gw = (uint32_t)gw;
	out->ip_valid = (ip != 0u);
	net_core_say(res, NET_SVC_OK, NULL);
}

void net_core_set_static(uint32_t ip, uint32_t mask, uint32_t gw,
                         struct net_svc_result *res)
{
	struct net_core_state *s = &net_core_board_state;
	const struct net_core_facts *f = &net_core_board_facts;
	ULONG old_ip = 0u, old_mask = 0u, old_gw = 0u;
	uint8_t old_source, old_running;
	UINT reinit = NX_SUCCESS;

	if (net_core_not_ready(res))
		return;
	/*
	 * NetX takes a gateway only on an interface's subnet, and the address has
	 * to be set first -- so a gateway off the new subnet would fail only after
	 * the address moved.  Refuse it here, before anything changes (issue #19);
	 * the rollback below is for what cannot be predicted.
	 */
	if (gw != 0u && (gw & mask) != (ip & mask)) {
		net_core_say(res, NET_SVC_REFUSED, NULL);
		return;
	}

	net_core_lock();
	(void)nx_ip_interface_address_get(f->ip, f->iface, &old_ip, &old_mask);
	if (nx_ip_gateway_address_get(f->ip, &old_gw) != NX_SUCCESS)
		old_gw = 0u;
	old_source = s->source;
	old_running = s->dhcp_running;

	if (s->dhcp_running && net_core_dhcp_stop() != NX_SUCCESS) {
		net_core_unlock();          /* nothing changed yet */
		net_core_say(res, NET_SVC_REFUSED, NULL);
		return;
	}
	if (nx_ip_interface_address_set(f->ip, f->iface, (ULONG)ip, (ULONG)mask)
	    != NX_SUCCESS || net_core_gw_apply(gw) != NX_SUCCESS)
		goto rollback;

	s->source = NET_CORE_SRC_STATIC;
	s->lease_stale = 0u;            /* the operator said what the address is */
	net_core_fail_clear();
	net_core_unlock();
	net_core_say(res, NET_SVC_OK, NULL);
	return;

rollback:
	/*
	 * Order matters: reinitialising clears a DHCP-set address, so it runs
	 * BEFORE the old address is put back -- and it is also what a stopped
	 * client needs before it may start again.
	 */
	if (old_running)
		reinit = nx_dhcp_interface_reinitialize(f->dhcp, f->iface);
	(void)nx_ip_interface_address_set(f->ip, f->iface, old_ip, old_mask);
	(void)net_core_gw_apply((uint32_t)old_gw);   /* 0 restores "no gateway" */
	s->source = old_source;
	if (old_running) {
		if (reinit == NX_SUCCESS &&
		    nx_dhcp_interface_start(f->dhcp, f->iface) == NX_SUCCESS) {
			s->dhcp_running = 1u;
		} else {
			/* Could not put the client back: say "DHCP, not running",
			 * which `net dhcp` recovers, not a client that is not there. */
			s->dhcp_running = 0u;
			s->source = NET_CORE_SRC_DHCP;
		}
	}
	net_core_unlock();
	net_core_say(res, NET_SVC_REFUSED, NULL);
}

void net_core_dhcp_start(struct net_svc_result *res)
{
	struct net_core_state *s = &net_core_board_state;
	const struct net_core_facts *f = &net_core_board_facts;
	UINT st;

	if (net_core_not_ready(res))
		return;
	if (!s->dhcp_created) {
		net_core_say(res, NET_SVC_REFUSED, NULL);
		return;
	}

	net_core_lock();
	/*
	 * Refused with the link down, changing nothing: a client started now
	 * would sit in its retransmit back-off with nobody to restart it when the
	 * link comes back.
	 */
	if (!net_core_link_is_up()) {
		net_core_unlock();
		net_core_say(res, NET_SVC_LINK, f->why_link);
		return;
	}

	if (!s->dhcp_running) {
		st = net_core_dhcp_begin(NULL);
		if (st == NX_SUCCESS)
			net_core_fail_clear();
		net_core_unlock();
		net_core_say(res, (st == NX_SUCCESS) ? NET_SVC_OK : NET_SVC_REFUSED,
		             NULL);
		return;
	}

	/*
	 * Running: a held lease is renewed in place and never thrown away (#9) --
	 * unless it is stale.  Stale says the link went down under it and nobody
	 * has checked it since (the network may be another one now), and NetX's
	 * force renew sends a request to the old server and keeps resending it
	 * to that server on its timer, while the call itself returns success at
	 * once (nxd_dhcp_client.c), so a "renewed" stale lease would keep the old
	 * address until its rebind time with nobody answering.  The
	 * operator asked to acquire again: start from scratch.
	 */
	if (!s->lease_stale) {
		st = nx_dhcp_interface_force_renew(f->dhcp, f->iface);
		if (st == NX_SUCCESS) {
			s->source = NET_CORE_SRC_DHCP;
			net_core_fail_clear();
			net_core_unlock();
			net_core_say(res, NET_SVC_RENEWED, NULL);
			return;
		}
		if (st != NX_DHCP_NOT_BOUND) {
			net_core_unlock();
			net_core_say(res, NET_SVC_REFUSED, NULL);
			return;
		}
	}
	if (net_core_dhcp_stop() != NX_SUCCESS) {
		net_core_unlock();
		net_core_say(res, NET_SVC_REFUSED, NULL);
		return;
	}
	/* Running without a lease, or with a stale one: start from scratch. */
	st = net_core_dhcp_begin(NULL);
	if (st == NX_SUCCESS)
		net_core_fail_clear();
	net_core_unlock();
	net_core_say(res, (st == NX_SUCCESS) ? NET_SVC_OK : NET_SVC_REFUSED, NULL);
}

void net_core_ping(uint32_t ip, unsigned timeout_ms, unsigned *rtt_ms,
                   struct net_svc_result *res)
{
	const struct net_core_facts *f = &net_core_board_facts;
	NX_PACKET *resp = NX_NULL;
	ULONG t0, t1;
	UINT st;

	if (net_core_not_ready(res))
		return;
	/* Say WHY rather than letting NetX refuse every probe in turn: with the
	 * link down the interface still has an address and looks configured. */
	if (!net_core_link_is_up()) {
		net_core_say(res, NET_SVC_LINK, f->why_link);
		return;
	}
	t0 = tx_time_get();
	st = nx_icmp_ping(f->ip, (ULONG)ip, NET_CORE_PING_DATA,
	                  (ULONG)(sizeof NET_CORE_PING_DATA - 1u), &resp,
	                  (ULONG)timeout_ms);
	t1 = tx_time_get();
	if (resp != NX_NULL)
		(void)nx_packet_release(resp);
	if (st == NX_SUCCESS) {
		if (rtt_ms != NULL)
			*rtt_ms = (unsigned)(t1 - t0);
		net_core_say(res, NET_SVC_OK, NULL);
	} else if (st == NX_NO_RESPONSE) {
		net_core_say(res, NET_SVC_TIMEOUT, NULL);
	} else {
		net_core_say(res, NET_SVC_REFUSED, NULL);
	}
}

void net_core_dhcp_source(struct net_svc_result *res)
{
	struct net_core_state *s = &net_core_board_state;

	if (net_core_not_ready(res))
		return;
	if (!s->dhcp_created) {
		net_core_say(res, NET_SVC_REFUSED, NULL);
		return;
	}
	net_core_lock();
	s->source = NET_CORE_SRC_DHCP;
	net_core_unlock();
	/* After the source changed: a worker that already handled the up starts
	   the client on this pass; with the link down it starts on the up. */
	net_core_wake();
	net_core_say(res, NET_SVC_OK, NULL);
}

bool net_core_last_failure(struct net_core_failure *out)
{
	struct net_core_state *s = &net_core_board_state;
	bool have;

	if (!s->ready)
		return false;
	net_core_lock();
	*out = s->last_fail;
	net_core_unlock();
	have = (out->op != NET_CORE_OP_NONE);
	return have;
}

const char *net_core_op_name(uint8_t op)
{
	switch (op) {
	case NET_CORE_OP_STOP:   return "stop";
	case NET_CORE_OP_REINIT: return "reinitialise";
	case NET_CORE_OP_START:  return "start";
	default:                 return "?";
	}
}

bool net_core_source_is_dhcp(void)
{
	const struct net_core_state *s = &net_core_board_state;

	return s->ready && s->source == NET_CORE_SRC_DHCP;
}
