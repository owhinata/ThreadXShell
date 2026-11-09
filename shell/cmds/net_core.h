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
 * anything else.  Only threads that hold no other lock take it: the
 * commands, the link worker, and on wio-lite-ai the telnet listener and the
 * KV boot thread.
 * Nothing that runs with a NetX mutex held takes it: the link notification
 * (the IP thread, nx_ip_protection held) only masks interrupts and sets an
 * event flag.
 *
 * THE LINK.  NetX calls the shared notification (registered by
 * net_core_init()) on the IP thread whenever the driver reports a link
 * change.  It writes NetX's own interface flag, records the level and, on a
 * down, a "the link fell" mark -- the two together, in one interrupt-masked
 * section -- and then sets the worker's event flag.  The worker (one ThreadX
 * thread, NET_CORE_WORKER_PRIORITY) copies the pair and clears the mark in one
 * masked section and reacts under the address lock: a fall stops a running
 * client (the address stays) and marks a DHCP address stale; the link up with
 * DHCP the source and the client not running reinitialises, drops the
 * address and starts it.  The event flag only wakes; what happened is in the
 * pair, and a notice after the copy wakes the worker again.  Commands read
 * the level in the same kind of section and never touch the mark: handling a
 * fall is the worker's alone (issue #220).
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

/** The DHCP step a link reaction failed at (net_core_failure.op). */
enum net_core_op {
	NET_CORE_OP_NONE = 0,    /**< no failure recorded                   */
	NET_CORE_OP_STOP,        /**< nx_dhcp_interface_stop()              */
	NET_CORE_OP_REINIT,      /**< nx_dhcp_interface_reinitialize()      */
	NET_CORE_OP_START,       /**< nx_dhcp_interface_start()             */
};

/**
 * The link worker's last failure: which DHCP step, NetX's answer, and the
 * link level the worker was reacting to.  The shared file cannot write the
 * board's log, so the board prints this as one line of `net info`
 * (net_core_last_failure()).  Cleared by a later success of the DHCP
 * operations -- a worker reaction whose steps all succeeded, or a successful
 * `net dhcp` / `net ip`.
 */
struct net_core_failure {
	uint8_t op;              /**< enum net_core_op; NONE = nothing failed */
	uint8_t link_up;         /**< the recorded level the reaction saw     */
	UINT    status;          /**< NetX's return value                     */
};

/** The worker's ThreadX priority: above the CLI (CLI_INSTANCE_PRIORITY 16),
 *  below both boards' IP threads (11, 12) and the DHCP thread (13 on both,
 *  NX_DHCP_THREAD_PRIORITY in board.cmake), so the
 *  notification never hands it the CPU while the IP thread holds its mutex. */
#define NET_CORE_WORKER_PRIORITY 15u

/**
 * The board's mutable state for net_core.c.  The board defines it without an
 * initializer (zero is "not initialised"); net_core_init() fills it.
 *
 * Under the address lock: source, dhcp_running, lease_stale, last_fail.
 * Under a ThreadX interrupt-masked section (TX_DISABLE / TX_RESTORE),
 * everywhere: link_up and link_fell, as one pair.  Written once by
 * net_core_init() before the scheduler starts: ready, dhcp_created.  The
 * ThreadX objects are created by net_core_init().
 */
struct net_core_state {
	TX_MUTEX addr_lock;
	/** Wakes the link worker; carries nothing (the pair below does). */
	TX_EVENT_FLAGS_GROUP wake;
	/** The link worker (its stack is the board's, in the facts). */
	TX_THREAD worker;
	/** net_core_init() created everything; until then every operation is
	 *  NET_SVC_DOWN with the board's words. */
	uint8_t ready;
	/** The board's nx_dhcp_create() succeeded (passed to net_core_init()).
	 *  A runtime fact, so it cannot be in the const facts. */
	uint8_t dhcp_created;
	/** The link level as last notified. */
	volatile uint8_t link_up;
	/** A down was notified since the worker last copied the pair.  Set by the
	 *  notification only, cleared by the worker only. */
	volatile uint8_t link_fell;
	/** enum net_core_source. */
	uint8_t source;
	/** The client is started as far as this file knows: set only after a
	 *  start NetX accepted, cleared only after a stop NetX accepted (or
	 *  answered "was not started"). */
	uint8_t dhcp_running;
	/** The link went down under a DHCP address and nothing has renewed it. */
	uint8_t lease_stale;
	/** The link worker's last failure (struct net_core_failure). */
	struct net_core_failure last_fail;
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
	/** The link worker's stack, placed by the board (CPU-only memory). */
	void    *worker_stack;
	ULONG    worker_stack_size;
};

/** The board's two objects (f746g-disco port/netxduo/nx_glue.c, wio-lite-ai
 *  src/nx_net.c). */
extern struct net_core_state       net_core_board_state;
extern const struct net_core_facts net_core_board_facts;

/**
 * Fill the state, create the address lock, the worker's event flags and the
 * worker thread, and register the shared link notification with NetX; the
 * ready mark is set last, only when everything succeeded.  From
 * tx_application_define(), right after the board created its NetX objects
 * (before the scheduler starts, so before the first link notice): it only
 * creates ThreadX objects and registers a callback, never waits and does not
 * depend on HAL_GetTick.  The board registers no link-status callback of its
 * own.  @p dhcp_created is the board's nx_dhcp_create() result.
 * @return 0, or -1 (the mark stays clear).
 */
int net_core_init(bool dhcp_created);

/**
 * One pass of the link worker: copy the level and the fall mark and clear the
 * mark (one masked section), then react under the address lock from the
 * source and the client's state of that moment.  The worker thread runs this
 * each time it is woken and does nothing else; the host test calls it
 * directly.  Never retries a failed step: recovery waits for the next notice
 * or the operator's `net dhcp` / `net ip`.
 */
void net_core_link_react(void);

/**
 * The link worker's last failure, read under the address lock, for the
 * board's line of `net info`.  @return true and @p out filled when one is
 * recorded, false when there is none (or before init).
 */
bool net_core_last_failure(struct net_core_failure *out);

/** NetX's name-free words for a net_core_op ("stop", "reinitialise",
 *  "start"), for the board's line. */
const char *net_core_op_name(uint8_t op);

/**
 * DHCP is the address source (a hint for a board's message, read without the
 * lock: one byte, and the answer may be stale by the time it is printed).
 */
bool net_core_source_is_dhcp(void);

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
 * "DHCP, not running", which a later `net dhcp` or link up recovers.  A
 * success clears the stale mark and the worker's last failure.  Allowed with
 * the link down.
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
 * failed step leaves the state where the steps that succeeded put it.  A
 * success clears the worker's last failure.
 */
void net_core_dhcp_start(struct net_svc_result *res);

/** `net ping`: the link down is NET_SVC_LINK; otherwise one nx_icmp_ping().
 *  Takes no lock (a probe waits up to a second). */
void net_core_ping(uint32_t ip, unsigned timeout_ms, unsigned *rtt_ms,
                   struct net_svc_result *res);

/**
 * wio-lite-ai's KV boot `net.mode=dhcp`: make DHCP the source and wake the
 * worker, which starts the client when the recorded link is up -- now, if the
 * up was already handled, or when it comes.  Unlike `net dhcp` it is not
 * refused with the link down: right after the bridge comes up the recorded
 * level may not have caught up yet, and the order must not rest on the IP
 * thread's priority.  No DHCP client is NET_SVC_REFUSED.
 */
void net_core_dhcp_source(struct net_svc_result *res);

#ifdef __cplusplus
}
#endif

#endif /* NET_CORE_H */
