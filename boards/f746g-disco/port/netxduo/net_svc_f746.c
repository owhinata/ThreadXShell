/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    net_svc_f746.c
 * @brief   The shared `net` command's adapter on STM32F746G-DISCO (issue #219).
 *
 * Checks this board's own state -- is the IP stack there at all -- and hands
 * the address, DHCP and ping operations to the shared implementation
 * (shell/cmds/net_core.c, issue #220), which also puts a failed static
 * address back.  Takes no `struct cli_instance`: printing, waiting and Ctrl+C
 * are the shared command's (shell/cmds/cmd_net.c).  Owns no storage.
 *
 * This board never returns BUSY: the IP stack is created once at boot and is
 * never taken down.
 */
#include "net_svc.h"

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#include "fmt.h"
#include "net_core.h"
#include "nx_glue.h"
#include "nx_mjpeg.h"
#include "nx_shell.h"   /* telnet console output statistics (issue #6) */

/* The one reason this board gives: main.c creates the IP stack only after
   eth_init() succeeded, so its absence says the link driver did not start. */
static const char net_f746_not_up[] = "IP stack not up";

/* 1 when the IP stack exists and the caller may go on; 0 with @p res filled
 * otherwise. */
static int net_f746_ready(struct net_svc_result *res)
{
	if (nx_net_is_up())
		return 1;
	res->status = NET_SVC_DOWN;
	res->why = net_f746_not_up;
	return 0;
}

void net_svc_addr_get(struct net_svc_addr *out, struct net_svc_result *res)
{
	if (net_f746_ready(res))
		net_core_addr_get(out, res);
}

void net_svc_set_static(uint32_t ip, uint32_t mask, uint32_t gw,
                        struct net_svc_result *res)
{
	if (net_f746_ready(res))
		net_core_set_static(ip, mask, gw, res);
}

void net_svc_dhcp_start(struct net_svc_result *res)
{
	if (net_f746_ready(res))
		net_core_dhcp_start(res);
}

void net_svc_ping(uint32_t ip, unsigned timeout_ms, unsigned *rtt_ms,
                  struct net_svc_result *res)
{
	if (net_f746_ready(res))
		net_core_ping(ip, timeout_ms, rtt_ms, res);
}

/* Nothing to arm: the telnet console is resident on this board. */
void net_svc_addressed(void)
{
}

/* ---- the board's lines of `net info` -------------------------------------- */

/* Everything the lines print, sampled before the first one is written. */
struct net_f746_sample {
	bool                  mjpeg_on;
	bool                  mjpeg_client;
	uint32_t              mjpeg_frames;
	bool                  telnet_on;
	struct nx_shell_stats ts;
	bool                  diag_on;
	struct nx_net_diag    d;
};

/* The longest line is the telnet one: about 105 characters of text and nine
   32-bit counters. */
#define NET_F746_LINE_MAX 256u

/* Format one whole line and write it.  0, or the writer's failure. */
static int net_f746_line(net_svc_write_fn write, void *ctx, const char *fmt, ...)
{
	char line[NET_F746_LINE_MAX];
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = fmt_vsnformat(line, sizeof line, fmt, ap);
	va_end(ap);
	return (write(ctx, line, (size_t)n) < 0) ? -1 : 0;
}

/* Kept out of line so the MJPEG statistics block is off the stack again before
   the lines are written. */
static void __attribute__((noinline))
net_f746_sample_mjpeg(struct net_f746_sample *s)
{
	struct nx_mjpeg_stats ms;

	s->mjpeg_on = nx_mjpeg_stats_get(&ms);
	if (s->mjpeg_on) {
		s->mjpeg_client = ms.client;
		s->mjpeg_frames = ms.sent_frames;
	}
}

/*
 * The MJPEG state (while it runs), the telnet console's output health and the
 * driver / TCP / packet-pool counters since boot (issue #202), in that order and
 * in today's words.  The getters each take what lock they need and are done
 * before anything is written; the groups are read one after another, not as one
 * snapshot.
 */
int net_svc_board_lines(net_svc_write_fn write, void *ctx)
{
	struct net_f746_sample s;
	const struct nx_net_diag *d = &s.d;

	net_f746_sample_mjpeg(&s);
	s.telnet_on = nx_shell_stats_get(&s.ts);
	s.diag_on = (nx_net_diag_get(&s.d) == NXG_OK);

	if (s.mjpeg_on &&
	    net_f746_line(write, ctx, "mjpeg: http://board:80 (%s, %lu frames)\r\n",
	                  s.mjpeg_client ? "streaming" : "idle",
	                  (unsigned long)s.mjpeg_frames) != 0)
		return -1;
	/* The refusal reasons are reported apart because they mean different
	 * things: qdepth is the designed back-pressure, win means the peer is not
	 * reading, and nobuf says the shared packet pool was the constraint. */
	if (s.telnet_on &&
	    net_f746_line(write, ctx,
	                  "telnet: :23 (%s, %lu sessions), tx %lu seg / %lu B, "
	                  "ring peak %lu B, refused q%lu w%lu buf%lu err%lu, "
	                  "dropped %lu B\r\n",
	                  s.ts.connected ? "client attached" : "idle",
	                  (unsigned long)s.ts.sessions,
	                  (unsigned long)s.ts.tx_segs, (unsigned long)s.ts.tx_bytes,
	                  (unsigned long)s.ts.tx_hiwater, (unsigned long)s.ts.tx_qdepth,
	                  (unsigned long)s.ts.tx_win, (unsigned long)s.ts.tx_nobuf,
	                  (unsigned long)s.ts.tx_err,
	                  (unsigned long)s.ts.tx_dropped) != 0)
		return -1;
	if (!s.diag_on)
		return 0;
	if (net_f746_line(write, ctx, "eth rx: ok %lu, drop %lu, no-buf %lu\r\n",
	                  (unsigned long)d->rx_ok, (unsigned long)d->rx_drop,
	                  (unsigned long)d->rx_no_buf) != 0)
		return -1;
	if (net_f746_line(write, ctx, "eth tx: ok %lu, drop %lu (mac-down %lu, "
	                  "coalesce %lu, hal %lu)\r\n",
	                  (unsigned long)d->tx_ok, (unsigned long)d->tx_drop,
	                  (unsigned long)d->tx_drop_mac_down,
	                  (unsigned long)d->tx_drop_coalesce,
	                  (unsigned long)d->tx_drop_hal) != 0)
		return -1;
	if (net_f746_line(write, ctx, "eth hal: last refusal err 0x%08lx, dma "
	                  "0x%08lx, state 0x%02lx\r\n",
	                  (unsigned long)d->hal_err_last,
	                  (unsigned long)d->hal_dma_err_last,
	                  (unsigned long)d->hal_state_last) != 0)
		return -1;
	if (net_f746_line(write, ctx, "eth dma: errors %lu, last 0x%08lx\r\n",
	                  (unsigned long)d->dma_err,
	                  (unsigned long)d->dma_err_last) != 0)
		return -1;
	if (d->tcp_valid) {
		if (net_f746_line(write, ctx, "tcp:    sent %lu pkts / %lu B, rcvd %lu "
		                  "pkts / %lu B, retransmit %lu\r\n",
		                  (unsigned long)d->tcp_pkts_sent,
		                  (unsigned long)d->tcp_bytes_sent,
		                  (unsigned long)d->tcp_pkts_rcvd,
		                  (unsigned long)d->tcp_bytes_rcvd,
		                  (unsigned long)d->tcp_retransmits) != 0)
			return -1;
		if (net_f746_line(write, ctx, "tcp:    invalid %lu, rx dropped %lu, "
		                  "checksum %lu; conns %lu, disconns %lu, dropped "
		                  "%lu\r\n",
		                  (unsigned long)d->tcp_invalid,
		                  (unsigned long)d->tcp_rx_dropped,
		                  (unsigned long)d->tcp_checksum_err,
		                  (unsigned long)d->tcp_conns,
		                  (unsigned long)d->tcp_disconns,
		                  (unsigned long)d->tcp_conns_dropped) != 0)
			return -1;
	}
	if (d->pool_valid &&
	    net_f746_line(write, ctx, "pool:   free %lu of %lu, empty %lu (waited "
	                  "%lu), bad release %lu\r\n",
	                  (unsigned long)d->pool_free, (unsigned long)d->pool_total,
	                  (unsigned long)d->pool_empty_req,
	                  (unsigned long)d->pool_empty_susp,
	                  (unsigned long)d->pool_invalid_rel) != 0)
		return -1;
	return 0;
}
