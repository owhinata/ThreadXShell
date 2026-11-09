/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/*
 * Wio Lite AI (STM32H725AEI6) -- the host's own IP interface (owhinata/wio-lite-ai#23 U3).
 *
 * Two things live here, because they are the same thing seen from two sides:
 *
 *   1. The NetX objects the shared address / DHCP / ping operations run on
 *      (shell/cmds/net_core.c, issue #220): the IP instance, its pool and the DHCP
 *      client, created here, and the state those operations keep, defined here.
 *   2. The resident owner of the bridge session: the thread that holds the RTL8720 link
 *      reference, turns the module into an L2 relay, keeps its watchdog fed, and takes
 *      the whole thing down again provably.
 *
 * ---- what "up" costs, and why there is an owner at all -------------------------
 *
 * The module's bridge is armed with a deadline (owhinata/wio-lite-ai#23 U2, firmware
 * wio-n7): it takes its own tap out `ms` after the last DATA_CFG, so a host that stops
 * asking cannot leave it forwarding into a link nobody reads.  The cap is 60 s, so a
 * RESIDENT interface has to keep asking -- that is the refresh loop.  And every re-arm
 * resets the module's DATA counters, so the refresh reads them first and accumulates: "was
 * anything lost, and on which side" is the question this whole issue has been decided by,
 * and a session that zeroed its own evidence every 8 s could not answer it.
 *
 * While the tap is in, the module's own lwIP receives nothing, so anything that runs on
 * that lwIP is refused while the host stack is up -- see nx_net_guard().
 * owhinata/wio-lite-ai#23 U4 moved the two things that wanted a TCP connection off it
 * entirely: `net echo` and the telnet console opened NetX sockets on THIS interface, and
 * therefore REQUIRED the host stack rather than being refused by it.  `net echo` was a
 * bring-up test server and was removed by issue #216; the telnet console is the one left.
 * What is left on the module's lwIP -- `net info`, `net ip`, `net dhcp` -- keeps its eRPC
 * backend, which is also what keeps a regression test against an untouched module
 * firmware alive.
 *
 * ---- the failure rule ----------------------------------------------------------
 *
 * ONE rule governs every unwind, and it comes from app/link_data.h's detach ordering:
 * detaching re-arms the link service thread's stale-byte flush, and if the module can
 * still emit a DATA frame, those "stale" bytes are the middle of one.  So:
 *
 *      once DATA_CFG(BRIDGE) has been issued, only nxn_stop() may end the session,
 *      whatever the answer was.
 *
 * A success means the tap is in.  Module status 5 means "the tcpip thread did not answer;
 * the tap may have gone in after all and the watchdog will take it out" -- which is not
 * "nothing happened".  A -2 timeout means we know nothing at all.  Statuses 1..4 really
 * do prove no callback was installed, but branching on them buys one CTRL exchange and
 * costs a claim that stops being true the next time the firmware changes.
 *
 * Silence has two sides, and the teardown (nxn_stop()) proves them differently:
 *
 *   - THIS side, by the driver's gate (issue #218).  Its first step is
 *     nx_link_driver_set_link(0), which closes the gate and fences the DATA channel's send
 *     generation in one masked step.  After it there is no path by which a NetX transmit
 *     -- from the IP thread, DHCP, the telnet console, or a CLI `net ping`, preempted or
 *     not -- is queued on the DATA channel; frames queued before it are drained by the
 *     settle, and only the owner thread reopens the gate, never while STOPPING.  So no
 *     producer has to be stopped, or proved stopped, first: the telnet console is
 *     resident and is not stopped at all (issue #221), and it cannot make the teardown
 *     FAIL.
 *   - The MODULE's side, by DATA_CFG(off) being acknowledged and the settle then finding
 *     both sides idle (erpc_data_quiescent() on ours, which after the gate is a stable
 *     fact rather than a snapshot; the module's queue and pool counters on its own).
 *
 * If the teardown cannot prove silence -- the link could not be claimed, DATA_CFG(off) was
 * never acknowledged, or the settle did not see both sides idle -- the state becomes
 * FAILED: force-quiesce (which app/link_data.h names as one of the teardowns that take the
 * module down with them, so detaching after it is safe), then say `wifi reset`.  The
 * claim failure is the one exception that cannot even force-quiesce, and leaves the tap
 * to the module's watchdog.
 */
#ifndef APP_NX_NET_H
#define APP_NX_NET_H

#include <stdbool.h>
#include <stdint.h>

#include "net_svc.h"   /* net_svc_write_fn */

struct cli_instance;

/*
 * ---- the transmit budget (owhinata/wio-lite-ai#23 U4) -----------------------------------
 *
 * Every frame this stack transmits ends up in the link's DATA transmit pool
 * (LINK_DATA_TX_BUFS, app/link_data.h), and a full pool there is a silent drop -- correct
 * for Ethernet, but a TCP retransmit timeout for anything running a stream on top.  So the
 * pool has to be at least as deep as the most NetX can hand the driver before the link
 * service thread has drained any of it:
 *
 *   LINK_DATA_TX_BUFS >= NXN_TCP_SOCKETS_MAX * NXN_TCP_TX_DEPTH
 *                        + NX_ARP_MAX_QUEUE_DEPTH
 *                        + NXN_TX_SPARE
 *
 * The ARP term is the one that is easy to miss: NetX queues packets per ARP entry while an
 * address is being resolved and flushes the whole list to the driver at once when the reply
 * arrives (_nx_arp_queue_send()), so it is a BURST, not a steady-state occupancy.
 * NXN_TX_SPARE covers the single frames -- an ICMP echo, a DHCP renewal, an ARP request or
 * response -- that can coincide with such a flush.
 *
 * This is an upper-bound ESTIMATE, not a proof: how many packets NetX can pass down in one
 * go depends on the configuration and the path.  It is checked on hardware instead --
 * `net info` reports `nx tx no-buf`, which must stay at zero.  It is deliberately NOT
 * claimed to survive a hostile ICMP flood or arbitrary IP fragmentation; those are
 * legitimate Ethernet drops.
 *
 * The _Static_assert that ties these to LINK_DATA_TX_BUFS lives in app/nx_link_driver.c,
 * the translation unit that owns the transmit path and already includes the NetX headers
 * for NX_ARP_MAX_QUEUE_DEPTH -- putting it in app/link_data.h would make the DATA channel
 * depend on NetX headers.
 */
#define NXN_TCP_TX_DEPTH      4u   /* nx_tcp_socket_transmit_configure max_queue_depth  */
#define NXN_TCP_SOCKETS_MAX   1u   /* the telnet console is the only TCP socket         */
#define NXN_TX_SPARE          4u   /* room for single ICMP / DHCP / ARP frames          */

/*
 * Initial TCP retransmit timeout, in ThreadX ticks (= ms, NX_IP_PERIODIC_RATE is 1000),
 * and the same value the f746 port uses.
 *
 * A shorter one is tempting -- this link's round trip is 1-5 ms, so 2 s to notice a lost
 * segment is a long time to stare at a console.  U4-1 tried 200 ms and it is NOT a free
 * choice: nx_tcp_socket_transmit_configure() writes nx_tcp_socket_timeout_rate, which NetX
 * also uses as the SYN+ACK retransmit timer during a passive open
 * (nx_tcp_packet_process.c:765-770), so it changes how a handshake behaves and not just
 * how a stall recovers.  Tuning it is therefore an experiment of its own with its own
 * evidence -- owhinata/wio-lite-ai#23 U4-3, against nx_tcp_socket_info_get()'s retransmit
 * counter -- and not something to carry into a first bring-up.
 */
#define NXN_TCP_RTO_MS        (2u * 1000u)
#define NXN_TCP_RTO_RETRIES   10u
#define NXN_TCP_RTO_SHIFT     1u

/* Return codes of nx_net_init(), mirroring the f746 nx_glue.h. */
#define NXN_OK          0
#define NXN_ERR        (-2)      /* a ThreadX / NetX create failed                 */

enum nx_net_state {
	NX_NET_OFF = 0,          /* the module's own lwIP owns the network          */
	NX_NET_ARMING,           /* bringing the bridge in                          */
	NX_NET_UP,               /* the host stack owns the network                 */
	NX_NET_STOPPING,
	NX_NET_FAILED            /* teardown could not prove silence -- `wifi reset` */
};

/* Module-side counters accumulated across bridge re-arms (each re-arm zeroes the
 * module's own), in the order LINK_DATA_STATS reports them. */
struct nx_net_modstats {
	uint32_t rx_frames, rx_bytes, rx_drops, rx_crc, rx_oversize, rx_gaps;
	uint32_t tx_frames, tx_bytes, tx_drops;
};

/*
 * Create every ThreadX/NetX object and the owner thread.  Object creation only -- no
 * blocking and no HAL_GetTick dependency -- so it is safe from tx_application_define()
 * (owhinata/wio-lite-ai#12).  The IP instance exists from boot with address 0.0.0.0, no MAC
 * and the link down; nothing touches the RTL8720 until nx_net_up().
 */
int  nx_net_init(void);

/* Is the host stack the one that owns the network right now? */
bool nx_net_is_up(void);
enum nx_net_state nx_net_state(void);

/* "off" / "arming" / "up" / "stopping" / "FAILED" -- so a refusal can name the state it
 * was refused in rather than just saying no. */
const char *nx_net_state_name(enum nx_net_state s);

/* Ask the owner thread to bring the interface up / take it down.  nx_net_up() returns 0
 * once the request is accepted (state becomes ARMING before it returns, so guards close
 * immediately); *why explains a refusal.  Poll nx_net_state() for the outcome. */
int  nx_net_up(const char **why);
void nx_net_down(void);

/*
 * "The link has been taken away and CHIP_EN is about to move" -- the epilogue of
 * rtl_link_force_quiesce() for `wifi on/off/reset` (owhinata/wio-lite-ai#41).
 *
 * The owner detects a revoked link by itself (the uart generation moved), but only when
 * it next wakes, which is up to NXN_REFRESH_MS later.  This hands it the news now and
 * waits, briefly, for it to leave NX_NET_UP -- so the very next command does not find a
 * stale UP and try to renew a bridge on a module that has just been power-cycled.
 *
 * NOT nx_net_down(): that is the graceful stop, which needs the coarse mutex and speaks
 * eRPC to the module, neither of which a recovery command can afford.  See the body.
 *
 * Call AFTER rtl_link_force_quiesce() and BEFORE moving CHIP_EN.  Returns 0 once the
 * interface is no longer up (already-down counts), -1 if it has not stood down in time --
 * which the caller reports and then carries on with, rather than refusing.
 */
int  nx_net_link_taken(void);

/* The MAC the interface transmits with (the module's), valid once up. */
void nx_net_mac_get(uint8_t mac[6]);
void nx_net_modstats_get(struct nx_net_modstats *out);

/*
 * The NetX objects, for the things that put a socket on this interface.  (The telnet
 * console names them in its facts instead, in nx_net.c, issue #221.)
 * Typed as void * so that a caller which only needs to pass them along does not have to
 * include nx_api.h; cast to NX_IP * / NX_PACKET_POOL *.
 *
 * NULL until nx_net_init() has succeeded.  Both are valid from then on regardless of the
 * session state -- the IP instance exists from boot with address 0.0.0.0 and the link
 * down -- so a caller still has to ask nx_net_is_up() before expecting traffic to move.
 *
 * (The f746 nx_glue.h has the same pair.  U3 deliberately did not port them because
 * nothing on this board created a socket yet; U4 does.)
 */
void *nx_net_ip(void);
void *nx_net_pool(void);

/*
 * Write the host-stack section of `net info` / the result of arming the interface, one
 * whole line per call of @p write (svc/net/net_svc.h, issue #219).  Every value is
 * sampled first and no lock is held while writing; a failed write ends it.  Returns 0,
 * or -1 after the write that failed (the lines before it stay written).
 */
int nx_net_write_status(net_svc_write_fn write, void *ctx);

/*
 * Refuse a command that would reconfigure the module underneath the interface.
 * Returns 0 when it may proceed, non-zero after printing why.
 *
 * It refuses in EVERY state except OFF, not just UP.  ARM releases the coarse mutex
 * before it declares itself up, and rtl_link_uart_gen() does not advance for ordinary
 * eRPC flows (only a 0->1 open and a force-quiesce), so the owner's generation watch
 * cannot see a `wifi connect` land in that window.  nx_net_up() therefore publishes
 * ARMING on the caller's thread, before the owner is even signalled.
 *
 * `wifi on` / `off` / `reset` must NOT use this: they are the recovery path, they
 * force-quiesce, and the generation watch turns the interface down cleanly afterwards.
 * Refusing them would take away the only way out of FAILED.
 */
int nx_net_guard(struct cli_instance *sh, const char *what);

/*
 * Keep the module's bridge alive across a long eRPC flow (owhinata/wio-lite-ai#30 B2b).
 * Caller must hold the rtl_link coarse mutex.  Returns 0 (nothing to hold, or held) / -1
 * (the module did not answer -- do NOT start the flow).  See the definition for why.
 */
int nx_net_hold_extend(void);

#endif /* APP_NX_NET_H */
