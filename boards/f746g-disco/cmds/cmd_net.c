/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    cmd_net.c
 * @brief   `net` shell command: NetX Duo IPv4 over the on-board Ethernet
 * (owhinata/stm32f746g-disco#49 P1/P2).
 *
 *   net info               IP / mask / gateway, MJPEG / telnet state, counters
 *   net ping <a.b.c.d> [n]  ICMP echo n times (default 4) with RTT
 *   net ip <a.b.c.d/mask> [gw]  set a static address (stops DHCP)
 *   net dhcp               (re)acquire an address via DHCP
 *
 * Everything here sits on NetX Duo and talks to port/netxduo (nx_glue.h) -- the
 * shell never includes nx_api.h directly.  The link layer (MAC, PHY, link
 * state, auto-negotiation) is the board's `eth` command (cmd_eth.c, issue #217).
 *
 * Clean-room design; no third-party code reused.
 */
#include "cli.h"
#include "nx_glue.h"
#include "nx_mjpeg.h"
#include "nx_shell.h"   /* telnet console output statistics (issue #6) */

#include <stddef.h>   /* size_t / NULL */
#include <stdint.h>

static void print_octets(struct cli_instance *sh, const char *label, uint32_t a)
{
	cli_print(sh, "%s%lu.%lu.%lu.%lu", label, (unsigned long)((a >> 24) & 0xFF),
	          (unsigned long)((a >> 16) & 0xFF), (unsigned long)((a >> 8) & 0xFF),
	          (unsigned long)(a & 0xFF));
}

/* Guard: the NetX IP stack (P2) must be up.  main.c creates it only after
   eth_init() succeeded, so this also says the link driver is initialised
   (issue #217: the link driver's own guard moved to `eth`). */
static int net_ip_ready(struct cli_instance *sh)
{
	if (!nx_net_is_up()) {
		cli_error(sh, "net: IP stack not up\r\n");
		return 0;
	}
	return 1;
}

static void net_print_ip(struct cli_instance *sh, const struct nx_net_info *ni)
{
	if (ni->ip_valid) {
		print_octets(sh, "ip:    ", ni->ip);
		cli_print(sh, "/%u ", cli_ipv4_mask_bits(ni->mask));
		/* 0 is "no gateway", not an address (issue #19): printing 0.0.0.0 reads
		   as a configured one. */
		if (ni->gw != 0u)
			print_octets(sh, "gw ", ni->gw);
		else
			cli_print(sh, "gw none");
		cli_print(sh, " (%s)\r\n", ni->dhcp_mode ? "dhcp" : "static");
	} else {
		cli_print(sh, "ip:    none (%s)\r\n",
		          ni->dhcp_mode ? "dhcp pending" : "unset");
	}
}

/* Driver, TCP and packet-pool counters since boot (issue #202).  Printed as
   read; the three groups are not one snapshot. */
static void net_print_diag(struct cli_instance *sh)
{
	struct nx_net_diag d;

	if (nx_net_diag_get(&d) != NXG_OK)
		return;
	cli_print(sh, "eth rx: ok %lu, drop %lu, no-buf %lu\r\n",
	          (unsigned long)d.rx_ok, (unsigned long)d.rx_drop,
	          (unsigned long)d.rx_no_buf);
	cli_print(sh, "eth tx: ok %lu, drop %lu (mac-down %lu, coalesce %lu, "
	              "hal %lu)\r\n",
	          (unsigned long)d.tx_ok, (unsigned long)d.tx_drop,
	          (unsigned long)d.tx_drop_mac_down,
	          (unsigned long)d.tx_drop_coalesce, (unsigned long)d.tx_drop_hal);
	cli_print(sh, "eth hal: last refusal err 0x%08lx, dma 0x%08lx, "
	              "state 0x%02lx\r\n",
	          (unsigned long)d.hal_err_last, (unsigned long)d.hal_dma_err_last,
	          (unsigned long)d.hal_state_last);
	cli_print(sh, "eth dma: errors %lu, last 0x%08lx\r\n",
	          (unsigned long)d.dma_err, (unsigned long)d.dma_err_last);
	if (d.tcp_valid) {
		cli_print(sh, "tcp:    sent %lu pkts / %lu B, rcvd %lu pkts / %lu B, "
		              "retransmit %lu\r\n",
		          (unsigned long)d.tcp_pkts_sent, (unsigned long)d.tcp_bytes_sent,
		          (unsigned long)d.tcp_pkts_rcvd, (unsigned long)d.tcp_bytes_rcvd,
		          (unsigned long)d.tcp_retransmits);
		cli_print(sh, "tcp:    invalid %lu, rx dropped %lu, checksum %lu; "
		              "conns %lu, disconns %lu, dropped %lu\r\n",
		          (unsigned long)d.tcp_invalid, (unsigned long)d.tcp_rx_dropped,
		          (unsigned long)d.tcp_checksum_err,
		          (unsigned long)d.tcp_conns, (unsigned long)d.tcp_disconns,
		          (unsigned long)d.tcp_conns_dropped);
	}
	if (d.pool_valid)
		cli_print(sh, "pool:   free %lu of %lu, empty %lu (waited %lu), "
		              "bad release %lu\r\n",
		          (unsigned long)d.pool_free, (unsigned long)d.pool_total,
		          (unsigned long)d.pool_empty_req,
		          (unsigned long)d.pool_empty_susp,
		          (unsigned long)d.pool_invalid_rel);
}

static int cmd_net_info(struct cli_instance *sh, int argc, char **argv)
{
	struct nx_net_info ni;
	struct nx_mjpeg_stats ms;
	struct nx_shell_stats ts;

	(void)argc;
	(void)argv;
	if (!net_ip_ready(sh))
		return 1;

	if (nx_net_info_get(&ni) == NXG_OK)
		net_print_ip(sh, &ni);
	if (nx_mjpeg_stats_get(&ms))
		cli_print(sh, "mjpeg: http://board:80 (%s, %lu frames)\r\n",
		          ms.client ? "streaming" : "idle",
		          (unsigned long)ms.sent_frames);
	/* Telnet console output health (issue #6).  The refusal reasons are
	 * reported apart because they mean different things: qdepth is the
	 * designed back-pressure, win means the peer is not reading, and nobuf
	 * says the shared packet pool was the constraint. */
	if (nx_shell_stats_get(&ts))
		cli_print(sh, "telnet: :23 (%s, %lu sessions), tx %lu seg / %lu B, "
		              "ring peak %lu B, refused q%lu w%lu buf%lu err%lu, "
		              "dropped %lu B\r\n",
		          ts.connected ? "client attached" : "idle",
		          (unsigned long)ts.sessions,
		          (unsigned long)ts.tx_segs, (unsigned long)ts.tx_bytes,
		          (unsigned long)ts.tx_hiwater, (unsigned long)ts.tx_qdepth,
		          (unsigned long)ts.tx_win, (unsigned long)ts.tx_nobuf,
		          (unsigned long)ts.tx_err, (unsigned long)ts.tx_dropped);
	net_print_diag(sh);
	return 0;
}

static int cmd_net_ping(struct cli_instance *sh, int argc, char **argv)
{
	uint32_t ip, c;
	int count = 4, ok = 0;
	unsigned rmin = 0xFFFFFFFFu, rmax = 0, rsum = 0;

	if (!net_ip_ready(sh))
		return 1;
	if (cli_parse_ipv4(argv[1], &ip) != 0) {
		cli_error(sh, "net: bad address '%s'\r\n", argv[1]);
		return 1;
	}
	if (argc >= 3 && cli_parse_u32(argv[2], &c) == 0 && c > 0 && c <= 100)
		count = (int)c;

	cli_print(sh, "PING %s, %d probes:\r\n", argv[1], count);
	for (int i = 0; i < count; i++) {
		unsigned rtt = 0;
		int rc;

		if (cli_cancel_requested(sh))
			break;
		rc = nx_net_ping(ip, 1000, &rtt);
		if (rc == NXG_OK) {
			cli_print(sh, "  reply %d: %u ms\r\n", i + 1, rtt);
			ok++;
			rsum += rtt;
			if (rtt < rmin) rmin = rtt;
			if (rtt > rmax) rmax = rtt;
		} else if (rc == NXG_TIMEOUT) {
			cli_print(sh, "  probe %d: timeout\r\n", i + 1);
		} else {
			cli_error(sh, "  probe %d: error\r\n", i + 1);
		}
		if (i + 1 < count && cli_sleep(sh, 1000) != 0)
			break;        /* Ctrl+C between probes */
	}
	cli_print(sh, "%d/%d received", ok, count);
	if (ok > 0)
		cli_print(sh, ", rtt min/avg/max %u/%u/%u ms",
		          rmin, rsum / (unsigned)ok, rmax);
	cli_print(sh, "\r\n");
	return 0;
}

static int cmd_net_ip(struct cli_instance *sh, int argc, char **argv)
{
	uint32_t ip, mask, gw = 0;

	if (!net_ip_ready(sh))
		return 1;
	if (cli_parse_ipv4_cidr(argv[1], &ip, &mask) != 0) {
		cli_error(sh, "net: bad address '%s' (use a.b.c.d/mask)\r\n", argv[1]);
		return 1;
	}
	if (argc >= 3 && cli_parse_ipv4(argv[2], &gw) != 0) {
		cli_error(sh, "net: bad gateway '%s'\r\n", argv[2]);
		return 1;
	}
	if (nx_net_set_static(ip, mask, gw) != NXG_OK) {
		cli_error(sh, "net: set static failed\r\n");
		return 1;
	}
	cli_print(sh, "net: static address set\r\n");
	return 0;
}

static int cmd_net_dhcp(struct cli_instance *sh, int argc, char **argv)
{
	(void)argc;
	(void)argv;
	if (!net_ip_ready(sh))
		return 1;
	int rc = nx_net_dhcp_renew();

	if (rc < 0) {                       /* NXG_RENEWED is a success (issue #9) */
		cli_error(sh, "net: DHCP start failed\r\n");
		return 1;
	}
	if (rc == NXG_RENEWED)
		cli_print(sh, "net: renewing the existing lease (kept meanwhile)...\r\n");
	else
		cli_print(sh, "net: acquiring via DHCP (up to 10 s, Ctrl+C to stop)...\r\n");
	for (int i = 0; i < 100; i++) {
		struct nx_net_info ni;

		if (cli_sleep(sh, 100) != 0)
			return 1;
		if (nx_net_info_get(&ni) == NXG_OK && ni.ip_valid) {
			net_print_ip(sh, &ni);
			return 0;
		}
	}
	cli_warn(sh, "net: no DHCP lease after 10 s\r\n");
	return 0;
}

/* ---- MJPEG-over-HTTP camera streaming (owhinata/stm32f746g-disco#49 P5) ------------ */

static int cmd_net_mjpeg_start(struct cli_instance *sh, int argc, char **argv)
{
	int rc;

	(void)argc;
	(void)argv;
	if (!net_ip_ready(sh))
		return 1;
	/* owhinata/stm32f746g-disco#101: MJPEG is a JPEG-class subscriber -- it attaches to the
    running base
	   (resolution follows the base), it no longer owns/starts the DCMI.  Report the
	   precise reason on a format/state clash instead of silently opening a dead port
	   (the owhinata/stm32f746g-disco#97 class of bug). */
	rc = nx_mjpeg_start();
	if (rc == -2) {
		cli_error(sh, "net: mjpeg already running\r\n");
		return 1;
	}
	if (rc == NX_MJPEG_NO_CAPTURE) {
		cli_error(sh, "net: no camera capture; run `camera format jpeg` then "
		          "`camera stream start` first\r\n");
		return 1;
	}
	if (rc == NX_MJPEG_FMT_CLASH) {
		cli_error(sh, "net: camera stream is raster (not JPEG); mjpeg needs JPEG "
		          "-- `camera stream stop`, `camera format jpeg`, restart\r\n");
		return 1;
	}
	if (rc == NX_MJPEG_PINS) {
		/* issue #72: the start failed and its unwind could not get the sink
		   back, so the stream stays reserved until a stop proves it clear. */
		cli_error(sh, "net: mjpeg start failed and the camera has not released "
		          "the frame; run `net mjpeg stop` before retrying\r\n");
		return 1;
	}
	if (rc != 0) {
		cli_error(sh, "net: mjpeg start failed (%d)\r\n", rc);
		return 1;
	}
	cli_print(sh, "net: mjpeg streaming at http://<board-ip>:80/ "
	          "(follows camera stream)\r\n");
	return 0;
}

static int cmd_net_mjpeg_stop(struct cli_instance *sh, int argc, char **argv)
{
	int rc;

	(void)argc;
	(void)argv;
	if (!net_ip_ready(sh))
		return 1;
	rc = nx_mjpeg_stop();
	if (rc == -1) {
		cli_error(sh, "net: mjpeg not running\r\n");
		return 1;
	}
	if (rc == NX_MJPEG_BUSY) {
		cli_error(sh, "net: busy -- another mjpeg start/stop is in progress\r\n");
		return 1;
	}
	if (rc == NX_MJPEG_PINS) {
		/* issue #72: the sink is detached but still pinned, so a producer
		   callback may still be copying into the send buffer.  Refusing the
		   restart is the point; retrying this command is what clears it. */
		cli_error(sh, "net: camera has not released the mjpeg frame; the stream "
		          "stays reserved -- retry `net mjpeg stop`\r\n");
		return 1;
	}
	if (rc != 0) {
		cli_error(sh, "net: mjpeg stop timed out\r\n");
		return 1;
	}
	cli_print(sh, "net: mjpeg detached (camera stream continues)\r\n");
	return 0;
}

static int cmd_net_mjpeg_stats(struct cli_instance *sh, int argc, char **argv)
{
	struct nx_mjpeg_stats st;

	(void)argc;
	(void)argv;
	if (!net_ip_ready(sh))
		return 1;
	if (!nx_mjpeg_stats_get(&st)) {
		cli_print(sh, "mjpeg: not running\r\n");
		return 0;
	}
	cli_print(sh, "state:     %s\r\n", st.client ? "streaming" : "idle (no client)");
	cli_print(sh, "conns:     %lu\r\n", (unsigned long)st.conns);
	cli_print(sh, "sent:      %lu frames, %lu bytes\r\n",
	          (unsigned long)st.sent_frames, (unsigned long)st.sent_bytes);
	cli_print(sh, "dropped:   busy %lu, oversized %lu\r\n",
	          (unsigned long)st.drop_busy, (unsigned long)st.drop_oversized);
	cli_print(sh, "errors:    send %lu, pool %lu\r\n",
	          (unsigned long)st.send_err, (unsigned long)st.pool_fail);
	/* issue #202: the send failures by NetX's answer, what the connection had
	   sent when the last one hit, and the re-accept that follows. */
	cli_print(sh, "send fail: window %lu, queue %lu, notconn %lu, nopkt %lu, "
	              "other %lu, last 0x%02lx\r\n",
	          (unsigned long)st.send_window, (unsigned long)st.send_queue,
	          (unsigned long)st.send_notconn, (unsigned long)st.send_nopkt,
	          (unsigned long)st.send_other, (unsigned long)st.send_last);
	cli_print(sh, "at fail:   %lu frames, %lu pkts, %lu bytes on that "
	              "connection\r\n",
	          (unsigned long)st.fail_frames, (unsigned long)st.fail_pkts,
	          (unsigned long)st.fail_bytes);
	cli_print(sh, "this conn: %lu frames, %lu pkts, %lu bytes\r\n",
	          (unsigned long)st.conn_frames, (unsigned long)st.conn_pkts,
	          (unsigned long)st.conn_bytes);
	cli_print(sh, "relisten:  failed %lu (pending %lu), last 0x%02lx\r\n",
	          (unsigned long)st.relisten_fail,
	          (unsigned long)st.relisten_pending,
	          (unsigned long)st.relisten_last);
	cli_print(sh, "teardown:  disconnect failed %lu, unaccept failed %lu\r\n",
	          (unsigned long)st.disconnect_fail, (unsigned long)st.unaccept_fail);
	/* issue #202, second change: sends retried instead of closing, why
	   connections closed, and the passive open armed once per client. */
	cli_print(sh, "retried:   %lu sends, longest wait %lu ms\r\n",
	          (unsigned long)st.send_retried, (unsigned long)st.stall_max);
	cli_print(sh, "closed:    tcp ended %lu, stalled 10 s %lu\r\n",
	          (unsigned long)st.close_tcp, (unsigned long)st.close_stall);
	cli_print(sh, "accept:    armed %lu, failed %lu (last 0x%02lx), re-armed %lu\r\n",
	          (unsigned long)st.accept_armed, (unsigned long)st.accept_fail,
	          (unsigned long)st.accept_last, (unsigned long)st.accept_rearm);
	cli_print(sh, "pending:   relisten answered pending %lu (taken as success)\r\n",
	          (unsigned long)st.relisten_pending_ok);
	return 0;
}

CLI_SUBCMD_SET_CREATE(net_mjpeg_subcmds,
	CLI_CMD(start, NULL,
	        "attach MJPEG-over-HTTP on :80 to the running JPEG camera stream",
	        cmd_net_mjpeg_start),
	CLI_CMD(stop, NULL, "detach MJPEG (the camera stream keeps running)",
	        cmd_net_mjpeg_stop),
	CLI_CMD(stats, NULL, "MJPEG client / frame / drop counters",
	        cmd_net_mjpeg_stats),
	CLI_SUBCMD_SET_END);

CLI_SUBCMD_SET_CREATE(net_subcmds,
	CLI_CMD(info, NULL, "IP / mask / gateway, telnet / MJPEG state, counters",
	        cmd_net_info),
	CLI_CMD_ARG_USAGE(ping, NULL, "ICMP echo <a.b.c.d> [count]",
	                  "<a.b.c.d> [count]", cmd_net_ping, 2, 1),
	CLI_CMD_ARG_USAGE(ip, NULL, "set static <a.b.c.d/mask> [gw]",
	                  "<a.b.c.d/mask> [gw]", cmd_net_ip, 2, 1),
	CLI_CMD(dhcp, NULL, "(re)acquire an address via DHCP", cmd_net_dhcp),
	CLI_CMD(mjpeg, net_mjpeg_subcmds,
	        "MJPEG-over-HTTP camera stream (start/stop/stats)", NULL),
	CLI_SUBCMD_SET_END);

CLI_CMD_REGISTER(net, net_subcmds,
                 "NetX Duo IPv4 over the on-board Ethernet (link: see `eth`)",
                 NULL, 1, 0);
