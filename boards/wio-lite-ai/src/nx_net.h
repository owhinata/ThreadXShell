/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/*
 * Wio Lite AI (STM32H725AEI6) -- the host's own IP interface (owhinata/wio-lite-ai#23 U3).
 *
 * The NetX objects the shared address / DHCP / ping operations and the telnet console
 * run on (shell/cmds/net_core.c, issue #220; shell/cmds/net_tty.c, issue #221): the IP
 * instance, its pool and the DHCP client, created here, and the state those operations
 * keep, defined here.  Plus the host-stack section of `net info`.
 *
 * The resident owner of the bridge session underneath -- the thread that holds the
 * RTL8720 link reference, turns the module into an L2 relay, keeps its watchdog fed,
 * and takes the whole thing down again provably -- is app/link_bridge.h (it lived in
 * this file until issue #222).  This file calls down into it, never the other way.
 */
#ifndef APP_NX_NET_H
#define APP_NX_NET_H

#include <stdbool.h>
#include <stdint.h>

#include "net_svc.h"   /* net_svc_write_fn */

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
 * The _Static_assert that ties these to LINK_DATA_TX_BUFS lives in app/nx_net.c, next to
 * the telnet console's facts that configure the socket with NXN_TCP_TX_DEPTH (issue
 * #222; until then it was in app/nx_link_driver.c, which made the link layer include this
 * header).  Putting it in app/link_data.h would make the DATA channel depend on NetX
 * headers.
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

/*
 * Create every ThreadX/NetX object, then the bridge session's thread
 * (link_bridge_init()).  Object creation only -- no blocking and no HAL_GetTick
 * dependency -- so it is safe from tx_application_define() (owhinata/wio-lite-ai#12).  The
 * IP instance exists from boot with address 0.0.0.0, no MAC and the link down; nothing
 * touches the RTL8720 until link_bridge_up().
 */
int  nx_net_init(void);

/*
 * Write the host-stack section of `net info` / the result of arming the interface, one
 * whole line per call of @p write (svc/net/net_svc.h, issue #219).  Every value is
 * sampled first and no lock is held while writing; a failed write ends it.  Returns 0,
 * or -1 after the write that failed (the lines before it stay written).
 */
int nx_net_write_status(net_svc_write_fn write, void *ctx);

#endif /* APP_NX_NET_H */
