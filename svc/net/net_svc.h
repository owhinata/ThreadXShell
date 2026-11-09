/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    net_svc.h
 * @brief   The contract of the shared `net` command (issue #219, epic #212).
 *
 * shell/cmds/cmd_net.c holds the grammar, the waits, the ping loop and its
 * totals and every line `net` prints; a board puts its IP stack behind the
 * calls below (f746g-disco port/netxduo/net_svc_f746.c, wio-lite-ai
 * src/net_svc_wio.c) and says what it has in its svc/net_svc_config.h.
 *
 * [!] AN ADAPTER TAKES NO `struct cli_instance`.  Printing, waiting and Ctrl+C
 * are the shared command's.  Each call is an operation that returns its own
 * result; none is a predicate about the board's state, and nothing here calls
 * up into the shell.
 *
 * [!] THE RESULTS ARE NOT FOLDED.  Each status says something different about
 * what to do next, so the command words them apart (svc/README.md).  A glue
 * value an adapter does not know is read as NET_SVC_REFUSED: every failure
 * exits 1, so that is the safe side.
 *
 * Addresses are host byte order (a.b.c.d -> (a<<24)|(b<<16)|(c<<8)|d).
 * Freestanding: no HAL, no board header, no tx_api.h.
 */
#ifndef NET_SVC_H
#define NET_SVC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** What one call did. */
enum net_svc_status {
	/** Done. */
	NET_SVC_OK = 0,
	/** `net dhcp` only: a lease was held, it is kept and a renew was sent.
	 *  A success. */
	NET_SVC_RENEWED,
	/** The IP stack does not have the network (not up, or taken away). */
	NET_SVC_DOWN,
	/** The IP stack is coming up or going down; waiting makes it go.
	 *  [!] RETURNED ONLY FROM THE ADAPTER'S OWN CHECK OF THE STATE, BEFORE
	 *  ANY GLUE CALL: BUSY GUARANTEES THAT NOTHING CHANGED, which is what
	 *  lets the shared command call the same operation again. */
	NET_SVC_BUSY,
	/** The IP stack is in a state only a reset of the link leaves. */
	NET_SVC_FAILED,
	/** The link is down. */
	NET_SVC_LINK,
	/** NetX refused.  A refused `net ip` puts back the address, the gateway
	 *  and the source (issue #220); a DHCP client it had stopped is started
	 *  again from scratch when it can be (the lease itself is not restored)
	 *  and left "DHCP, not running" when it cannot.  A failure while putting
	 *  things back is not reported.  [!] A REFUSED `net dhcp` PUTS NOTHING
	 *  BACK: a step that failed part way can leave the address dropped and
	 *  the client stopped, so nothing may say "restored". */
	NET_SVC_REFUSED,
	/** `net ping` only: no reply within the probe's wait. */
	NET_SVC_TIMEOUT,
};

/**
 * The result of one call.  The caller owns it (on its stack); the adapter
 * fills both fields before returning.
 */
struct net_svc_result {
	enum net_svc_status status;
	/** Why, in the board's words, for NET_SVC_DOWN / _BUSY / _FAILED /
	 *  _LINK; NULL otherwise.  A pointer to a string LITERAL, so it outlives
	 *  the call and needs neither a copy nor any storage. */
	const char *why;
};

/**
 * The address of the interface, from one call.
 *
 * [!] ONE CALL, NOT ONE MOMENT.  The contract promises that every field comes
 * from the same call, not that they were all read at the same instant.  Every
 * board reads them in one hold of its address lock (shell/cmds/net_core.c),
 * but the DHCP client writes the address and the gateway of a new lease in
 * two separate NetX calls under its own mutex, which is not taken here: for a
 * few microseconds a reader can pair the new address with the old gateway
 * (issue #220, kept as the contract rather than reaching into NetX's
 * internals).
 */
struct net_svc_addr {
	bool     ip_valid;     /**< a non-zero address is assigned               */
	bool     dhcp_mode;    /**< the DHCP client owns the address (vs static) */
	/** The link went down while this DHCP address was live and nothing
	 *  renewed it -- set where the board's link layer records the link going
	 *  down (issue #220); `net ip` and `net dhcp` clear it. */
	bool     lease_stale;
	uint32_t ip, mask, gw; /**< gw 0 is "no gateway", not an address          */
};

/**
 * The sink the board's lines of `net info` are written to, one whole line per
 * call (the "\r\n" included).  Length-bearing, so no formatter crosses the
 * boundary, and failing.
 *
 * [!] AN ADAPTER MUST NOT STORE @p write, AND MUST NOT CALL IT WITH A LOCK
 * HELD OR INTERRUPTS MASKED: it samples every value into its own stack frame
 * first, releases everything, and then writes.  On a failed write it stops
 * writing and returns the failure; lines already written stay written.
 *
 * @return the bytes taken, or negative on failure.
 */
typedef int (*net_svc_write_fn)(void *ctx, const char *s, size_t len);

/** The interface's address (`net info`, and the lines after `net ip` /
 *  `net dhcp`).  @p out is valid only with NET_SVC_OK. */
void net_svc_addr_get(struct net_svc_addr *out, struct net_svc_result *res);

/** `net ip`: a static address, @p gw 0 for none (stops DHCP). */
void net_svc_set_static(uint32_t ip, uint32_t mask, uint32_t gw,
                        struct net_svc_result *res);

/** `net dhcp`: (re)start the DHCP client.  NET_SVC_OK when an acquisition
 *  was started (the address is dropped first, so an address that appears
 *  afterwards is the lease), NET_SVC_RENEWED when a held lease is renewed in
 *  place, NET_SVC_LINK (nothing changed) with the link down.  The wait for
 *  the lease is the shared command's. */
void net_svc_dhcp_start(struct net_svc_result *res);

/** `net ping`: one ICMP echo to @p ip, waiting up to @p timeout_ms.  With
 *  NET_SVC_OK, @p *rtt_ms is the round trip. */
void net_svc_ping(uint32_t ip, unsigned timeout_ms, unsigned *rtt_ms,
                  struct net_svc_result *res);

/** `net info`: write the board's own lines (its counters) after the address.
 *  @return 0, or the negative result of the write that failed. */
int net_svc_board_lines(net_svc_write_fn write, void *ctx);

/**
 * The operator's `net ip` or `net dhcp` has just given the interface an
 * address.  wio-lite-ai arms its telnet console here; f746g-disco does
 * nothing.  [!] TEMPORARY: issue #221 makes the console resident on both
 * boards and removes this call.
 */
void net_svc_addressed(void);

#ifdef __cplusplus
}
#endif

#endif /* NET_SVC_H */
