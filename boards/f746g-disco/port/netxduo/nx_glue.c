/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    nx_glue.c
 * @brief   NetX Duo IPv4 bring-up + diagnostics facade (issue #49 P2).  See nx_glue.h.
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

#define LOG_TAG "net"
#include "log.h"

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
static bool dhcp_created;
static bool dhcp_started;
static bool static_mode;

/*
 * The one interface (nx_ip_create above).  Every DHCP call below is the
 * INTERFACE-scoped variant with this index, never the global one: the global
 * nx_dhcp_stop() / nx_dhcp_reinitialize() / nx_dhcp_force_renew() loop over the
 * records, DISCARD each per-interface status and unconditionally return
 * NX_SUCCESS (nxd_dhcp_client.c).  Checking their return value is therefore
 * meaningless -- and for force_renew actively harmful, because "nothing was
 * bound" would read as "renew sent".  nx_dhcp_create() enables index 0, so the
 * record always exists.
 */
#define NXG_IFACE  0u

/*
 * Serialises the glue's own state (dhcp_started / static_mode / link_up) together
 * with the NetX DHCP calls that move it.  Without it, `net dhcp` from the shell
 * races the link-status callback on the NetX IP thread, and the pair can leave the
 * flags describing a client that is in a different state.
 *
 * ORDER: this mutex is always taken BEFORE any NetX internal mutex, never after.
 * The link-status callback runs on the IP helper thread holding nx_ip_protection,
 * so it takes this one with TX_NO_WAIT only -- it must never block there.  When it
 * cannot get it, the shell thread that holds it is by definition mid-way through a
 * DHCP operation, so the callback leaves `link_start_pending` for it to finish
 * (nxg_unlock), rather than dropping the link-up event.
 *
 * NOTE (issue for the follow-up, NOT fixed here): NetX itself has a latent
 * IP-mutex/DHCP-mutex cycle -- the IP thread calls this callback while holding
 * nx_ip_protection and the callback calls into DHCP, while the DHCP thread holds
 * the DHCP mutex and calls nx_ip_interface_address_set().  This mutex neither
 * creates nor widens that cycle (the callback never blocks on it); breaking it
 * needs the DHCP start moved off the IP thread entirely.
 */
static TX_MUTEX     nxg_lock;
static volatile bool link_up;              /* last state the callback reported  */
static volatile bool link_start_pending;   /* callback deferred a DHCP start    */

extern VOID nx_eth_driver(NX_IP_DRIVER *driver_req_ptr);

/* ---- glue lock helpers ----------------------------------------------------- */

static void nxg_lock_acquire(void)
{
	(void)tx_mutex_get(&nxg_lock, TX_WAIT_FOREVER);
}

/* Start DHCP if the link is up and nothing else owns the address.  CALL WITH THE
   GLUE LOCK HELD.  Clears the pending flag either way: when the preconditions do
   not hold there is nothing left to defer. */
static void nxg_dhcp_autostart_locked(void)
{
	UINT s;

	link_start_pending = false;
	if (!link_up || !dhcp_created || static_mode || dhcp_started)
		return;

	s = nx_dhcp_interface_start(&eth_dhcp, NXG_IFACE);
	if (s == NX_SUCCESS)
		dhcp_started = true;
	else
		LOG_ERR("dhcp start failed (0x%02x)", (unsigned)s);
}

/* Release the glue lock, first finishing any start the link callback had to defer
   because this thread was holding it. */
static void nxg_unlock(void)
{
	if (link_start_pending)
		nxg_dhcp_autostart_locked();
	tx_mutex_put(&nxg_lock);
}

/* NetX link-status callback (IP helper thread context, under nx_ip_protection):
   update the interface link flag and kick DHCP on the first link-up. */
static void nx_link_status_cb(NX_IP *ip, UINT iface_index, UINT up)
{
	ip->nx_ip_interface[iface_index].nx_interface_link_up = (UCHAR)up;
	link_up = (up != 0u);

	/* TX_NO_WAIT is mandatory here: this runs on the IP thread with
	   nx_ip_protection held, so blocking would stall the whole stack. */
	if (tx_mutex_get(&nxg_lock, TX_NO_WAIT) != TX_SUCCESS) {
		link_start_pending = true;      /* nxg_unlock() picks it up */
		LOG_INF("link-cb up=%u (glue busy; start deferred)", (unsigned)up);
		return;
	}
	nxg_dhcp_autostart_locked();
	tx_mutex_put(&nxg_lock);
}

int nx_net_init(void)
{
	UINT s;

	/* Before anything can change the glue's state (the link callback fires as
	   soon as nx_ip_create runs the driver). */
	if (tx_mutex_create(&nxg_lock, "nxglue", TX_INHERIT) != TX_SUCCESS) {
		LOG_ERR("glue mutex create failed");
		return NXG_ERR;
	}

	nx_system_initialize();

	s = nx_packet_pool_create(&eth_pool, "eth", NXG_PAYLOAD,
	                          eth_pool_mem, sizeof eth_pool_mem);
	if (s != NX_SUCCESS) {
		LOG_ERR("packet pool create failed (0x%02x)", s);
		return NXG_ERR;
	}

	/* Bind the RX pool to the driver BEFORE nx_ip_create (which runs the driver
	   INITIALIZE on the IP thread) -- it measures the packet<->payload offset. */
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

	/* DHCP is the boot default (user choice).  Created here, started by the
	   link-up callback; reuse the shared non-cacheable pool. */
	if (nx_dhcp_create(&eth_dhcp, &eth_ip, "eth") == NX_SUCCESS) {
		nx_dhcp_packet_pool_set(&eth_dhcp, &eth_pool);
		dhcp_created = true;
	} else {
		LOG_WRN("dhcp create failed; use 'net ip' for a static address");
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

int nx_net_info_get(struct nx_net_info *out)
{
	ULONG ip = 0, mask = 0, gw = 0;

	if (!nx_up)
		return NXG_ERR_STATE;

	/* Snapshot under the lock so `net info` cannot catch a half-applied
	   transition (address already cleared, flags not yet updated). */
	nxg_lock_acquire();
	nx_ip_address_get(&eth_ip, &ip, &mask);
	nx_ip_gateway_address_get(&eth_ip, &gw);
	out->ip = (uint32_t)ip;
	out->mask = (uint32_t)mask;
	out->gw = (uint32_t)gw;
	out->ip_valid = (ip != 0);
	out->dhcp_mode = dhcp_created && !static_mode;
	nxg_unlock();
	return NXG_OK;
}

/*
 * Switch to a static address.  Transactional: either the address, the gateway and
 * the mode flags all move together, or nothing does.  The old code stopped DHCP
 * and set static_mode BEFORE the address call, so a failure there left the board
 * with no DHCP client, no address, and a mode flag claiming static.
 */
int nx_net_set_static(uint32_t ip, uint32_t mask, uint32_t gw)
{
	ULONG old_ip = 0, old_mask = 0, old_gw = 0;
	bool  old_started, old_static;
	int   rc = NXG_OK;
	UINT  s;

	if (!nx_up)
		return NXG_ERR_STATE;

	nxg_lock_acquire();

	nx_ip_address_get(&eth_ip, &old_ip, &old_mask);
	nx_ip_gateway_address_get(&eth_ip, &old_gw);
	old_started = dhcp_started;
	old_static  = static_mode;

	if (dhcp_started) {
		s = nx_dhcp_interface_stop(&eth_dhcp, NXG_IFACE);
		if (s != NX_SUCCESS && s != NX_DHCP_NOT_STARTED) {
			rc = NXG_ERR;
			goto out;              /* nothing changed yet */
		}
		dhcp_started = false;
	}

	if (nx_ip_address_set(&eth_ip, ip, mask) != NX_SUCCESS) {
		rc = NXG_ERR;
		goto rollback;
	}
	if (nx_ip_gateway_address_set(&eth_ip, gw) != NX_SUCCESS) {
		rc = NXG_ERR;
		goto rollback;
	}

	static_mode = true;
	LOG_INF("static %lu.%lu.%lu.%lu", (ip >> 24) & 0xFF, (ip >> 16) & 0xFF,
	        (ip >> 8) & 0xFF, ip & 0xFF);
	goto out;

rollback:
	/* Order matters: nx_dhcp_interface_reinitialize() clears the DHCP-managed
	   address, so it has to run BEFORE the old address is put back.  It is also
	   mandatory before a restart -- NetX documents that a stopped client must be
	   reinitialised before nx_dhcp_interface_start() (nxd_dhcp_client.c). */
	if (old_started)
		(void)nx_dhcp_interface_reinitialize(&eth_dhcp, NXG_IFACE);
	(void)nx_ip_address_set(&eth_ip, old_ip, old_mask);
	if (old_gw != 0u)                  /* 0 == there was no gateway to restore */
		(void)nx_ip_gateway_address_set(&eth_ip, old_gw);
	static_mode = old_static;
	if (old_started) {
		if (nx_dhcp_interface_start(&eth_dhcp, NXG_IFACE) == NX_SUCCESS) {
			dhcp_started = true;
		} else {
			/* Could not put DHCP back: leave a state the link-up callback
			   and a later `net dhcp` can recover from, rather than one that
			   claims a client that is not running. */
			dhcp_started = false;
			static_mode  = false;
			LOG_ERR("static failed and DHCP could not be restarted");
		}
	}

out:
	nxg_unlock();
	return rc;
}

/*
 * `net dhcp`: re-acquire an address.  Three cases, because the one thing this must
 * not do is throw away a working lease (issue #9 -- the old code called
 * nx_dhcp_reinitialize() on a RUNNING client, which clears the address, and then
 * nx_dhcp_start() failed on the still-bound socket and its status was discarded,
 * leaving the board with no address and no way back).
 *
 *   not started  -> start
 *   BOUND        -> force renew; the lease stays valid the whole time (NXG_RENEWED)
 *   started but not bound (stuck in INIT/SELECTING/REQUESTING)
 *                -> stop, reinitialise, start.  Nothing is lost: there is no lease.
 */
int nx_net_dhcp_renew(void)
{
	int  rc;
	UINT s;

	if (!nx_up)
		return NXG_ERR_STATE;
	if (!dhcp_created)
		return NXG_ERR;

	nxg_lock_acquire();
	static_mode = false;

	if (!dhcp_started) {
		s = nx_dhcp_interface_start(&eth_dhcp, NXG_IFACE);
		if (s != NX_SUCCESS) {
			LOG_ERR("dhcp start failed (0x%02x)", (unsigned)s);
			rc = NXG_ERR;
			goto out;
		}
		dhcp_started = true;
		rc = NXG_OK;
		goto out;
	}

	s = nx_dhcp_interface_force_renew(&eth_dhcp, NXG_IFACE);
	if (s == NX_SUCCESS) {
		rc = NXG_RENEWED;              /* lease kept, DHCPREQUEST sent */
		goto out;
	}
	if (s != NX_DHCP_NOT_BOUND) {
		LOG_ERR("dhcp force renew failed (0x%02x)", (unsigned)s);
		rc = NXG_ERR;
		goto out;
	}

	/* Not bound: restart the state machine from scratch. */
	s = nx_dhcp_interface_stop(&eth_dhcp, NXG_IFACE);
	if (s != NX_SUCCESS && s != NX_DHCP_NOT_STARTED) {
		LOG_ERR("dhcp stop failed (0x%02x)", (unsigned)s);
		rc = NXG_ERR;
		goto out;
	}
	dhcp_started = false;              /* the link callback may restart it now */

	s = nx_dhcp_interface_reinitialize(&eth_dhcp, NXG_IFACE);
	if (s != NX_SUCCESS) {
		LOG_ERR("dhcp reinitialize failed (0x%02x)", (unsigned)s);
		rc = NXG_ERR;
		goto out;
	}
	s = nx_dhcp_interface_start(&eth_dhcp, NXG_IFACE);
	if (s != NX_SUCCESS) {
		LOG_ERR("dhcp restart failed (0x%02x)", (unsigned)s);
		rc = NXG_ERR;
		goto out;
	}
	dhcp_started = true;
	rc = NXG_OK;

out:
	nxg_unlock();
	return rc;
}

int nx_net_ping(uint32_t ip, unsigned timeout_ms, unsigned *rtt_ms)
{
	NX_PACKET *resp = NX_NULL;
	ULONG t0;
	UINT rc;

	if (!nx_up)
		return NXG_ERR_STATE;

	t0 = tx_time_get();
	rc = nx_icmp_ping(&eth_ip, (ULONG)ip, "nx_glue_ping", 12, &resp,
	                  (ULONG)timeout_ms);    /* NX_IP_PERIODIC_RATE=1000 -> ms    */
	if (rc == NX_SUCCESS) {
		if (rtt_ms != NULL)
			*rtt_ms = (unsigned)(tx_time_get() - t0);
		nx_packet_release(resp);
		return NXG_OK;
	}
	if (resp != NX_NULL)
		nx_packet_release(resp);
	return (rc == NX_NO_RESPONSE) ? NXG_TIMEOUT : NXG_ERR;
}
