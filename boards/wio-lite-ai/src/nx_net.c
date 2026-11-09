/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/*
 * The host's own IP interface over the RTL8720 L2 bridge (owhinata/wio-lite-ai#23 U3):
 * the NetX objects and the shared operations' state.  See app/nx_net.h.  The bridge
 * session underneath is app/link_bridge.c (issue #222).
 */
#include "nx_net.h"

#include <stdarg.h>

#include "nx_api.h"
#include "nxd_dhcp_client.h"

#include "fmt.h"
#include "link_bridge.h"      /* the bridge session below this file (issue #222) */
#include "link_data.h"        /* LINK_DATA_TX_BUFS, for the transmit budget */
#define LOG_TAG "nx"
#include "log.h"
#include "net_core.h"         /* the shared address / DHCP / ping (issue #220) */
#include "net_tty.h"          /* the shared telnet console (issue #221) */
#include "nx_link_driver.h"
#include "mem_sections.h"  /* DTCM_BSS: CPU-only data out of AXI-SRAM (issue #46) */

/* ---- tunables --------------------------------------------------------------- */

/*
 * Packet pool.  1600 leaves room above a 1514-byte frame for the 2-byte receive pad and
 * the NX_PHYSICAL_HEADER/TRAILER reservation.
 *
 * 48 kB is about 29 packets (U3 ran 32 kB / 19, which was sized for ARP, DHCP and ICMP
 * alone).  U4 puts TCP sockets on this interface, and a socket holds packets that the pool
 * cannot reclaim until they are acknowledged -- up to NXN_TCP_TX_DEPTH per socket, for as
 * many as NXN_TCP_SOCKETS_MAX of them -- on top of whatever is in flight on the receive
 * side.  Running the pool dry does not merely slow things down: nx_link_driver_rx()
 * allocates with NX_NO_WAIT, so an empty pool is a dropped frame and, for a stream, a
 * retransmit timeout.
 *
 * U4-3 measured it and DELIBERATELY DID NOT SHRINK IT.  The low-water mark across 2.5 MB
 * of echoed TCP, DHCP and a telnet console was 25 of 29 -- four packets ever in use -- so
 * the U3 figure of 19 would have been ample and would have returned 16 kB of DTCM.  It is
 * not taken because the trade is bad in both directions: nothing else on this MCU is
 * asking for that DTCM (the region is under half used), while being wrong costs exactly
 * the failure this whole increment exists to prevent.  Cheap insurance against an
 * expensive mistake is worth buying even when the premium looks unnecessary.
 */
#define NXN_PAYLOAD        1600u
#define NXN_POOL_BYTES     (48u * 1024u)

#define NXN_IP_PRIORITY    11u        /* just below the link service thread (10)     */
/* The shared link worker must run below the IP thread, so it cannot run before the link
 * notification returns (shell/cmds/net_core.h). */
_Static_assert(NXN_IP_PRIORITY < NET_CORE_WORKER_PRIORITY,
               "the net link worker must be below the IP thread");
#define NXN_IP_STACK       2048u
#define NXN_ARP_CACHE      1040u      /* ~20 entries                                 */


/*
 * The shared link worker's stack (shell/cmds/net_core.c, issue #220).  One pass is the
 * address lock, then at most a DHCP stop and a reinitialise / address drop / start, each
 * a short NetX call chain; the eRPC never runs on it.  1 KB, against this board's nx-net
 * owner at 3072 B with a measured peak of 788-860 B on far longer chains (eRPC calls with
 * 160-byte locals).  DTCM like every other thread stack here (check_dtcm_residency.py).
 */
#define NXN_WORKER_STACK   1024u


/* ---- state ------------------------------------------------------------------ */

/*
 * The pool lives in DTCM (ldscript .nx_pool).  The only bus master that ever touches a
 * payload is the CPU -- frames are memcpy'd to and from the link's DATA pool, there is no
 * DMA in this firmware at all -- so DTCM is both safe and the better choice: zero wait
 * states, no D-cache lines spent on 1.5 kB frames, and none of the AXI-SRAM budget.
 */
static UCHAR nxn_pool_mem[NXN_POOL_BYTES]
	__attribute__((aligned(32), section(".nx_pool")));

static NX_PACKET_POOL nxn_pool;
static NX_IP          nxn_ip;
static NX_DHCP        nxn_dhcp;
static ULONG          nxn_ip_stack[NXN_IP_STACK / sizeof(ULONG)] DTCM_BSS;
static ULONG          nxn_arp_cache[NXN_ARP_CACHE / sizeof(ULONG)];
static UCHAR          net_worker_stack[NXN_WORKER_STACK] DTCM_BSS __attribute__((aligned(8)));

static bool nxn_dhcp_created;

/*
 * The shared operations' state and facts (shell/cmds/net_core.h, issue #220):
 * the address lock, the link worker and its wake, the source, the DHCP client's
 * state, the stale mark, the recorded link level and the worker's last failure.
 * Plain .bss (AXI-SRAM) like the other NetX control blocks here; nothing but the
 * CPU touches it.  The worker's stack is DTCM (net_worker_stack above).
 *
 * LOCK ORDER: the address lock is NEVER taken while the rtl_link coarse mutex is
 * held.  It is taken only by threads that hold no other lock: the CLI entry points
 * (which take no link at all), the shared link worker and the KV boot thread
 * (kv_boot.c); the telnet console (shell/cmds/net_tty.c) never takes it; the bridge's
 * thread (app/link_bridge.c) never takes it.  Below it sit the NetX DHCP mutex and
 * nx_ip_protection, which is the same direction everything else in this file goes.
 *
 * Boots with no source: nothing addresses this interface until `net dhcp`,
 * `net ip` or the KV boot does.
 */
struct net_core_state net_core_board_state;
const struct net_core_facts net_core_board_facts = {
	.ip          = &nxn_ip,
	.dhcp        = &nxn_dhcp,
	.iface       = 0u,
	.boot_source = NET_CORE_SRC_NONE,
	.why_link    = "the link is down (not associated) -- `wifi connect` first",
	.why_down    = "the host stack did not initialise",
	.worker_stack      = net_worker_stack,
	.worker_stack_size = sizeof net_worker_stack,
};

/*
 * The shared telnet console's state and facts (shell/cmds/net_tty.h, issue #221).
 *
 * The listener's stack and the transmit ring are DTCM (CPU-only: the listener copies
 * out of the ring into an NX_PACKET, and the pool is DTCM too), each a STANDALONE
 * symbol because cmake/check_dtcm_residency.py matches by name -- keep `net_tty_stack`
 * and `net_tty_tx_buf` in its REQUIRED_DTCM.  The receive ring, the per-packet
 * extraction buffer and the state stay plain .bss (AXI-SRAM), as they were.
 *
 * The listener's stack: the whole transmit path runs on it (nx_tcp_socket_send ->
 * IP -> app/nx_link_driver.c -> link_data_send).  2560 B as before #221; the peak
 * measured with #221's code is 588 B (492-612 B before it).  If DTCM gets tight
 * this is a safe 1 KB to give back.
 *
 * Transmit queue depth and retransmit: the DATA channel's budget (nx_net.h), and the
 * first retransmit at NXN_TCP_RTO_MS, which the console's deadline must exceed.
 */
#define NXN_TTY_RX_RING    512u
#define NXN_TTY_EXTRACT    1500u
#define NXN_TTY_TX_RING    4096u
#define NXN_TTY_STACK      2560u
_Static_assert(NET_TTY_TX_DEADLINE > NXN_TCP_RTO_MS,
               "the telnet deadline must cover the first retransmit");
/*
 * The transmit budget (nx_net.h): every frame NetX hands the driver lands in the DATA
 * channel's transmit pool, so the pool must hold the most it can be handed at once.
 * Checked HERE, where the socket's queue depth is configured (the facts below), and not
 * in a diagnostic command: until owhinata/wio-lite-ai#27 it lived in nx_echo.c, where
 * deleting the command would have silently taken the production sizing guarantee with
 * it, and until issue #222 in app/nx_link_driver.c.  If a future socket or a deeper
 * queue breaks it, the build stops rather than the console stalling on a TCP
 * retransmit timer.
 */
_Static_assert(NXN_TCP_SOCKETS_MAX * NXN_TCP_TX_DEPTH + NX_ARP_MAX_QUEUE_DEPTH +
                       NXN_TX_SPARE <= LINK_DATA_TX_BUFS,
               "LINK_DATA_TX_BUFS is too shallow for the NetX transmit budget "
               "(see the transmit budget note in app/nx_net.h)");

static uint8_t net_tty_rx_buf[NXN_TTY_RX_RING];
static uint8_t net_tty_extract[NXN_TTY_EXTRACT];
static uint8_t net_tty_tx_buf[NXN_TTY_TX_RING] DTCM_BSS;
static UCHAR   net_tty_stack[NXN_TTY_STACK] DTCM_BSS __attribute__((aligned(8)));

struct net_tty_state net_tty_board_state;
const struct net_tty_facts net_tty_board_facts = {
	.ip             = &nxn_ip,
	.pool           = &nxn_pool,
	.rx_buf         = net_tty_rx_buf,  .rx_size      = sizeof net_tty_rx_buf,
	.extract_buf    = net_tty_extract, .extract_size = sizeof net_tty_extract,
	.tx_buf         = net_tty_tx_buf,  .tx_size      = sizeof net_tty_tx_buf,
	.stack          = net_tty_stack,   .stack_size   = sizeof net_tty_stack,
	.tx_queue_depth = NXN_TCP_TX_DEPTH,
	.rto_ticks      = NXN_TCP_RTO_MS,
	.rto_retries    = NXN_TCP_RTO_RETRIES,
	.rto_shift      = NXN_TCP_RTO_SHIFT,
	.other_console  = "USB CDC",
};

/* ---- init -------------------------------------------------------------------- */

int nx_net_init(void)
{
	UINT s;

	nx_system_initialize();

	s = nx_packet_pool_create(&nxn_pool, "nx-link", NXN_PAYLOAD, nxn_pool_mem,
	                          sizeof nxn_pool_mem);
	if (s != NX_SUCCESS) {
		LOG_ERR("packet pool create failed (0x%02x)", (unsigned)s);
		return NXN_ERR;
	}

	/* Before nx_ip_create(), which runs NX_LINK_INITIALIZE. */
	nx_link_driver_set_pool(&nxn_pool);

	s = nx_ip_create(&nxn_ip, "nx-link", 0, 0xFFFFFF00UL, &nxn_pool, nx_link_driver,
	                 (VOID *)nxn_ip_stack, sizeof nxn_ip_stack, NXN_IP_PRIORITY);
	if (s != NX_SUCCESS) {
		LOG_ERR("ip create failed (0x%02x)", (unsigned)s);
		return NXN_ERR;
	}

	nx_arp_enable(&nxn_ip, (VOID *)nxn_arp_cache, sizeof nxn_arp_cache);
	nx_icmp_enable(&nxn_ip);
	nx_udp_enable(&nxn_ip);                  /* DHCP                                */
	nx_tcp_enable(&nxn_ip);                  /* compiled in for U4; no sockets yet  */

	if (nx_dhcp_create(&nxn_dhcp, &nxn_ip, "nx-link") == NX_SUCCESS) {
		nx_dhcp_packet_pool_set(&nxn_dhcp, &nxn_pool);
		/*
		 * No nx_dhcp_state_change_notify() here.  It used to log every transition
		 * (2=INIT 3=SELECTING 4=REQUESTING 5=BOUND), which was worth having while the
		 * bridge was new -- a stall at SELECTING meant "no OFFER came back", i.e. the
		 * tap.  Four INF lines per lease is noise now that leases are routine; the
		 * failure it was watching for shows up as `net dhcp` timing out anyway.
		 */
		nxn_dhcp_created = true;
	} else {
		LOG_WRN("dhcp create failed; use `net ip` for a static address");
	}

	/* The shared lock and state, the link worker and the link notification (it
	 * registers NetX's link-status callback, which NetX needs before it even asks
	 * the driver: nx_link_driver.c).  Before the scheduler, as everything here. */
	if (net_core_init(nxn_dhcp_created) != 0)
		return NXN_ERR;
	/* The bridge session's thread and event flags (app/link_bridge.c, issue #222),
	 * where this file used to create them. */
	if (link_bridge_init() != 0)
		return NXN_ERR;

	LOG_INF("NetX Duo ready (pool %u B in DTCM, IP prio %u)",
	        (unsigned)NXN_POOL_BYTES, (unsigned)NXN_IP_PRIORITY);
	return NXN_OK;
}


/* ---- reporting ----------------------------------------------------------------- */

/* Everything nx_net_write_status() prints, sampled before the first line is
 * written (issue #219): the writer may block on a slow console, and a line
 * computed from values read while earlier lines were going out could disagree
 * with them. */
struct nxn_status_sample {
	struct link_bridge_session  ses;     /* state, why, mac, radio, assoc polls */
	bool                        link_up;
	struct nx_link_stats        st;
	ULONG                       pool_avail, pool_total, pool_empty;
	struct link_bridge_modstats ms;
};

/* The longest line is `nx rx`: about 80 characters of text and seven 32-bit
 * counters. */
#define NXN_STATUS_LINE_MAX 192u

/* Format one whole line and write it.  0, or the writer's failure. */
static int nxn_status_line(net_svc_write_fn write, void *ctx, const char *fmt, ...)
{
	char line[NXN_STATUS_LINE_MAX];
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = fmt_vsnformat(line, sizeof line, fmt, ap);
	va_end(ap);
	return (write(ctx, line, (size_t)n) < 0) ? -1 : 0;
}

int nx_net_write_status(net_svc_write_fn write, void *ctx)
{
	struct nxn_status_sample s;
	ULONG waits = 0, invalid = 0;

	/* ---- sample: no lock is held past this block, and nothing is written in it */
	/* The session's values in one call (app/link_bridge.c, issue #222); the driver's
	 * and the pool's are read here, and the module's nine counters last, as one
	 * masked copy.  Nothing printed has to agree with anything sampled in another of
	 * these reads -- only the nine module counters with each other. */
	link_bridge_session_get(&s.ses);
	if (s.ses.state != LINK_BRIDGE_OFF) {
		s.link_up = nx_link_driver_link_up();
		nx_link_driver_get_stats(&s.st);
		s.pool_avail = s.pool_total = s.pool_empty = 0;
		nx_packet_pool_info_get(&nxn_pool, &s.pool_total, &s.pool_avail,
		                        &s.pool_empty, &waits, &invalid);
		/* Accumulated across bridge re-arms -- each one zeroes the module's own copy. */
		link_bridge_modstats_get(&s.ms);
	}

	/* ---- write, from the sample only */
	if (nxn_status_line(write, ctx, "  host stack: %s%s%s\r\n",
	                    link_bridge_state_name(s.ses.state),
	                    (s.ses.why && s.ses.why[0]) ? " -- " : "",
	                    (s.ses.why) ? s.ses.why : "") != 0)
		return -1;
	if (s.ses.state == LINK_BRIDGE_OFF)
		return 0;

	/* `link` is the ASSOCIATION (owhinata/wio-lite-ai#30 B2a), which is what decides whether a frame
	 * can reach the air.  `driver` is rltk_wlan_running() -- the WiFi driver being
	 * started -- which stays up across an AP going away, so it is reported separately
	 * rather than being allowed to masquerade as the link state. */
	if (nxn_status_line(write, ctx,
	                    "  mac: %02x:%02x:%02x:%02x:%02x:%02x, link %s, driver %s\r\n",
	                    s.ses.mac[0], s.ses.mac[1], s.ses.mac[2], s.ses.mac[3],
	                    s.ses.mac[4], s.ses.mac[5],
	                    s.link_up ? "up" : "DOWN",
	                    s.ses.radio_up ? "started" : "stopped") != 0)
		return -1;

	if (nxn_status_line(write, ctx,
	                    "  assoc polls: %lu associated, %lu not, %lu no answer\r\n",
	                    (unsigned long)s.ses.assoc_yes, (unsigned long)s.ses.assoc_no,
	                    (unsigned long)s.ses.assoc_noanswer) != 0)
		return -1;

	if (nxn_status_line(write, ctx,
	                    "  nx rx: %lu frames, %lu B, no-buf %lu, oversize %lu, "
	                    "undersize %lu, unknown %lu, down %lu\r\n",
	                    (unsigned long)s.st.rx_frames, (unsigned long)s.st.rx_bytes,
	                    (unsigned long)s.st.rx_no_buf, (unsigned long)s.st.rx_oversize,
	                    (unsigned long)s.st.rx_undersize,
	                    (unsigned long)s.st.rx_unknown_type,
	                    (unsigned long)s.st.rx_link_down) != 0)
		return -1;
	if (nxn_status_line(write, ctx,
	                    "  nx tx: %lu frames, %lu B, no-buf %lu, oversize %lu, "
	                    "coalesced %lu, down %lu\r\n",
	                    (unsigned long)s.st.tx_frames, (unsigned long)s.st.tx_bytes,
	                    (unsigned long)s.st.tx_no_buf, (unsigned long)s.st.tx_oversize,
	                    (unsigned long)s.st.tx_coalesced,
	                    (unsigned long)s.st.tx_link_down) != 0)
		return -1;

	if (nxn_status_line(write, ctx, "  nx pool: %lu/%lu free, %lu empty requests\r\n",
	                    (unsigned long)s.pool_avail, (unsigned long)s.pool_total,
	                    (unsigned long)s.pool_empty) != 0)
		return -1;

	if (nxn_status_line(write, ctx,
	                    "  mod data rx: %lu frames, %lu B, drops %lu, crc %lu, "
	                    "oversize %lu, gaps %lu%s\r\n",
	                    (unsigned long)s.ms.rx_frames, (unsigned long)s.ms.rx_bytes,
	                    (unsigned long)s.ms.rx_drops, (unsigned long)s.ms.rx_crc,
	                    (unsigned long)s.ms.rx_oversize, (unsigned long)s.ms.rx_gaps,
	                    (s.ms.rx_drops || s.ms.rx_crc || s.ms.rx_oversize ||
	                     s.ms.rx_gaps) ? "  <-- LOSS" : "  (clean)") != 0)
		return -1;
	return nxn_status_line(write, ctx, "  mod data tx: %lu frames, %lu B, drops %lu\r\n",
	                       (unsigned long)s.ms.tx_frames, (unsigned long)s.ms.tx_bytes,
	                       (unsigned long)s.ms.tx_drops);
}
