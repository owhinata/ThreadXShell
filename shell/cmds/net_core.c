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
 * NX_SUCCESS.
 */
static UINT net_core_dhcp_begin(void)
{
	struct net_core_state *s = &net_core_board_state;
	const struct net_core_facts *f = &net_core_board_facts;
	UINT st;

	st = nx_dhcp_interface_reinitialize(f->dhcp, f->iface);
	if (st != NX_SUCCESS)
		return st;
	net_core_drop_address();
	s->source = NET_CORE_SRC_DHCP;
	s->lease_stale = 0u;
	st = nx_dhcp_interface_start(f->dhcp, f->iface);
	if (st != NX_SUCCESS)
		return st;
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

/* ---- init -------------------------------------------------------------------- */

int net_core_init(bool dhcp_created)
{
	struct net_core_state *s = &net_core_board_state;

	s->ready = 0u;
	if (tx_mutex_create(&s->addr_lock, "net-addr", TX_INHERIT) != TX_SUCCESS)
		return -1;
	s->dhcp_created = dhcp_created ? 1u : 0u;
	s->link_up = 0u;
	/* A board that boots on DHCP but could not create the client has no
	   source until someone sets a static address. */
	s->source = net_core_board_facts.boot_source;
	if (s->source == NET_CORE_SRC_DHCP && !dhcp_created)
		s->source = NET_CORE_SRC_NONE;
	s->dhcp_running = 0u;
	s->lease_stale = 0u;
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
		st = net_core_dhcp_begin();
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
	st = net_core_dhcp_begin();
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
	UINT st = NX_SUCCESS;

	if (net_core_not_ready(res))
		return;
	if (!s->dhcp_created) {
		net_core_say(res, NET_SVC_REFUSED, NULL);
		return;
	}
	net_core_lock();
	s->source = NET_CORE_SRC_DHCP;
	if (!s->dhcp_running)
		st = net_core_dhcp_begin();   /* [!] step 1 only: see net_core.h */
	net_core_unlock();
	net_core_say(res, (st == NX_SUCCESS) ? NET_SVC_OK : NET_SVC_REFUSED, NULL);
}

/* ---- the link, until the worker -------------------------------------------------- */

void net_core_link_note(bool up)
{
	TX_INTERRUPT_SAVE_AREA

	TX_DISABLE
	net_core_board_state.link_up = up ? 1u : 0u;
	TX_RESTORE
}

UINT net_core_link_up_start(void)
{
	struct net_core_state *s = &net_core_board_state;
	UINT st = NX_SUCCESS;

	if (!s->ready || !s->dhcp_created)
		return NX_SUCCESS;
	net_core_lock();
	/* Decided under the lock from the state of that moment: a `net ip` that
	 * got here first made the source static, and nothing starts. */
	if (net_core_link_is_up() && s->source == NET_CORE_SRC_DHCP &&
	    !s->dhcp_running)
		st = net_core_dhcp_begin();
	net_core_unlock();
	return st;
}

UINT net_core_link_lost(bool stop_dhcp)
{
	struct net_core_state *s = &net_core_board_state;
	const struct net_core_facts *f = &net_core_board_facts;
	ULONG ip = 0u, mask = 0u;
	UINT st = NX_SUCCESS;

	if (!s->ready)
		return NX_SUCCESS;
	net_core_lock();
	/* Recorded here too, under the lock, so a `net dhcp` that takes the lock
	 * after this sees the link down whether or not the IP thread has run the
	 * link-status callback yet. */
	net_core_link_note(false);
	if (stop_dhcp && s->dhcp_running)
		st = net_core_dhcp_stop();
	(void)nx_ip_interface_address_get(f->ip, f->iface, &ip, &mask);
	if (s->source == NET_CORE_SRC_DHCP && ip != 0u)
		s->lease_stale = 1u;
	net_core_unlock();
	return st;
}
