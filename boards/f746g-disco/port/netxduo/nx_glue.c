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
#include "stm32f7xx_hal.h"    /* __get_PRIMASK / __disable_irq / __set_PRIMASK   */

#include "nx_glue.h"
#include "nx_eth_driver.h"
#include "net_core.h"         /* the shared address / DHCP / ping (issue #220) */

#define LOG_TAG "net"
#include "log.h"

#include <string.h>           /* memset (nx_net_diag_get)                         */

/* Pool: one Ethernet frame (1514) + the driver's 2-byte RX align pad + headroom,
   ~38 packets.  In non-cacheable SDRAM (zero-copy: payloads go straight to the
   ETH DMA). */
#define NXG_PAYLOAD       1600u
#define NXG_POOL_BYTES    (64u * 1024u)
#define NXG_IP_PRIORITY   12          /* below camera(10), above touch/GUIX/cli  */
#define NXG_IP_STACK      2048u
#define NXG_ARP_CACHE     1040u       /* ~20 entries                             */

static UCHAR eth_pool_mem[NXG_POOL_BYTES]
	__attribute__((aligned(32), section(".sdram.eth")));

static NX_PACKET_POOL eth_pool;       /* control block: CPU-only -> regular SRAM */
static NX_IP          eth_ip;
static NX_DHCP        eth_dhcp;        /* user pool set -> no embedded DMA buffer */
static ULONG          ip_stack[NXG_IP_STACK / sizeof(ULONG)];
static ULONG          arp_cache[NXG_ARP_CACHE / sizeof(ULONG)];

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
 * the address lock, the source, the DHCP client's state, the stale mark and the
 * recorded link level.  CPU-only, so regular SRAM like the other control
 * blocks.  The lock order on this board is
 *
 *     address lock -> nx_dhcp_mutex -> nx_ip_protection -> eth_lock
 *
 * and it is acyclic only because nothing takes the address lock while holding
 * a NetX mutex or eth_lock: the link-status callback (IP thread, under
 * nx_ip_protection) only records the level, and the DHCP start after a link-up
 * runs on the eth-link poll thread, which holds nothing (issue #13).
 */
struct net_core_state net_core_board_state;
const struct net_core_facts net_core_board_facts = {
	.ip          = &eth_ip,
	.dhcp        = &eth_dhcp,
	.iface       = NXG_IFACE,
	.boot_source = NET_CORE_SRC_DHCP,     /* DHCP is the boot default */
	.why_link    = "the Ethernet link is down (no cable? see `eth`)",
	.why_down    = "IP stack not up",
};

/*
 * The autostart request: written by the IP thread in the link-status callback,
 * read and cleared by the eth-link poll hook.  Every access is inside a short
 * save/restore-PRIMASK section (NXG_CRIT_*), which on this single-core part is
 * what excludes the producer from the consumer's snapshot.  [!] TEMPORARY:
 * issue #220 step 2 moves the reaction to the link into the shared worker.
 */
static volatile bool link_start_pending;   /* a link-up wants DHCP started      */

/* PRIMASK critical section, as in nx_shell.c / backend/cli_backend_uart.c.  Nests
   safely inside ThreadX's own PRIMASK sections, and restores the previous mask
   rather than enabling unconditionally. */
#define NXG_CRIT_ENTER()  do { uint32_t _pm = __get_PRIMASK(); __disable_irq()
#define NXG_CRIT_EXIT()   __set_PRIMASK(_pm); } while (0)

extern VOID nx_eth_driver(NX_IP_DRIVER *driver_req_ptr);

/*
 * Poll-complete hook, registered with the ETH driver and run on the `eth-link`
 * thread (issue #13).  This is where the DHCP autostart actually happens: that
 * thread holds no lock at all when it gets here, so the shared start -- the
 * address lock, and through it the DHCP and IP mutexes -- follows the
 * documented order.  Waiting here delays only the next PHY poll.
 *
 * The request is consumed even if the shared side then finds nothing to do (the
 * source is static, the client runs): a start that fails is retried on the next
 * link-up or a manual `net dhcp`, not five times a second.
 */
static void nxg_poll_hook(void *arg)
{
	bool pending;
	UINT s;

	(void)arg;
	NXG_CRIT_ENTER();
	pending            = link_start_pending;
	link_start_pending = false;
	NXG_CRIT_EXIT();
	if (!pending)
		return;
	s = net_core_link_up_start();
	if (s != NX_SUCCESS)
		LOG_ERR("dhcp start failed (0x%02x)", (unsigned)s);
}

/*
 * NetX link-status callback -- IP helper thread, WITH nx_ip_protection HELD.
 *
 * Everything here must be non-blocking and NetX-free: taking the DHCP mutex from
 * this context is the deadlock in issue #13, and taking the address lock would put
 * a NetX mutex above it in the order.  So this only publishes: NetX's own link
 * flag (a byte store, on the thread NetX itself would have written it --
 * upstream's deferred link-status processing never assigns it, and route lookup
 * reads it outside the IP mutex), then the shared record of the level
 * (net_core_link_note(), one masked store) and the autostart request.
 *
 * The request is published on every callback reporting up, not only on a
 * down -> up transition, because NetX raises this callback for a speed or duplex
 * change too and the old code started DHCP on those as well.  It cannot become a
 * busy loop: the driver only raises the event when the PHY state actually changed.
 */
static void nx_link_status_cb(NX_IP *ip, UINT iface_index, UINT up)
{
	ip->nx_ip_interface[iface_index].nx_interface_link_up = (UCHAR)up;

	net_core_link_note(up != 0u);
	if (up != 0u) {
		NXG_CRIT_ENTER();
		link_start_pending = true;
		NXG_CRIT_EXIT();
	}
}

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

	nx_ip_link_status_change_notify_set(&eth_ip, nx_link_status_cb);

	/* DHCP is the boot default (user choice).  Created here; the link-status
	   callback publishes the request and the eth-link poll thread does the start
	   (issue #13).  Reuses the shared non-cacheable pool. */
	if (nx_dhcp_create(&eth_dhcp, &eth_ip, "eth") == NX_SUCCESS) {
		nx_dhcp_packet_pool_set(&eth_dhcp, &eth_pool);
		dhcp_created = true;
	} else {
		LOG_WRN("dhcp create failed; use 'net ip' for a static address");
	}

	/* The shared operations' lock and state.  Before the hook below and before
	   the scheduler starts, so the first link callback finds them in place. */
	if (net_core_init(dhcp_created) != 0) {
		LOG_ERR("net core init failed");
		return NXG_ERR;
	}

	/* Last, so every precondition the hook reads is already final.  Ordering is
	   not what makes this safe -- nothing runs until the scheduler starts -- but
	   it keeps the hook from being readable as "may run mid-init". */
	nx_eth_set_poll_hook(nxg_poll_hook, NULL);

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
