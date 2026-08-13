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
#include "stm32f7xx_hal.h"    /* __get_PRIMASK / __disable_irq / __set_PRIMASK   */

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
 * Serialises the glue's own state (dhcp_started / static_mode) together with the
 * NetX DHCP calls that move it.  Without it, `net dhcp` from the shell races the
 * link-status callback on the NetX IP thread, and the pair can leave the flags
 * describing a client that is in a different state.
 *
 * ORDER: this mutex is always taken BEFORE any NetX internal mutex, never after.
 * The whole lock graph on this board is
 *
 *     nxg_lock -> nx_dhcp_mutex -> nx_ip_protection -> eth_lock
 *
 * and it is acyclic only because NOTHING takes this mutex while holding a NetX
 * mutex or eth_lock.  The one edge that used to violate it was
 * nx_ip_protection -> nx_dhcp_mutex: the IP thread calls the link-status callback
 * while holding nx_ip_protection, and that callback used to start DHCP, whose
 * first act is to take the DHCP mutex -- while the DHCP thread holds that mutex
 * across its whole loop body and calls nx_ip_interface_address_set(), which waits
 * for nx_ip_protection.  Both threads stopping takes the entire stack down, not
 * just DHCP (issue #13).  The callback therefore no longer calls NetX at all: it
 * publishes the link state and the autostart request, and the work runs on the
 * eth-link poll thread, which holds nothing (see nxg_poll_hook).
 */
static TX_MUTEX nxg_lock;

/*
 * The link handshake: written by the IP thread in the link-status callback, read
 * and cleared by the two consumers below.  `volatile` is not what makes this safe
 * -- it stops the compiler caching the loads, but it is not an inter-thread
 * synchronisation primitive and gives no ordering.  Every access is inside a short
 * save/restore-PRIMASK section (NXG_CRIT_*), which on this single-core part is
 * what actually excludes the producer from a consumer's snapshot.
 */
static volatile bool link_up;              /* last state the callback reported  */
static volatile bool link_start_pending;   /* a link-up wants DHCP started      */

/* PRIMASK critical section, as in nx_shell.c / backend/cli_backend_uart.c.  Nests
   safely inside ThreadX's own PRIMASK sections, and restores the previous mask
   rather than enabling unconditionally. */
#define NXG_CRIT_ENTER()  do { uint32_t _pm = __get_PRIMASK(); __disable_irq()
#define NXG_CRIT_EXIT()   __set_PRIMASK(_pm); } while (0)

extern VOID nx_eth_driver(NX_IP_DRIVER *driver_req_ptr);

/* ---- glue lock helpers ----------------------------------------------------- */

static void nxg_lock_acquire(void)
{
	(void)tx_mutex_get(&nxg_lock, TX_WAIT_FOREVER);
}

/*
 * Consume a published link-up: start DHCP if nothing else owns the address.  CALL
 * WITH THE GLUE LOCK HELD, and never from the IP thread (issue #13).
 *
 * The snapshot and the clear happen together under PRIMASK so the producer cannot
 * land between them; everything that can block or take a lock -- the DHCP call and
 * its logging -- runs afterwards, with interrupts back on.  The edge is consumed
 * even if the preconditions turn out to be false: that matches the previous
 * behaviour, where a start that fails is retried on the next qualifying link event
 * or a manual `net dhcp`, not five times a second.
 */
static void nxg_run_link_pending_locked(void)
{
	bool pending, up;
	UINT s;

	NXG_CRIT_ENTER();
	pending            = link_start_pending;
	up                 = link_up;
	link_start_pending = false;
	NXG_CRIT_EXIT();

	if (!pending || !up || !dhcp_created || static_mode || dhcp_started)
		return;

	s = nx_dhcp_interface_start(&eth_dhcp, NXG_IFACE);
	if (s == NX_SUCCESS)
		dhcp_started = true;
	else
		LOG_ERR("dhcp start failed (0x%02x)", (unsigned)s);
}

/* Release the glue lock, first finishing any start published while this thread
   held it -- so a link-up during a shell command starts DHCP at once instead of
   waiting for the next PHY poll. */
static void nxg_unlock(void)
{
	nxg_run_link_pending_locked();
	tx_mutex_put(&nxg_lock);
}

/*
 * Poll-complete hook, registered with the ETH driver and run on the `eth-link`
 * thread (issue #13).  This is where the DHCP autostart actually happens: that
 * thread holds no lock at all when it gets here, so taking the glue lock -- and
 * through it the DHCP and IP mutexes -- follows the documented order.  Waiting
 * here delays only the next PHY poll.
 */
static void nxg_poll_hook(void *arg)
{
	(void)arg;
	nxg_lock_acquire();
	nxg_run_link_pending_locked();
	tx_mutex_put(&nxg_lock);
}

/*
 * NetX link-status callback -- IP helper thread, WITH nx_ip_protection HELD.
 *
 * Everything here must be non-blocking and NetX-free: taking the DHCP mutex from
 * this context is the deadlock in issue #13, and taking the glue lock would put a
 * NetX mutex above it in the order.  So this only publishes: NetX's own link flag
 * (a byte store, on the thread NetX itself would have written it -- upstream's
 * deferred link-status processing never assigns it, and route lookup reads it
 * outside the IP mutex), then the glue's copy plus the autostart request.
 *
 * The request is published on every callback reporting up, not only on a
 * down -> up transition, because NetX raises this callback for a speed or duplex
 * change too and the old code started DHCP on those as well.  It cannot become a
 * busy loop: the driver only raises the event when the PHY state actually changed.
 */
static void nx_link_status_cb(NX_IP *ip, UINT iface_index, UINT up)
{
	ip->nx_ip_interface[iface_index].nx_interface_link_up = (UCHAR)up;

	NXG_CRIT_ENTER();
	link_up = (up != 0u);
	if (up != 0u)
		link_start_pending = true;
	NXG_CRIT_EXIT();
}

int nx_net_init(void)
{
	UINT s;

	/* Before anything can change the glue's state.  This whole function runs from
	   tx_application_define(), i.e. with the scheduler not yet started, so neither
	   the IP thread nor the eth-link thread can observe a half-built glue -- but
	   the mutex still has to exist before the first user of it is created. */
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
