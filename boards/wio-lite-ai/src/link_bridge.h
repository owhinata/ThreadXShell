/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/*
 * Wio Lite AI (STM32H725AEI6) -- the bridge session over the RTL8720 link
 * (owhinata/wio-lite-ai#23 U3; app/nx_net.c until issue #222).
 *
 * The resident owner of the session: the thread that holds the RTL8720 link reference,
 * turns the module into an L2 relay, keeps its watchdog fed, and takes the whole thing
 * down again provably.  The NetX objects that run over it are app/nx_net.h's; this is
 * the link layer underneath them and does not call up (app/link_bridge.c says what it
 * calls).
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
 * that lwIP cannot run while the host stack is up.  A command that would reconfigure
 * the module underneath the interface asks link_bridge_state() first: the link
 * diagnostics refuse unless it is off (cmd_wifi_link.c), and `wifi flash` takes the host
 * stack down first (cmd_wifi_flash.c).
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
#ifndef APP_LINK_BRIDGE_H
#define APP_LINK_BRIDGE_H

#include <stdbool.h>
#include <stdint.h>

enum link_bridge_state {
	LINK_BRIDGE_OFF = 0,     /* the module's own lwIP owns the network          */
	LINK_BRIDGE_ARMING,      /* bringing the bridge in                          */
	LINK_BRIDGE_UP,          /* the host stack owns the network                 */
	LINK_BRIDGE_STOPPING,
	LINK_BRIDGE_FAILED       /* teardown could not prove silence -- `wifi reset` */
};

/* Module-side counters accumulated across bridge re-arms (each re-arm zeroes the
 * module's own), in the order LINK_DATA_STATS reports them. */
struct link_bridge_modstats {
	uint32_t rx_frames, rx_bytes, rx_drops, rx_crc, rx_oversize, rx_gaps;
	uint32_t tx_frames, tx_bytes, tx_drops;
};

/* The session's values `net info` prints (nx_net_write_status()), copied in one call. */
struct link_bridge_session {
	enum link_bridge_state state;
	const char            *why;       /* why it last went down or refused; "" if none */
	uint8_t                mac[6];    /* the module's, valid once up                   */
	bool                   radio_up;  /* the module's WiFi driver is started           */
	uint32_t               assoc_yes, assoc_no, assoc_noanswer;   /* association polls */
};

/*
 * Create the session's thread and its event flags.  Object creation only, so it is safe
 * from tx_application_define(); called by nx_net_init() once the NetX objects exist.
 * Returns 0, or -1 when either create failed -- and only after BOTH succeeded do
 * link_bridge_up() / link_bridge_down() / link_bridge_link_taken() act.
 */
int  link_bridge_init(void);

/* Is the host stack the one that owns the network right now? */
bool link_bridge_is_up(void);
enum link_bridge_state link_bridge_state(void);

/* "off" / "arming" / "up" / "stopping" / "FAILED" -- so a refusal can name the state it
 * was refused in rather than just saying no. */
const char *link_bridge_state_name(enum link_bridge_state s);

/* Ask the owner thread to bring the interface up / take it down.  link_bridge_up()
 * returns 0 once the request is accepted (state becomes ARMING before it returns, so
 * guards close immediately); *why explains a refusal.  Poll link_bridge_state() for the
 * outcome. */
int  link_bridge_up(const char **why);
void link_bridge_down(void);

/*
 * "The link has been taken away and CHIP_EN is about to move" -- the epilogue of
 * rtl_link_force_quiesce() for `wifi on/off/reset` (owhinata/wio-lite-ai#41).
 *
 * The owner detects a revoked link by itself (the uart generation moved), but only when
 * it next wakes, which is up to NXN_REFRESH_MS later.  This hands it the news now and
 * waits, briefly, for it to leave LINK_BRIDGE_UP -- so the very next command does not
 * find a stale UP and try to renew a bridge on a module that has just been power-cycled.
 *
 * NOT link_bridge_down(): that is the graceful stop, which needs the coarse mutex and
 * speaks eRPC to the module, neither of which a recovery command can afford.  See the
 * body.
 *
 * Call AFTER rtl_link_force_quiesce() and BEFORE moving CHIP_EN.  Returns 0 once the
 * interface is no longer up (already-down counts), -1 if it has not stood down in time --
 * which the caller reports and then carries on with, rather than refusing.
 */
int  link_bridge_link_taken(void);

/* The module-side counters, as one consistent copy (`net info`). */
void link_bridge_modstats_get(struct link_bridge_modstats *out);

/* The session's values for `net info`, in one call. */
void link_bridge_session_get(struct link_bridge_session *out);

/*
 * Keep the module's bridge alive across a long eRPC flow (owhinata/wio-lite-ai#30 B2b).
 * Caller must hold the rtl_link coarse mutex.  Returns 0 (nothing to hold, or held) / -1
 * (the module did not answer -- do NOT start the flow).  See the definition for why.
 */
int link_bridge_hold_extend(void);

#endif /* APP_LINK_BRIDGE_H */
