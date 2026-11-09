/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/*
 * The bridge session over the RTL8720 link (owhinata/wio-lite-ai#23 U3; its own file
 * since issue #222).  See app/link_bridge.h for the design, the failure rule and the
 * threading.
 *
 * ---- what this file holds -----------------------------------------------------
 *
 * The resident thread `nx-net` (priority 13, 3072 B of DTCM stack) and everything it
 * owns: arming the module's L2 bridge, refreshing it, the association hysteresis, the
 * automatic re-association, the teardown and FAILED, the recovery after `wifi
 * on/off/reset`, and the module's DATA counters accumulated across re-arms.  The thread
 * and event-flag names and the log tag `nx` are the ones the board has always shown in
 * `thread` and `dmesg`.
 *
 * ---- the layering (svc/README.md, the net section) ----------------------------
 *
 * This is the link layer.  It calls the RTL8720 side (rtl_link, erpc, link_data,
 * wifi_rpc, wifi_auto, rtl8720) and, for the NetX interface, only its own driver's
 * setters (nx_link_driver_set_link / set_mac / set_speed / reset_stats / link_up, and
 * nx_link_driver_rx handed to link_data_attach()).  It does NOT call or include the
 * layer above: no net_core / net_tty / net_svc, no nx_net.h, no DHCP client, no NetX
 * application API.  app/nx_net.c, which creates the NetX objects, calls down into it
 * (link_bridge_init(), link_bridge_session_get(), link_bridge_modstats_get()); board
 * commands call it directly.
 */
#include "link_bridge.h"

#include <string.h>

#include "tx_api.h"
#include "stm32h7xx_hal.h"   /* __get_PRIMASK / __disable_irq / __set_PRIMASK */

#include "erpc.h"
#include "link_data.h"
#define LOG_TAG "nx"
#include "log.h"
#include "nx_link_driver.h"
#include "rtl_link.h"
#include "rtl8720.h"
#include "wifi_auto.h"
#include "wifi_rpc.h"
#include "mem_sections.h"  /* DTCM_BSS: CPU-only data out of AXI-SRAM (issue #46) */

/* ---- tunables --------------------------------------------------------------- */

#define NXN_OWNER_PRIORITY 13u
/*
 * 3072, not 2048: owhinata/wio-lite-ai#32 puts a new call chain on this thread -- nxn_refresh() ->
 * nxn_auto_reconnect() -> wifi_auto_attempt() -> wifi_rpc_connect() (which builds its
 * request in a 160-byte local) -> erpc_call_ex().
 *
 * Measured on board #2 after 13 re-association attempts: `thread` reports a peak of
 * 860 B, so 2048 would have been ample and would have returned 1 kB of AXI-SRAM.  It is
 * NOT taken back, for the same reason the packet pool above was not shrunk: the region is
 * 41 % free and nothing is asking for that kilobyte, while the high-water mark is a
 * best-effort measurement of the paths that HAPPENED to run, and the thread it protects
 * is the one that owns the network.
 */
#define NXN_OWNER_STACK    3072u

/*
 * The module takes its own tap out this long after the last DATA_CFG (cap 60 s,
 * fw/rtl8720 WIO_ETH_HOLD_MAX_MS), so the interface re-arms well inside it.  A refresh
 * that cannot get the coarse mutex is skipped, not failed, and 30 s absorbs about three
 * of those.
 */
#define NXN_HOLD_MS        30000u
/* The module's ceiling (fw/rtl8720 WIO_ETH_HOLD_MAX_MS).  Used only by
 * link_bridge_hold_extend() to cover a long eRPC flow that cannot refresh. */
#define NXN_HOLD_MAX_MS    60000u
#define NXN_REFRESH_MS     8000u
/*
 * How long link_bridge_link_taken() waits for the owner to act on a revoked link
 * (owhinata/wio-lite-ai#41).  The owner only has to reach the generation check at the top of
 * its UP branch and run nxn_link_revoked(), which issues no eRPC, takes no link and no
 * lock (the DHCP stop is the shared link worker's since issue #220) -- so this is a
 * scheduler turn, not a transaction, and half a second is ample slack.  It is
 * deliberately NOT sized for the module: nothing here talks to it.
 */
#define NXN_TAKEN_WAIT_MS  500u
#define NXN_TAKEN_POLL_MS  5u

#define NXN_CTRL_TMO_MS    500u
#define NXN_DCFG_TMO_MS    2500u      /* CFG(off) delays its ack until the module's
                                       * DATA writer is idle -- as `link` does        */
#define NXN_RPC_TMO_MS     5000u
/*
 * How long an owhinata/wio-lite-ai#32 re-association may block.  It must EXCEED the module's own
 * join timeout, which is 20 s (rtw_down_timeout_sema(join_sema, 20000) in lib_arduino.a's
 * wifi_connect): give up first and the module is still holding its serial mutex when the
 * next call goes out, and its answer arrives as a stale frame we have to drop.
 */
#define NXN_CONNECT_TMO_MS 22000u
#define NXN_CLAIM_MS       RTL_LINK_CLAIM_WAIT_MS
#define NXN_ARM_CLAIM_MS   20000u     /* how long ARM waits for the coarse mutex      */

#define NXN_OFF_RETRIES    3
#define NXN_SETTLE_TRIES   50         /* x 20 ms = 1 s, as link_data_settle()         */
#define NXN_REFRESH_FAILS  2          /* consecutive CFG failures before STOP         */

/*
 * Consecutive association samples that must be non-positive before the link is declared
 * down (owhinata/wio-lite-ai#30 B2a).  Two at NXN_REFRESH_MS = up to ~16 s of latency on a real
 * disconnect, which is the price of not dropping every socket on one lost round trip.
 */
#define NXN_LINK_DOWN_SAMPLES 2

#define NXN_DSTAT_WORDS    12u        /* LINK_DATA_STATS reply                        */
#define NXN_ETH_REPLY_MIN  (8u + ERPC_ETH_STAT_WORDS * 4u)

#define NXN_EVT_CMD        0x1u

/* Firmware generation that first has the L2 bridge (2.1.3+wio-n7).  It also implies the
 * LINK CTRL channel that NXN_LINK_RATE needs, which arrived in n5. */
#define NXN_MIN_MODULE_GEN 7u

/* The rate the interface runs the link at.  Both ends' ceiling (the module's UART is
 * documented 110..6000000, USART1 reaches 8.59 M), and worth ~1.6x on echoed TCP. */
#define NXN_LINK_RATE      6000000u

/* ---- state ------------------------------------------------------------------ */

/* The thread and its event flags exist (link_bridge_init() succeeded). */
static bool nxn_ready;

/* Owner thread. */
static TX_THREAD             nxn_thread;
static TX_EVENT_FLAGS_GROUP  nxn_evt;
static UCHAR                 nxn_stack[NXN_OWNER_STACK] DTCM_BSS __attribute__((aligned(8)));

/*
 * State is written by the owner thread and, for the ARMING/STOPPING transitions, by the
 * requesting CLI thread -- deliberately, so that a command that checks the state (the
 * link diagnostics refuse unless it is OFF) sees the change the moment the command is
 * typed rather than when the owner gets around to it.
 */
static volatile enum link_bridge_state nxn_state = LINK_BRIDGE_OFF;
static volatile uint8_t  nxn_req_stop;

/* Owner-thread-private. */
static uint32_t nxn_uart_gen;
static uint8_t  nxn_mac[6];
static uint8_t  nxn_radio_up;

/* Association tracking for the link state (owhinata/wio-lite-ai#30 B2a).  Owner thread
 * only.  The stale mark it used to keep is the shared one now (issue #220). */
static uint8_t  nxn_link_down_run;
/* Association poll tally, so a spurious link-down can be told apart from a real one:
 * "the module said no" and "the module did not answer" are different failures and only
 * the first is the network's fault. */
static uint32_t nxn_assoc_yes, nxn_assoc_no, nxn_assoc_noanswer;
static uint8_t  nxn_reply[ERPC_CTRL_MAX];
static struct link_bridge_modstats nxn_mod;

/* Why the interface last went down or refused, for `net info`. */
static const char *nxn_why = "";

/* The dirty latch behind FAILED: cleared when the module is actually power-cycled. */
static uint32_t nxn_failed_at;

/*
 * Consecutive failed bridge re-arms, SESSION state and not a static local inside the
 * refresh: a failure carried over from a previous session would make the next one give
 * up after a single bad refresh instead of two consecutive ones.  Reset wherever a
 * session begins or ends.
 */
static int nxn_refresh_fails;

/* ---- small helpers ---------------------------------------------------------- */

static void nxn_opts(struct wifi_rpc_opts *o, uint32_t ms)
{
	memset(o, 0, sizeof *o);
	o->timeout_ms = ms;
	/* No abort hook: this is not running on a console thread, so there is nobody to
	 * press Ctrl+C, and an aborted request would leave the link dirty. */
}

/* ---- CTRL exchanges (owner thread, coarse mutex held) ----------------------- */

static int nxn_data_cfg(uint8_t mode, uint32_t ms)
{
	struct erpc_diag diag = {0};
	uint8_t req[12];
	int n;

	req[0] = mode;
	req[1] = 0u;
	req[2] = 0u;
	req[3] = 0u;
	erpc_put_u32le(req + 4, ms);
	erpc_put_u32le(req + 8, ERPC_CTRL_DATA_MAGIC);
	n = erpc_ctrl_call(ERPC_CTRL_DATA_CFG, req, (uint16_t)sizeof req, nxn_reply,
	                   (uint16_t)sizeof nxn_reply, NXN_DCFG_TMO_MS, &diag);
	if (n < 0) {
		LOG_ERR("DATA_CFG(%u) rc %d status %u", (unsigned)mode, n,
		        (unsigned)((n == -3) ? erpc_ctrl_last_status() : 0u));
		return -1;
	}
	return 0;
}

/* Read the module's DATA counters into @st.  Returns 0 on success. */
static int nxn_data_stats(uint32_t st[NXN_DSTAT_WORDS])
{
	struct erpc_diag diag = {0};
	unsigned i;
	int n = erpc_ctrl_call(ERPC_CTRL_DATA_STATS, NULL, 0u, nxn_reply,
	                       (uint16_t)sizeof nxn_reply, NXN_CTRL_TMO_MS, &diag);

	if (n < (int)(NXN_DSTAT_WORDS * 4u))
		return -1;
	for (i = 0u; i < NXN_DSTAT_WORDS; i++)
		st[i] = erpc_get_u32le(nxn_reply + i * 4u);
	return 0;
}

/*
 * Fold the module's DATA counters into our running totals.  Every non-OFF DATA_CFG zeroes
 * them on the module, so this must happen BEFORE each re-arm or a long-lived interface
 * would keep destroying its own loss ledger.  Words 9/10 are queue depth and pool
 * in-use -- instantaneous, not cumulative, so they are not accumulated here.
 */
static void nxn_mod_accumulate(const uint32_t st[NXN_DSTAT_WORDS])
{
	/*
	 * Interrupts masked for the whole update, and likewise for the read in
	 * link_bridge_modstats_get(), so a reader can never catch the ledger half-folded.
	 * Individual aligned words could not tear on this core anyway -- what this buys is
	 * that the NINE of them are consistent WITH EACH OTHER, which is the only reason
	 * anyone reads them: "frames vs drops" is a ratio, and a snapshot spliced from two
	 * different moments quietly lies about it.  A few dozen cycles, once per refresh.
	 */
	uint32_t pm = __get_PRIMASK();

	__disable_irq();
	nxn_mod.rx_frames   += st[0];
	nxn_mod.rx_bytes    += st[1];
	nxn_mod.rx_drops    += st[2];
	nxn_mod.rx_crc      += st[3];
	nxn_mod.rx_oversize += st[4];
	nxn_mod.rx_gaps     += st[5];
	nxn_mod.tx_frames   += st[6];
	nxn_mod.tx_bytes    += st[7];
	nxn_mod.tx_drops    += st[8];
	__set_PRIMASK(pm);
}

static int nxn_eth_info(uint8_t mac[6], uint8_t *flags)
{
	struct erpc_diag diag = {0};
	int n = erpc_ctrl_call(ERPC_CTRL_ETH_INFO, NULL, 0u, nxn_reply,
	                       (uint16_t)sizeof nxn_reply, NXN_CTRL_TMO_MS, &diag);

	if (n < (int)NXN_ETH_REPLY_MIN)
		return -1;
	memcpy(mac, nxn_reply, 6);
	*flags = nxn_reply[6];
	return 0;
}

/* ---- the owner thread -------------------------------------------------------- */

/*
 * Wait until BOTH ends are quiet.  This is app/link_data.h's detach ordering rule, and it
 * is a state test rather than a delay because there is no defensible number of
 * milliseconds to wait: the module's side is proved by the CFG(off) acknowledgement plus
 * its own queue and pool counters, the host's by erpc_data_quiescent().
 *
 * erpc_data_quiescent(), NOT erpc_link_quiescent(): the stronger question additionally
 * requires that no eRPC request be outstanding, and a telnet console keeps a blocking
 * accept outstanding by design, so it could never succeed with one armed (owhinata/wio-lite-ai#23
 * U1).
 */
static int nxn_settle(void)
{
	uint32_t st[NXN_DSTAT_WORDS];
	int tries;

	for (tries = 0; tries < NXN_SETTLE_TRIES; tries++) {
		if (erpc_data_quiescent() && nxn_data_stats(st) == 0 &&
		    st[9] == 0u && st[10] == 0u) {
			nxn_mod_accumulate(st);
			return 0;
		}
		tx_thread_sleep(20u);
	}
	return -1;
}

/*
 * The teardown could not prove the module is silent.  Do NOT detach yet: instead take the
 * host's byte stream away entirely.  app/link_data.h names force-quiesce as one of the
 * teardowns "that takes the module down with it, where nothing survives to be
 * desynchronised" -- it drops the reference count to zero, runs erpc_link_closed() (which
 * reclaims every DATA buffer) and closes the UART, so after it there is no stream left to
 * lose synchronisation with.
 *
 * It does NOT power-cycle the module, so the tap may stay in until the module's own
 * watchdog fires (up to NXN_HOLD_MS).  `wifi reset` ends it immediately, which is why
 * that is what the message says and why `wifi on/off/reset` are never guarded.
 *
 * Called with the coarse mutex HELD; force-quiesce requires that and does not release it.
 */
static void nxn_enter_failed(const char *why)
{
	rtl_link_force_quiesce();
	link_data_detach();
	nxn_uart_gen = 0u;
	nxn_failed_at = rtl_link_quiesce_gen();
	nxn_why = why;
	nxn_state = LINK_BRIDGE_FAILED;
	LOG_ERR("%s -- the host stack could not be shut down cleanly; run `wifi reset`",
	        why);
}

/*
 * End the session.  Every exit passes through exactly one rtl_link_unclaim(), tracked by
 * @claimed rather than by remembering to write it on each branch.
 */
static void nxn_stop(const char *why)
{
	int claimed = 0;
	int tries;

	nxn_state = LINK_BRIDGE_STOPPING;
	nxn_refresh_fails = 0;

	/*
	 * Silence our side first (issue #218).
	 *
	 * Closing the driver's gate is the whole host-side proof.  nx_link_driver_set_link(0)
	 * clears link_up and fences the DATA channel's send generation in one masked step,
	 * so from here on no NetX frame can be queued on the channel: a new send fails the
	 * gate, and a send that had already passed it -- on whichever thread called into
	 * NetX, preempted for however long -- holds a stale generation and is refused when
	 * it takes a buffer or when it queues the filled one (app/link_data.h).  Frames
	 * queued before the fence are drained by nxn_settle() below.  Only this thread
	 * opens the gate again (nxn_arm(), the UP-state refresh), and neither runs while
	 * STOPPING.  So nxn_settle()'s host-side reading is a stable fact rather than a
	 * snapshot; its module-side reading rests on DATA_CFG(off) exactly as before.
	 *
	 * That is why the telnet console is not stopped here at all: since issue #221 it
	 * is resident and keeps listening, and its session (if any) is ended by its own
	 * listener once the shared link worker, woken by the down notice of set_link(0),
	 * passes the fall on.  Its FIN or a
	 * retransmit may still reach the driver, but none of it reaches the channel.
	 * (Until #218 the console was stopped synchronously FIRST and an unprovable stop
	 * went FAILED; that proved silence for one producer only.)
	 */
	nx_link_driver_set_link(0);
	/* The DHCP client is stopped by the shared link worker, which the down notice
	 * of set_link(0) wakes (issue #220).  It may run after the module is told
	 * DATA_CFG(off) below; a DHCP frame it would send meanwhile is refused at the
	 * gate just closed. */

	if (rtl_link_claim(NXN_ARM_CLAIM_MS) != 0) {
		/* Nobody gave up the mutex in twenty seconds.  We cannot even ask the module
		 * to stop, let alone prove it did.  There is no claim-free way to reach
		 * force-quiesce safely, so say so and leave the tap to the watchdog. */
		nxn_why = "the link stayed busy";
		nxn_failed_at = rtl_link_quiesce_gen();
		nxn_state = LINK_BRIDGE_FAILED;
		LOG_ERR("could not take the link to shut down; run `wifi reset`");
		return;
	}
	claimed = 1;

	for (tries = 0; tries < NXN_OFF_RETRIES; tries++) {
		if (nxn_data_cfg(ERPC_DATA_MODE_OFF, 0u) == 0)
			break;
	}
	if (tries == NXN_OFF_RETRIES) {
		nxn_enter_failed("the module never acknowledged DATA_CFG(off)");
		goto out;
	}

	if (nxn_settle() != 0) {
		nxn_enter_failed("the DATA channel did not go quiet");
		goto out;
	}

	link_data_detach();
	rtl_link_uart_unref_gen(nxn_uart_gen);
	nxn_uart_gen = 0u;
	nxn_why = why;
	nxn_state = LINK_BRIDGE_OFF;
	LOG_INF("host stack down (%s)", why);

out:
	if (claimed)
		rtl_link_unclaim();
}

/*
 * Bring the interface up.  The prologue order is `wifi link arp`'s (shell/cmds/cmd_wifi_link.c) and
 * every step is in that position for a reason recorded there.
 */
static void nxn_arm(void)
{
	struct wifi_rpc_opts o;
	uint32_t waited = 0u;
	int32_t result = 0;
	uint8_t flags = 0u;
	int claimed = 0;
	int armed = 0;                   /* DATA_CFG(BRIDGE) has been ISSUED */
	int attached = 0;

	nxn_refresh_fails = 0;

	/*
	 * No wait for the telnet console (issue #221): it is resident and its listening
	 * socket stays across sessions of the interface.  A session it still has from
	 * before the fall is ended by the link worker's notice, and what it transmits
	 * while the gate is shut is dropped there.
	 */
	if (!rtl8720_powered()) {
		nxn_why = "the RTL8720 is powered off";
		goto refuse;
	}

	while (rtl_link_claim(NXN_CLAIM_MS) != 0) {
		waited += NXN_CLAIM_MS;
		if (nxn_req_stop || waited >= NXN_ARM_CLAIM_MS) {
			nxn_why = "the link stayed busy";
			goto refuse;
		}
	}
	claimed = 1;

	if (rtl_link_uart_ref(RTL8720_UART_AT, rtl_link_erpc_baud()) != 0) {
		nxn_why = "the link UART is held at a different rate";
		goto refuse;
	}
	nxn_uart_gen = rtl_link_uart_gen();

	if (erpc_module_gen() < NXN_MIN_MODULE_GEN) {
		nxn_why = "the module firmware has no L2 bridge (needs 2.1.3+wio-n7; "
		          "run `wifi ver`)";
		goto unref;
	}

	nxn_opts(&o, NXN_RPC_TMO_MS);
	/* The module answers WIFI_RPC_OK (which is 0) for "associated" -- a result code, not
	 * a boolean.  Testing it for truth gets the answer exactly backwards, which is what
	 * the first board run did: `wifi status` said connected and this refused. */
	if (wifi_rpc_is_connected(&o, &result) != 0 || result != WIFI_RPC_OK) {
		nxn_why = "the module is not associated (run `wifi connect <ssid> ...`)";
		goto unref;
	}

	/*
	 * Take the link to its full rate before anything rides on it.
	 *
	 * Every module boots at 2 Mbaud -- that is its firmware, and this increment does not
	 * reflash it -- so 6 M can only ever be reached by asking, once per module boot.
	 * `net up` is the right place to ask: it is the command that says "I want throughput
	 * on this link", and owhinata/wio-lite-ai#23 measured the difference at 305 vs 500 kB/s of echoed
	 * TCP.
	 *
	 * [!] This DELIBERATELY REVERSES issue #23 U0-3's "the rate change is exclusive and
	 * manual", and the reason it was manual has not gone away: a rate change is the one
	 * operation here that can cost the link, and its guaranteed recovery is `wifi reset`.
	 * What changed is the evidence -- U1 through U4-2 have driven 6 Mbaud through
	 * full-duplex saturation, five 512 kB TCP transfers and the telnet console without a
	 * single rate-change failure -- and the cost of being wrong, which is a power cycle
	 * of a companion chip, not a flash operation.
	 *
	 * A refusal is NOT fatal: 2 Mbaud carries everything here, just slower, so a module
	 * that will not switch gets a warning and the bridge continues.  Only RTL_RATE_DEAD
	 * is terminal, and it is terminal for the whole link rather than for us.
	 */
	if (rtl_link_erpc_baud() != NXN_LINK_RATE) {
		int rc = rtl_link_set_rate(NXN_LINK_RATE);

		if (rc == RTL_RATE_DEAD) {
			nxn_why = "the link died changing rate -- run `wifi reset`";
			goto unref;
		}
		if (rc != RTL_RATE_OK)
			LOG_WRN("staying at %lu baud: the module would not change rate",
			        (unsigned long)rtl_link_erpc_baud());
		else
			LOG_INF("link raised to %lu baud", (unsigned long)NXN_LINK_RATE);
	}

	/* Last point at which giving up is free: nothing has been told to the module and
	 * the DATA channel has no consumer yet. */
	if (nxn_req_stop) {
		nxn_req_stop = 0u;
		nxn_why = "cancelled";
		goto unref;
	}
	/*
	 * Stop the module's DHCP client.  A TRANSPORT failure refuses the whole session --
	 * "we do not know whether it stopped" is not good enough when the bridge is about
	 * to zero the netif address underneath it.  A non-zero module RESULT is different:
	 * it was executed, and "it was not running" is the state we want.
	 */
	if (wifi_rpc_dhcpc_stop(&o, 0u, &result) != 0) {
		nxn_why = "could not stop the module's DHCP client";
		goto unref;
	}

	/* Attach BEFORE the bridge is enabled, so no frame can arrive with no consumer. */
	nx_link_driver_reset_stats();
	memset(&nxn_mod, 0, sizeof nxn_mod);
	if (link_data_attach(nx_link_driver_rx, NULL) != 0) {
		nxn_why = "the link's DATA channel already has an owner "
		          "(a `link` command is running)";
		goto unref;
	}
	attached = 1;

	if (nxn_req_stop) {
		/* Still free: attached, but the module has not been asked to bridge, so the
		 * detach in `unref:` cannot desynchronise anything. */
		nxn_req_stop = 0u;
		nxn_why = "cancelled";
		goto unref;
	}

	nxn_link_down_run = 0u;
	nxn_assoc_yes = nxn_assoc_no = nxn_assoc_noanswer = 0u;
	armed = 1;                       /* from here on, only nxn_stop() may end this */
	if (nxn_data_cfg(ERPC_DATA_MODE_BRIDGE, NXN_HOLD_MS) != 0) {
		nxn_why = "the module refused to bridge (see the log; `wifi connect` first?)";
		goto stop;
	}

	if (nxn_eth_info(nxn_mac, &flags) != 0) {
		nxn_why = "the module did not report its MAC";
		goto stop;
	}
	nxn_radio_up = (flags & ERPC_ETH_F_RUNNING) ? 1u : 0u;
	if (!nxn_radio_up) {
		nxn_why = "the module's radio is down";
		goto stop;
	}

	rtl_link_unclaim();
	claimed = 0;

	/*
	 * The NetX calls go here, AFTER the coarse mutex is released: they take
	 * nx_ip_protection, and the owner must never block on a NetX mutex while holding
	 * the lock every shell command needs.
	 */
	if (nx_link_driver_set_mac(nxn_mac) != 0) {
		nxn_why = "NetX refused the module's MAC";
		goto stop;
	}
	nx_link_driver_set_speed(rtl_link_erpc_baud());
	nx_link_driver_set_link(1);

	nxn_why = "";
	nxn_state = LINK_BRIDGE_UP;
	/* A stop that arrived while the bridge was going in is honoured by the next pass of
	 * the owner loop, now that the state it has to unwind is fully established -- that
	 * is the whole point of only letting nxn_stop() end an armed session. */
	LOG_INF("host stack up, mac %02x:%02x:%02x:%02x:%02x:%02x",
	        nxn_mac[0], nxn_mac[1], nxn_mac[2], nxn_mac[3], nxn_mac[4], nxn_mac[5]);
	return;

stop:
	/* The module has been told to bridge (whatever it answered), so the only honest
	 * way out is the full teardown. */
	if (claimed) {
		rtl_link_unclaim();
		claimed = 0;
	}
	nxn_stop(nxn_why);
	return;

unref:
	if (attached && !armed)
		link_data_detach();          /* safe: the module was never told to send */
	rtl_link_uart_unref_gen(nxn_uart_gen);
	nxn_uart_gen = 0u;
refuse:
	if (claimed)
		rtl_link_unclaim();
	nxn_state = LINK_BRIDGE_OFF;
	LOG_WRN("cannot bring the host stack up -- %s", nxn_why);
}

/*
 * Turn an association sample into the interface's link state (owhinata/wio-lite-ai#30 B2a).
 *
 * @assoc: 1 associated, 0 not, -1 no answer (transport failure / refresh skipped).
 *
 * HYSTERESIS, and only in the down direction: a single missed round trip must not tear
 * a TCP session down, so it takes NXN_LINK_DOWN_SAMPLES consecutive non-positive samples
 * (8 s apart) to declare the link down.  Coming back needs one good sample -- being late
 * to say "up" costs nothing, being early to say "down" costs every socket.
 *
 * The write itself goes through nx_link_driver_set_link(), which only flags the change
 * and lets the IP thread run the shared status notification; nx_interface_link_up is
 * never written from this thread.  Nothing here starts or stops DHCP or marks a lease
 * stale: the shared link worker does, woken by that notification (issue #220).
 */
static void nxn_publish_link(int assoc)
{
	int up = nx_link_driver_link_up();

	if (assoc > 0)      nxn_assoc_yes++;
	else if (assoc == 0) nxn_assoc_no++;
	else                 nxn_assoc_noanswer++;

	if (assoc > 0) {
		nxn_link_down_run = 0u;
		if (!up) {
			nx_link_driver_set_link(1);
			LOG_INF("link up (associated)");
		}
		return;
	}
	if (assoc < 0 && !up)
		return;                      /* no answer and already down: nothing to say */

	if (nxn_link_down_run < 0xFFu)
		nxn_link_down_run++;
	if (!up || nxn_link_down_run < NXN_LINK_DOWN_SAMPLES)
		return;

	nx_link_driver_set_link(0);
	LOG_WRN("link down (%s)", (assoc == 0) ? "not associated" : "module did not answer");
}

/*
 * Abort hook for an owhinata/wio-lite-ai#32 re-association.  Polled by erpc_call_ex() about once a
 * millisecond on this thread.
 *
 * Disarming IS the abort -- see app/wifi_auto.h -- so there is no separate cancel flag and
 * therefore no window in which a command has asked us to stop and an attempt starts anyway.
 * The other two terms are the reasons the owner itself must stop: a `net down` waiting to
 * be acted on, and the link having been taken away from under us.
 */
static int nxn_auto_abort(void *ctx)
{
	(void)ctx;
	return (!wifi_auto_armed() || nxn_req_stop ||
	        rtl_link_uart_gen() != nxn_uart_gen) ? 1 : 0;
}

/*
 * Re-associate after the module has told us it is not associated (owhinata/wio-lite-ai#32).  Called
 * from nxn_refresh() with the coarse mutex held; returns a fresh association sample for
 * nxn_publish_link(): 1 associated, 0 not, -1 no answer.
 *
 * THE ABORT PATH IS THE DELICATE PART.  erpc_call_ex()'s abort releases the HOST's slot and
 * nothing else -- rpc_wifi_connect is outside the module firmware's concurrent allow-list
 * (fw/rtl8720 patch 0003), so the module stays inside wifi_connect() holding its serial
 * mutex for up to its own 20 s timeout.  An aborted attempt therefore issues NO further
 * eRPC and returns immediately, because the whole point of being abortable is to hand the
 * coarse mutex back to the `wifi off` / `wifi reset` that asked for it.  Leaving the hold
 * at its 60 s ceiling on the way out is safe: it is a watchdog bound, so a wider one only
 * means the module waits longer before dropping a tap that the next refresh re-arms anyway.
 */
static int nxn_auto_reconnect(void)
{
	struct wifi_rpc_opts o;
	uint32_t st[NXN_DSTAT_WORDS];
	int32_t connected = -1;
	int rc;

	/*
	 * EVERY non-OFF DATA_CFG zeroes the module's DATA counters, and this function issues
	 * two of them -- so each one has to be preceded by taking the ledger, exactly as
	 * nxn_refresh() does before its own re-arm.  Without this, an attempt would destroy
	 * the loss evidence for the window it spans, which is the one window where knowing
	 * whether frames were lost matters most.
	 */
	if (nxn_data_stats(st) == 0)
		nxn_mod_accumulate(st);
	/* Hold the module's tap open across the attempt: we are the thread that would
	 * otherwise be refreshing it, and we are about to block for up to 22 s. */
	if (nxn_data_cfg(ERPC_DATA_MODE_BRIDGE, NXN_HOLD_MAX_MS) != 0)
		return 0;                    /* no hold, no attempt -- and still not associated */

	nxn_opts(&o, NXN_CONNECT_TMO_MS);
	o.should_abort = nxn_auto_abort;
	rc = wifi_auto_attempt(&o);
	if (rc == -4)
		return 0;                    /* aborted: touch nothing, let the caller unclaim */

	/* Back to the ordinary hold (ledger first, as above).  A failure here is the same
	 * event nxn_refresh_fails exists for -- the module no longer acknowledging the
	 * bridge -- so it counts, unlike the extension above, which we merely chose to ask
	 * for and whose failure only costs us this attempt. */
	if (nxn_data_stats(st) == 0)
		nxn_mod_accumulate(st);
	if (nxn_data_cfg(ERPC_DATA_MODE_BRIDGE, NXN_HOLD_MS) != 0)
		nxn_refresh_fails++;

	if (rc != 0)
		return 0;                    /* a failed join needs no second opinion */

	/* Only a reported success is worth another round trip, and by now the module is
	 * idle: our 22 s exceeds the 20 s it gives itself. */
	nxn_opts(&o, NXN_RPC_TMO_MS);
	if (wifi_rpc_is_connected(&o, &connected) != 0)
		return -1;
	if (connected != WIFI_RPC_OK)
		return 0;
	/* Not marked stale (issue #220, decided with the user): a re-association inside one
	 * refresh never took the link down, so NetX and the lease saw nothing. */
	return 1;
}

/*
 * Feed the module's watchdog, and take its loss ledger before doing so: every non-OFF
 * DATA_CFG zeroes the module's DATA counters.
 */
static void nxn_refresh(void)
{
	uint32_t st[NXN_DSTAT_WORDS];
	int assoc = -1;                  /* -1 = not asked / no answer */

	if (rtl_link_claim(NXN_CLAIM_MS) != 0)
		return;                      /* skipped, not failed -- 30 s absorbs a few */

	if (nxn_data_stats(st) == 0)
		nxn_mod_accumulate(st);

	if (nxn_data_cfg(ERPC_DATA_MODE_BRIDGE, NXN_HOLD_MS) != 0) {
		nxn_refresh_fails++;
	} else {
		uint8_t mac[6], flags = 0u;
		struct wifi_rpc_opts o;
		int32_t connected = -1;

		nxn_refresh_fails = 0;
		if (nxn_eth_info(mac, &flags) == 0) {
			memcpy(nxn_mac, mac, sizeof mac);
			nxn_radio_up = (flags & ERPC_ETH_F_RUNNING) ? 1u : 0u;
		}
		/*
		 * Association, piggy-backed on the refresh we are already here for (owhinata/wio-lite-ai#30
		 * B2a).  It is the ONLY reliable source: the ETH_INFO flag above is
		 * rltk_wlan_running(), which says the WiFi DRIVER is started -- not that it is
		 * joined to an AP -- so using it as the link signal would report "up" while the
		 * AP is gone.  One extra eRPC round trip per NXN_REFRESH_MS, on a thread that
		 * already holds the coarse mutex, so the lock order (coarse -> erpc) is the
		 * established one and nothing new can deadlock.
		 */
		nxn_opts(&o, NXN_RPC_TMO_MS);
		if (wifi_rpc_is_connected(&o, &connected) == 0)
			assoc = (connected == WIFI_RPC_OK) ? 1 : 0;

		/*
		 * Automatic re-association (owhinata/wio-lite-ai#32), on a DEFINITE "not associated" only.
		 * assoc < 0 is a transport failure -- the module did not answer -- which is
		 * not the network's fault and must never make us re-join.
		 *
		 * This runs BEFORE the link state is published, and that is the point: the
		 * declaration of link-down needs NXN_LINK_DOWN_SAMPLES consecutive samples,
		 * so a re-association that lands inside one refresh means NetX never sees the
		 * link drop at all and no socket -- telnet console included -- is torn down.
		 */
		if (assoc == 0 && wifi_auto_should_try())
			assoc = nxn_auto_reconnect();
	}
	rtl_link_unclaim();

	/* Outside the coarse mutex: publishing touches NetX, and the owner must never block
	 * on a NetX mutex while holding the lock every shell command needs. */
	nxn_publish_link(assoc);

	if (nxn_refresh_fails >= NXN_REFRESH_FAILS) {
		nxn_refresh_fails = 0;
		/* Not a bare "go down": the module may still be bridged, so the same proof
		 * obligation applies and only nxn_stop() discharges it. */
		nxn_stop("the module stopped acknowledging the bridge");
	}
}

/*
 * `wifi on/off/reset` force-quiesced the link and revoked our reference.  That is the
 * module-down exception in person, so it does not go through nxn_stop(): the UART is
 * already closed and erpc_link_closed() has already reclaimed every DATA buffer, so
 * detaching cannot desynchronise anything.  rtl_link_uart_unref_gen() is a no-op once the
 * generation has moved, which is what stops us decrementing somebody else's reference.
 */
static void nxn_link_revoked(void)
{
	nx_link_driver_set_link(0);       /* the shared worker stops DHCP (issue #220) */
	link_data_detach();
	rtl_link_uart_unref_gen(nxn_uart_gen);
	nxn_uart_gen = 0u;
	nxn_why = "the RTL8720 link was taken away (wifi on/off/reset)";
	nxn_state = LINK_BRIDGE_OFF;
	LOG_WRN("%s", nxn_why);
}

static void nxn_entry(ULONG arg)
{
	ULONG flags;

	(void)arg;

	for (;;) {
		/*
		 * Consume the stop request only in a state that can act on it.  Clearing it
		 * unconditionally used to lose a `net down` typed while ARMING: the owner
		 * wiped the flag and then carried on bringing the interface up, so the
		 * request evaporated.  During ARMING the flag is left standing -- nxn_arm()
		 * checks it at its own safe points, and whatever it does not catch is acted
		 * on by the next pass through here.
		 */
		if (nxn_req_stop && (nxn_state == LINK_BRIDGE_UP || nxn_state == LINK_BRIDGE_OFF ||
		                     nxn_state == LINK_BRIDGE_FAILED)) {
			int was_up = (nxn_state == LINK_BRIDGE_UP);

			nxn_req_stop = 0u;
			if (was_up)
				nxn_stop("requested");
		}

		switch (nxn_state) {
		case LINK_BRIDGE_ARMING:
			nxn_arm();
			break;

		case LINK_BRIDGE_UP:
			/*
			 * A moved generation means wifi on/off/reset force-quiesced the link
			 * and revoked our reference.  Check it before the refresh: there is
			 * no point asking a module that has been power-cycled.
			 */
			if (rtl_link_uart_gen() != nxn_uart_gen) {
				nxn_link_revoked();
				break;
			}
			nxn_refresh();
			if (nxn_state == LINK_BRIDGE_UP)
				(void)tx_event_flags_get(&nxn_evt, NXN_EVT_CMD, TX_OR_CLEAR,
				                         &flags, NXN_REFRESH_MS);
			break;

		case LINK_BRIDGE_FAILED:
			/*
			 * FAILED still holds things -- possibly the DATA consumer and a UART
			 * reference -- because it could not prove they were safe to give
			 * back.  `wifi reset` is what makes them safe, and it shows up here
			 * as the quiesce generation moving.  Without this the latch would
			 * never clear and `wifi link arp` would be refused forever.
			 */
			if (rtl_link_quiesce_gen() != nxn_failed_at) {
				link_data_detach();
				rtl_link_uart_unref_gen(nxn_uart_gen);
				nxn_uart_gen = 0u;
				nxn_why = "recovered by a module power cycle";
				nxn_state = LINK_BRIDGE_OFF;
				LOG_INF("%s", nxn_why);
				break;
			}
			(void)tx_event_flags_get(&nxn_evt, NXN_EVT_CMD, TX_OR_CLEAR, &flags,
			                         500u);
			break;

		default:
			(void)tx_event_flags_get(&nxn_evt, NXN_EVT_CMD, TX_OR_CLEAR, &flags,
			                         TX_WAIT_FOREVER);
			break;
		}
	}
}

/* ---- init -------------------------------------------------------------------- */

int link_bridge_init(void)
{
	if (tx_event_flags_create(&nxn_evt, "nx-net") != TX_SUCCESS)
		return -1;
	if (tx_thread_create(&nxn_thread, "nx-net", nxn_entry, 0, nxn_stack,
	                     sizeof nxn_stack, NXN_OWNER_PRIORITY, NXN_OWNER_PRIORITY,
	                     TX_NO_TIME_SLICE, TX_AUTO_START) != TX_SUCCESS)
		return -1;

	nxn_ready = true;
	return 0;
}

/* ---- control API -------------------------------------------------------------- */

bool link_bridge_is_up(void)
{
	return nxn_state == LINK_BRIDGE_UP;
}

enum link_bridge_state link_bridge_state(void)
{
	return nxn_state;
}

int link_bridge_up(const char **why)
{
	if (!nxn_ready) {
		if (why)
			*why = "the host stack did not initialise";
		return -1;
	}
	if (nxn_state == LINK_BRIDGE_FAILED) {
		/* The owner thread clears this within half a second of a `wifi reset`; it
		 * has to do the clearing, because it is the one holding what must be given
		 * back.  Until then, say what fixes it. */
		if (why)
			*why = "the host stack could not be shut down cleanly -- run "
			       "`wifi reset`";
		return -1;
	}
	if (nxn_state != LINK_BRIDGE_OFF) {
		if (why)
			*why = "the host stack is already up or in transition";
		return -1;
	}

	/*
	 * Publish ARMING on THIS thread, before the owner is signalled: a command that
	 * refuses unless the state is OFF has to see it from the moment this one is typed,
	 * not from whenever the owner next runs: the state first, then the wake-up.
	 */
	nxn_why = "";
	nxn_state = LINK_BRIDGE_ARMING;
	tx_event_flags_set(&nxn_evt, NXN_EVT_CMD, TX_OR);
	return 0;
}

void link_bridge_down(void)
{
	if (!nxn_ready)
		return;
	nxn_req_stop = 1u;
	tx_event_flags_set(&nxn_evt, NXN_EVT_CMD, TX_OR);
}

/*
 * The link has just been force-quiesced and CHIP_EN is about to move (owhinata/wio-lite-ai#41).
 *
 * The owner ALREADY has the revocation: nxn_entry()'s UP branch compares
 * rtl_link_uart_gen() against nxn_uart_gen before every refresh, and
 * rtl_link_force_quiesce() advances that generation.  What it does not have is a
 * wake-up -- it is asleep in the NXN_REFRESH_MS wait at the bottom of the same branch,
 * so the revocation landed up to eight seconds late and `wifi connect` meanwhile saw a
 * stale LINK_BRIDGE_UP: wifi_hold_bridge() asked a module that had just been power-cycled to
 * renew the bridge, and refused when it did not answer.  That is the whole of
 * owhinata/wio-lite-ai#41 -- the time between the two, not a missing path.
 *
 * NOT link_bridge_down(), deliberately.  That posts nxn_req_stop and runs the graceful
 * nxn_stop(), which takes the coarse mutex for up to NXN_ARM_CLAIM_MS and talks eRPC to
 * the module (DATA_CFG(off), then a settle it must PROVE) -- against a module that is
 * about to lose power, and in front of the three commands whose whole job is to work
 * when everything else is stuck.  cmd_wifi_flash.c can afford that shape because it may
 * refuse; `wifi reset` may not.  nxn_link_revoked() is the unwind that suits a module
 * that is disappearing: no eRPC, no lock, driver gate closed before the detach.
 *
 * So this writes no state of its own -- half-installed bridges are the owner's to roll
 * back (see link_bridge.h) -- it only pokes and waits.  Best effort: on timeout the caller
 * says so and carries on, because refusing a recovery command is never the answer.
 */
int link_bridge_link_taken(void)
{
	uint32_t waited = 0u;

	if (!nxn_ready)
		return 0;

	tx_event_flags_set(&nxn_evt, NXN_EVT_CMD, TX_OR);
	/*
	 * Sleep rather than spin.  The owner runs at NXN_OWNER_PRIORITY (13) and the CLI
	 * threads below it (16), so today it preempts us the moment the flag is set and the
	 * first poll already sees OFF -- but a busy-wait would turn a future priority change
	 * into a hang, and this loop is not on any hot path.
	 */
	while (nxn_state == LINK_BRIDGE_UP && waited < NXN_TAKEN_WAIT_MS) {
		tx_thread_sleep(NXN_TAKEN_POLL_MS);
		waited += NXN_TAKEN_POLL_MS;
	}
	return (nxn_state == LINK_BRIDGE_UP) ? -1 : 0;
}

void link_bridge_modstats_get(struct link_bridge_modstats *out)
{
	/* Paired with nxn_mod_accumulate(): a consistent snapshot, not nine independent
	 * reads (`net info` quotes drops as a fraction of frames). */
	uint32_t pm = __get_PRIMASK();

	__disable_irq();
	*out = nxn_mod;
	__set_PRIMASK(pm);
}

/* ---- reporting ----------------------------------------------------------------- */

const char *link_bridge_state_name(enum link_bridge_state s)
{
	switch (s) {
	case LINK_BRIDGE_OFF:      return "off";
	case LINK_BRIDGE_ARMING:   return "arming";
	case LINK_BRIDGE_UP:       return "up";
	case LINK_BRIDGE_STOPPING: return "stopping";
	default:              return "FAILED";
	}
}

void link_bridge_session_get(struct link_bridge_session *out)
{
	out->state = nxn_state;
	out->why = nxn_why;
	memcpy(out->mac, nxn_mac, sizeof out->mac);
	out->radio_up = (nxn_radio_up != 0u);
	out->assoc_yes = nxn_assoc_yes;
	out->assoc_no = nxn_assoc_no;
	out->assoc_noanswer = nxn_assoc_noanswer;
}

/*
 * Hold the module's bridge watchdog open across a long eRPC flow (owhinata/wio-lite-ai#30 B2b).
 *
 * The owner refreshes the tap every NXN_REFRESH_MS by taking the coarse mutex -- so a
 * command that holds that mutex for longer than NXN_HOLD_MS silently loses the bridge
 * mid-flow.  `wifi connect` blocks up to 15 s on the module and `wifi scan` up to 15 s
 * twice (waiting out a previous scan, then its own), which is well past it.
 *
 * CONTRACT: the caller already holds the coarse mutex, exactly like
 * rtl_link_force_quiesce().  This issues one CTRL with the module's maximum hold; the
 * next ordinary refresh puts it back to NXN_HOLD_MS.  Re-arming is idempotent on the
 * module (fw/rtl8720 e_bridge_locked() just moves the deadline), so calling it between
 * the phases of a long flow is free.
 *
 * Returns 0 when the bridge is not up (nothing to hold) or the extension was
 * acknowledged, -1 when the module did not answer -- and a caller that gets -1 MUST NOT
 * start the long flow, because the alternative is losing the bridge in the middle of it.
 */
int link_bridge_hold_extend(void)
{
	if (nxn_state != LINK_BRIDGE_UP)
		return 0;
	return (nxn_data_cfg(ERPC_DATA_MODE_BRIDGE, NXN_HOLD_MAX_MS) == 0) ? 0 : -1;
}
