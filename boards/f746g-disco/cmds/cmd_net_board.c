/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    cmd_net_board.c
 * @brief   This board's own `net` subcommand: `net mjpeg start|stop|stats`
 * (owhinata/stm32f746g-disco#49 P5).
 *
 * `net` itself is the shared command (shell/cmds/cmd_net.c, issue #219), which
 * appends the table below through NET_SVC_BOARD_SUBCMDS (svc/net_svc_config.h)
 * and registers the `net` root; this file registers no root.  info / ip /
 * dhcp / ping are the shared command's, over port/netxduo/net_svc_f746.c.
 *
 * Everything here sits on NetX Duo and talks to port/netxduo (nx_glue.h,
 * nx_mjpeg.h) -- the shell never includes nx_api.h directly.
 *
 * Clean-room design; no third-party code reused.
 */
#include "cli.h"
#include "nx_glue.h"
#include "nx_mjpeg.h"

#include <stddef.h>   /* NULL */
#include <stdint.h>

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

/* Not CLI_SUBCMD_SET_CREATE (which makes it static): the shared command's
   table names it (NET_SVC_BOARD_DECLS / NET_SVC_BOARD_SUBCMDS). */
const struct cli_cmd net_board_mjpeg_subcmds[] = {
	CLI_CMD(start, NULL,
	        "attach MJPEG-over-HTTP on :80 to the running JPEG camera stream",
	        cmd_net_mjpeg_start),
	CLI_CMD(stop, NULL, "detach MJPEG (the camera stream keeps running)",
	        cmd_net_mjpeg_stop),
	CLI_CMD(stats, NULL, "MJPEG client / frame / drop counters",
	        cmd_net_mjpeg_stats),
	CLI_SUBCMD_SET_END,
};
