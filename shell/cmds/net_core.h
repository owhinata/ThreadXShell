/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    net_core.h
 * @brief   The one implementation of the address, DHCP and ping operations
 *          over NetX Duo, for every board with a NetX IP stack (issue #220,
 *          epic #212).
 *
 * shell/cmds/net_core.c holds the operations; a board's adapter
 * (f746g-disco port/netxduo/net_svc_f746.c, wio-lite-ai src/net_svc_wio.c)
 * checks only its own state (is the IP stack there, is it changing) and then
 * calls them.  The NetX objects, the driver and the link layer stay the
 * board's.  svc/net/net_svc.h stays freestanding: the NetX types live here.
 *
 * [!] net_core.c OWNS NO STORAGE (cmake/check_no_mutable_storage.py, per
 * board).  The board DEFINES the two objects below under their fixed names --
 * its mutable state and its constant facts -- and net_core.c reaches them
 * through these extern declarations, resolved at link time (the same shape as
 * svc/nn/nn_dispatch_state.h).  Nothing is registered at init: a pointer the
 * shared file remembered would itself be mutable storage in it, and NetX's
 * link-status callback takes no context argument to carry one.
 *
 * [!] LOCK ORDER: the address lock (net_core_state.addr_lock) -> the DHCP
 * client's mutex (taken by NetX inside nx_dhcp_*) -> nx_ip_protection -> the
 * driver's.  net_core.c takes only the address lock, and never while it holds
 * anything else.  Nothing that runs with a NetX mutex held (the IP thread's
 * link-status callback) takes it: net_core_link_note() only masks interrupts.
 */
#ifndef NET_CORE_H
#define NET_CORE_H

#include <stdbool.h>
#include <stdint.h>

#include "tx_api.h"
#include "nx_api.h"
#include "nxd_dhcp_client.h"

#include "net_svc.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Who owns the interface's address. */
enum net_core_source {
	NET_CORE_SRC_NONE = 0,   /**< nobody has set one                    */
	NET_CORE_SRC_DHCP,       /**< the DHCP client (running or not)      */
	NET_CORE_SRC_STATIC,     /**< the operator's `net ip` (or KV)       */
};

/**
 * The board's mutable state for net_core.c.  The board defines it without an
 * initializer (zero is "not initialised"); net_core_init() fills it.
 *
 * Under the address lock: source, dhcp_running, lease_stale.  Under a ThreadX
 * interrupt-masked section (TX_DISABLE / TX_RESTORE), everywhere: link_up.
 * Written once by net_core_init() before the scheduler starts: ready,
 * dhcp_created.
 */
struct net_core_state {
	TX_MUTEX addr_lock;
	/** net_core_init() created everything; until then every operation is
	 *  NET_SVC_DOWN with the board's words. */
	uint8_t ready;
	/** The board's nx_dhcp_create() succeeded (passed to net_core_init()).
	 *  A runtime fact, so it cannot be in the const facts. */
	uint8_t dhcp_created;
	/** The link level as last recorded (net_core_link_note()). */
	volatile uint8_t link_up;
	/** enum net_core_source. */
	uint8_t source;
	/** The client is started as far as this file knows: set only after a
	 *  start NetX accepted, cleared only after a stop NetX accepted (or
	 *  answered "was not started"). */
	uint8_t dhcp_running;
	/** The link went down under a DHCP address and nothing has renewed it. */
	uint8_t lease_stale;
};

/** The board's constant facts for net_core.c. */
struct net_core_facts {
	NX_IP   *ip;
	NX_DHCP *dhcp;
	UINT     iface;        /**< the one interface every call names       */
	uint8_t  boot_source;  /**< enum net_core_source at init (with DHCP)  */
	/** The board's words for NET_SVC_LINK and for NET_SVC_DOWN before
	 *  net_core_init() succeeded.  String literals. */
	const char *why_link;
	const char *why_down;
};

/** The board's two objects (f746g-disco port/netxduo/nx_glue.c, wio-lite-ai
 *  src/nx_net.c). */
extern struct net_core_state       net_core_board_state;
extern const struct net_core_facts net_core_board_facts;

/**
 * Create the address lock and fill the state; the ready mark is set last,
 * only when everything succeeded.  From tx_application_define(), right after
 * the board created its NetX objects: it only creates a ThreadX object, never
 * waits and does not depend on HAL_GetTick.  @p dhcp_created is the board's
 * nx_dhcp_create() result.  @return 0, or -1 (the mark stays clear).
 */
int net_core_init(bool dhcp_created);

/* ---- the operations of svc/net/net_svc.h --------------------------------- */

/**
 * The address, from ONE hold of the address lock: the source, the DHCP state
 * and the stale mark are that lock's, and ip / mask / gw are read through
 * NetX's public calls inside the same hold.
 *
 * [!] ONE CALL, NOT ONE MOMENT.  The DHCP client's mutex is NOT taken: when
 * a lease arrives, the DHCP thread writes the address and the gateway in two
 * separate NetX calls, so for a few microseconds this can pair the new
 * address with the old gateway.  Closing that would mean taking NetX's
 * internal member nx_dhcp_mutex, which no public call exposes; the contract
 * keeps "one call" instead (issue #220, decided with the user).
 */
void net_core_addr_get(struct net_svc_addr *out, struct net_svc_result *res);

/**
 * `net ip`.  A gateway off the new subnet is refused before anything is
 * touched.  Otherwise: stop DHCP, set the address, set (or clear, gw 0) the
 * gateway.  On a failure everything is put back -- the address, the gateway,
 * the source and the DHCP client; a client that cannot be restarted is left
 * "DHCP, not running", which a later `net dhcp` recovers.  A success clears
 * the stale mark.  Allowed with the link down.
 */
void net_core_set_static(uint32_t ip, uint32_t mask, uint32_t gw,
                         struct net_svc_result *res);

/**
 * `net dhcp`.  The link down is NET_SVC_LINK and changes nothing; no DHCP
 * client is NET_SVC_REFUSED.  Otherwise one of three: not running -> drop
 * the address and start (NET_SVC_OK); a lease held and not stale -> renew
 * only (NET_SVC_RENEWED); running without a lease, or with a stale one ->
 * stop, drop the address and start again (NET_SVC_OK).  A stale lease is not
 * renewed: NetX's force renew only sends one request to the old server, so
 * after the link went down it would answer RENEWED for an address nobody
 * confirms.  Every start is preceded by a reinitialise (NetX:
 * a stopped client must be reinitialised before it starts).  The source
 * becomes DHCP and the stale mark clears once the address is dropped; a
 * failed step leaves the state where the steps that succeeded put it.
 */
void net_core_dhcp_start(struct net_svc_result *res);

/** `net ping`: the link down is NET_SVC_LINK; otherwise one nx_icmp_ping().
 *  Takes no lock (a probe waits up to a second). */
void net_core_ping(uint32_t ip, unsigned timeout_ms, unsigned *rtt_ms,
                   struct net_svc_result *res);

/**
 * wio-lite-ai's KV boot `net.mode=dhcp`: make DHCP the source.  Unlike
 * `net dhcp` it is not refused with the link down.
 *
 * [!] TEMPORARY SHAPE (step 1 of issue #220): with no link worker yet, it
 * also starts the client when it is not running, without looking at the
 * recorded link -- what the KV path did before.  Step 2 turns it into "set
 * the source and wake the worker", which starts it when the link is up.
 */
void net_core_dhcp_source(struct net_svc_result *res);

/* ---- the link, until the worker (issue #220 step 2) ----------------------- */

/**
 * Record the link level.  From the board's NetX link-status callback (the IP
 * thread, nx_ip_protection held): one interrupt-masked store, no NetX call,
 * no lock, no wait.  [!] TEMPORARY: step 2 replaces the board's callback
 * with the shared notification.
 */
void net_core_link_note(bool up);

/**
 * [!] TEMPORARY entry (step 1 of issue #220), f746g-disco's link poll hook
 * after a link-up: if the recorded link is up, the source is DHCP and the
 * client is not running, reinitialise, drop the address and start it.
 * @return NX_SUCCESS (also when there was nothing to do), or the NetX status
 * of the step that failed, for the board's log.
 */
UINT net_core_link_up_start(void);

/**
 * [!] TEMPORARY entry (step 1 of issue #220), wio-lite-ai's resident thread
 * after it closed the link: record the link down; with @p stop_dhcp stop a
 * running client (recorded as stopped only when NetX stopped it); and mark
 * the lease stale when the source is DHCP and there is an address.
 * @return NX_SUCCESS, or the status of a stop that failed.
 */
UINT net_core_link_lost(bool stop_dhcp);

#ifdef __cplusplus
}
#endif

#endif /* NET_CORE_H */
