/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    nx_glue.c
 * @brief   NetX Duo IPv4 bring-up + diagnostics facade (owhinata/stm32f746g-disco#49
 * P2). See nx_glue.h.
 *
 * The single packet pool lives in `.sdram.eth` (FMC bank2, MPU non-cacheable):
 * the new HAL_ETH does no D-cache maintenance, so every DMA-visible payload --
 * RX buffers, TX payloads, and (via nx_dhcp_packet_pool_set) the DHCP datagrams
 * -- must be non-cacheable.  The control blocks (NX_IP/NX_PACKET_POOL/NX_DHCP)
 * and the IP thread stack / ARP cache are CPU-only, so they stay in regular SRAM.
 */
#include "nx_api.h"
#include "nxd_dhcp_client.h"

#include "nx_glue.h"
#include "nx_eth_driver.h"
#include "net_core.h"         /* the shared address / DHCP / ping (issue #220) */
#include "net_tty.h"          /* the shared telnet console (issue #221) */

#define LOG_TAG "net"
#include "log.h"

#include <string.h>           /* memset (nx_net_diag_get)                         */

/* Pool: one Ethernet frame (1514) + the driver's 2-byte RX align pad + headroom,
   ~38 packets.  In non-cacheable SDRAM (zero-copy: payloads go straight to the
   ETH DMA). */
#define NXG_PAYLOAD       1600u
#define NXG_POOL_BYTES    (64u * 1024u)
#define NXG_IP_PRIORITY   12          /* below camera(10), above touch/GUIX/cli  */
/* The shared link worker must run below the IP thread, so it cannot run before
   the link notification returns (shell/cmds/net_core.h). */
_Static_assert(NXG_IP_PRIORITY < NET_CORE_WORKER_PRIORITY,
               "the net link worker must be below the IP thread");
#define NXG_IP_STACK      2048u
#define NXG_ARP_CACHE     1040u       /* ~20 entries                             */
/*
 * The shared link worker (shell/cmds/net_core.c, issue #220): one pass is the
 * address lock, then at most a DHCP stop and a reinitialise / address drop /
 * start, each a short NetX call chain on this thread.  1 KB, like the eth-link
 * thread that ran the same DHCP start until #220 (1024 B, 408 B peak).
 */
#define NXG_WORKER_STACK  1024u

static UCHAR eth_pool_mem[NXG_POOL_BYTES]
	__attribute__((aligned(32), section(".sdram.eth")));

static NX_PACKET_POOL eth_pool;       /* control block: CPU-only -> regular SRAM */
static NX_IP          eth_ip;
static NX_DHCP        eth_dhcp;        /* user pool set -> no embedded DMA buffer */
static ULONG          ip_stack[NXG_IP_STACK / sizeof(ULONG)];
static ULONG          arp_cache[NXG_ARP_CACHE / sizeof(ULONG)];
static ULONG          net_worker_stack[NXG_WORKER_STACK / sizeof(ULONG)];

static bool nx_up;

/*
 * The one interface (nx_ip_create below).  The shared operations name it on
 * every call and use only the interface-scoped DHCP calls (shell/cmds/
 * net_core.c says why).  nx_dhcp_create() enables index 0, so the record
 * always exists.
 */
#define NXG_IFACE  0u

/*
 * The shared operations' state and facts (shell/cmds/net_core.h, issue #220):
 * the address lock, the link worker and its wake, the source, the DHCP
 * client's state, the stale mark, the recorded link level and the worker's
 * last failure.  CPU-only, so regular SRAM like the other control blocks (the
 * worker's stack too).  The lock order on this board is
 *
 *     address lock -> nx_dhcp_mutex -> nx_ip_protection -> eth_lock
 *
 * and it is acyclic only because nothing takes the address lock while holding
 * a NetX mutex or eth_lock: the shared link notification (IP thread, under
 * nx_ip_protection) only records the level and wakes the worker, and the
 * worker and the commands hold nothing when they take it (issue #13).
 */
struct net_core_state net_core_board_state;
const struct net_core_facts net_core_board_facts = {
	.ip          = &eth_ip,
	.dhcp        = &eth_dhcp,
	.iface       = NXG_IFACE,
	.boot_source = NET_CORE_SRC_DHCP,     /* DHCP is the boot default */
	.why_link    = "the Ethernet link is down (no cable? see `eth`)",
	.why_down    = "IP stack not up",
	.worker_stack      = net_worker_stack,
	.worker_stack_size = sizeof net_worker_stack,
};

/*
 * The shared telnet console's state and facts (shell/cmds/net_tty.h, issue
 * #221).  CPU-only, so regular SRAM like the other control blocks: the rings,
 * the per-packet extraction buffer and the listener's stack.
 *
 * The listener's stack: it runs the whole transmit path (packet allocate /
 * append / nx_tcp_socket_send -> IP -> nx_eth_driver), and the per-packet
 * receive work is the IP thread's.  2560 B as before #221, with a measured
 * peak of 448 B on this board; re-measure with `thread` after a change to
 * the drain.
 *
 * The transmit queue depth (8) bounds what this console can hold of
 * eth_pool; 2 s / 10 retries / shift 1 put the first retransmit at 2 s,
 * which the console's no-progress deadline must exceed (below).
 */
#define NXG_TTY_RX_RING   512u
#define NXG_TTY_EXTRACT   1500u
#define NXG_TTY_TX_RING   4096u
#define NXG_TTY_STACK     2560u
#define NXG_TTY_TX_QUEUE  8u
#define NXG_TTY_RTO       (2u * NX_IP_PERIODIC_RATE)
_Static_assert(NET_TTY_TX_DEADLINE > NXG_TTY_RTO,
               "the telnet deadline must cover the first retransmit");

static uint8_t net_tty_rx_buf[NXG_TTY_RX_RING];
static uint8_t net_tty_extract[NXG_TTY_EXTRACT];
static uint8_t net_tty_tx_buf[NXG_TTY_TX_RING];
static UCHAR   net_tty_stack[NXG_TTY_STACK] __attribute__((aligned(8)));

struct net_tty_state net_tty_board_state;
const struct net_tty_facts net_tty_board_facts = {
	.ip             = &eth_ip,
	.pool           = &eth_pool,
	.rx_buf         = net_tty_rx_buf,  .rx_size      = sizeof net_tty_rx_buf,
	.extract_buf    = net_tty_extract, .extract_size = sizeof net_tty_extract,
	.tx_buf         = net_tty_tx_buf,  .tx_size      = sizeof net_tty_tx_buf,
	.stack          = net_tty_stack,   .stack_size   = sizeof net_tty_stack,
	.tx_queue_depth = NXG_TTY_TX_QUEUE,
	.rto_ticks      = NXG_TTY_RTO,
	.rto_retries    = 10u,
	.rto_shift      = 1u,
	.other_console  = "VCP",
};

extern VOID nx_eth_driver(NX_IP_DRIVER *driver_req_ptr);

int nx_net_init(void)
{
	bool dhcp_created = false;
	UINT s;

	nx_system_initialize();

	s = nx_packet_pool_create(&eth_pool, "eth", NXG_PAYLOAD,
	                          eth_pool_mem, sizeof eth_pool_mem);
	if (s != NX_SUCCESS) {
		LOG_ERR("packet pool create failed (0x%02x)", s);
		return NXG_ERR;
	}

	/* Bind the RX pool to the driver BEFORE nx_ip_create, which creates the IP
	   thread that later runs the driver's INITIALIZE -- it measures the
	   packet<->payload offset. */
	nx_eth_driver_set_pool(&eth_pool);

	s = nx_ip_create(&eth_ip, "eth", 0, 0xFFFFFF00UL, &eth_pool, nx_eth_driver,
	                 (VOID *)ip_stack, sizeof ip_stack, NXG_IP_PRIORITY);
	if (s != NX_SUCCESS) {
		LOG_ERR("ip create failed (0x%02x)", s);
		return NXG_ERR;
	}

	nx_arp_enable(&eth_ip, (VOID *)arp_cache, sizeof arp_cache);
	nx_icmp_enable(&eth_ip);
	nx_udp_enable(&eth_ip);              /* DHCP needs UDP                        */
	nx_tcp_enable(&eth_ip);              /* P3-ready (no sockets yet)             */

	/* DHCP is the boot default (user choice).  Created here; the shared link
	   worker starts it when the link comes up (issue #220).  Reuses the shared
	   non-cacheable pool. */
	if (nx_dhcp_create(&eth_dhcp, &eth_ip, "eth") == NX_SUCCESS) {
		nx_dhcp_packet_pool_set(&eth_dhcp, &eth_pool);
		dhcp_created = true;
	} else {
		LOG_WRN("dhcp create failed; use 'net ip' for a static address");
	}

	/* The shared operations' lock and state, the link worker and the link
	   notification (it registers NetX's link-status callback).  Before the
	   scheduler starts, so the first link notice finds them in place. */
	if (net_core_init(dhcp_created) != 0) {
		LOG_ERR("net core init failed");
		return NXG_ERR;
	}

	nx_up = true;
	LOG_INF("NetX Duo up (IPv4); pool %u B, IP thread prio %u",
	        (unsigned)NXG_POOL_BYTES, NXG_IP_PRIORITY);
	return NXG_OK;
}

bool nx_net_is_up(void)
{
	return nx_up;
}

void *nx_net_ip(void)
{
	return nx_up ? (void *)&eth_ip : NULL;
}

void *nx_net_pool(void)
{
	return nx_up ? (void *)&eth_pool : NULL;
}

/* Counters for `net info` (issue #202).  Only reads: nx_tcp_info_get() and
   nx_packet_pool_info_get() take the IP / pool protection themselves, and the
   driver's counters are copied without a lock (nx_eth_driver_get_stats()). */
int nx_net_diag_get(struct nx_net_diag *out)
{
	struct nx_eth_stats es;
	ULONG ps = 0, pbs = 0, prs = 0, pbr = 0, pinv = 0, prd = 0, pck = 0;
	ULONG pc = 0, pd = 0, pcd = 0, prt = 0;
	ULONG tot = 0, fr = 0, er = 0, es_ = 0, ir = 0;

	if (!nx_up)
		return NXG_ERR_STATE;
	memset(out, 0, sizeof *out);

	nx_eth_driver_get_stats(&es);
	out->rx_ok            = (uint32_t)es.rx_ok;
	out->rx_drop          = (uint32_t)es.rx_drop;
	out->rx_no_buf        = (uint32_t)es.rx_no_buf;
	out->tx_ok            = (uint32_t)es.tx_ok;
	out->tx_drop          = (uint32_t)es.tx_drop;
	out->tx_drop_mac_down = (uint32_t)es.tx_drop_mac_down;
	out->tx_drop_coalesce = (uint32_t)es.tx_drop_coalesce;
	out->tx_drop_hal      = (uint32_t)es.tx_drop_hal;
	out->hal_err_last     = (uint32_t)es.hal_err_last;
	out->hal_dma_err_last = (uint32_t)es.hal_dma_err_last;
	out->hal_state_last   = (uint32_t)es.hal_state_last;
	out->dma_err          = (uint32_t)es.dma_err;
	out->dma_err_last     = (uint32_t)es.dma_err_last;

	if (nx_tcp_info_get(&eth_ip, &ps, &pbs, &prs, &pbr, &pinv, &prd, &pck,
	                    &pc, &pd, &pcd, &prt) == NX_SUCCESS) {
		out->tcp_valid         = true;
		out->tcp_pkts_sent     = (uint32_t)ps;
		out->tcp_bytes_sent    = (uint32_t)pbs;
		out->tcp_pkts_rcvd     = (uint32_t)prs;
		out->tcp_bytes_rcvd    = (uint32_t)pbr;
		out->tcp_invalid       = (uint32_t)pinv;
		out->tcp_rx_dropped    = (uint32_t)prd;
		out->tcp_checksum_err  = (uint32_t)pck;
		out->tcp_conns         = (uint32_t)pc;
		out->tcp_disconns      = (uint32_t)pd;
		out->tcp_conns_dropped = (uint32_t)pcd;
		out->tcp_retransmits   = (uint32_t)prt;
	}
	if (nx_packet_pool_info_get(&eth_pool, &tot, &fr, &er, &es_, &ir)
	    == NX_SUCCESS) {
		out->pool_valid       = true;
		out->pool_total       = (uint32_t)tot;
		out->pool_free        = (uint32_t)fr;
		out->pool_empty_req   = (uint32_t)er;
		out->pool_empty_susp  = (uint32_t)es_;
		out->pool_invalid_rel = (uint32_t)ir;
	}
	return NXG_OK;
}
