/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * A stub `net` adapter (issue #219): answers svc/net/net_svc.h from the script
 * in net_stub and records what the shared command called.  Also the clock the
 * command's waits read (tx_time_get()).  See net_svc_stub.h.
 */
#include <string.h>

#include "tx_api.h"
#include "cli_backend_dummy.h"
#include "host_glue.h"
#include "net_svc.h"
#include "net_svc_stub.h"

struct net_stub net_stub;
static struct cli_transport *net_stub_tr;

ULONG tx_time_get(void)
{
	return net_stub.now;
}

void net_stub_reset(void)
{
	memset(&net_stub, 0, sizeof net_stub);
	net_stub.rtt = 3u;
	net_stub.address.ip_valid = true;
	net_stub.address.dhcp_mode = true;
	net_stub.address.ip = 0xC0A8010Au;      /* 192.168.1.10 */
	net_stub.address.mask = 0xFFFFFF00u;
	net_stub.address.gw = 0xC0A80101u;      /* 192.168.1.1  */
	/* A start that is not 0, so an "elapsed = now" bug shows, and close
	 * enough to the top that the clock wraps under a long wait. */
	net_stub.now = (ULONG)0 - (ULONG)0x1000u;
}

void net_stub_set_transport(void *tr)
{
	net_stub_tr = (struct cli_transport *)tr;
}

void net_stub_sleep_hook(struct cli_instance *sh, void *arg)
{
	static const char etx = 0x03;

	(void)sh;
	(void)arg;
	net_stub.sleeps++;
	net_stub.now += cli_test_sleep_ticks();
	if (net_stub.ctrl_c_at_sleep != 0 &&
	    net_stub.sleeps == net_stub.ctrl_c_at_sleep && net_stub_tr != NULL)
		cli_dummy_inject(net_stub_tr, &etx, 1);
}

static const char *net_stub_why(enum net_svc_status s)
{
	switch (s) {
	case NET_SVC_DOWN:   return "stub: down";
	case NET_SVC_BUSY:   return "stub: busy";
	case NET_SVC_FAILED: return "stub: failed";
	case NET_SVC_LINK:   return "stub: link down";
	default:             return NULL;
	}
}

/* The answer for the @p call-th (1-based) call of @p q, and the clock. */
static void net_stub_answer(const struct net_stub_seq *q, int call,
                            struct net_svc_result *res)
{
	int total = net_stub.calls_static + net_stub.calls_dhcp +
	            net_stub.calls_ping + net_stub.calls_addr;

	net_stub.now += net_stub.call_ms;
	if (total > NET_STUB_RUNAWAY) {
		net_stub.runaway = 1;
		res->status = NET_SVC_DOWN;
		res->why = "stub: runaway";
		return;
	}
	if (q->n == 0)
		res->status = NET_SVC_OK;
	else
		res->status = q->st[(call <= q->n ? call : q->n) - 1];
	res->why = net_stub_why(res->status);
}

void net_svc_addr_get(struct net_svc_addr *out, struct net_svc_result *res)
{
	net_stub.calls_addr++;
	net_stub_answer(&net_stub.addr, net_stub.calls_addr, res);
	if (res->status != NET_SVC_OK)
		return;
	*out = net_stub.address;
	if (net_stub.addr_valid_late)
		out->ip_valid = (long)(net_stub.now - net_stub.addr_valid_at) >= 0;
}

void net_svc_set_static(uint32_t ip, uint32_t mask, uint32_t gw,
                        struct net_svc_result *res)
{
	net_stub.calls_static++;
	net_stub.last_ip = ip;
	net_stub.last_mask = mask;
	net_stub.last_gw = gw;
	net_stub_answer(&net_stub.set_static, net_stub.calls_static, res);
	if (res->status != NET_SVC_BUSY)
		net_stub.applied_static++;
}

void net_svc_dhcp_start(struct net_svc_result *res)
{
	net_stub.calls_dhcp++;
	net_stub_answer(&net_stub.dhcp, net_stub.calls_dhcp, res);
	if (res->status != NET_SVC_BUSY)
		net_stub.applied_dhcp++;
}

void net_svc_ping(uint32_t ip, unsigned timeout_ms, unsigned *rtt_ms,
                  struct net_svc_result *res)
{
	net_stub.calls_ping++;
	net_stub.last_ip = ip;
	net_stub.last_timeout_ms = timeout_ms;
	net_stub_answer(&net_stub.ping, net_stub.calls_ping, res);
	if (res->status == NET_SVC_OK && rtt_ms != NULL)
		*rtt_ms = net_stub.rtt + (unsigned)net_stub.calls_ping;
}

int net_svc_board_lines(net_svc_write_fn write, void *ctx)
{
	net_stub.calls_lines++;
	for (int i = 0; i < net_stub.nlines; i++) {
		int rc = write(ctx, net_stub.lines[i], strlen(net_stub.lines[i]));

		if (rc < 0)
			return rc;
		if (rc != (int)strlen(net_stub.lines[i]))
			net_stub.write_rc_bad++;
		net_stub.lines_written++;
		if (net_stub.fail_after_lines != 0 && net_stub_tr != NULL &&
		    net_stub.lines_written == net_stub.fail_after_lines)
			cli_dummy_set_tx_fail(net_stub_tr, 1);
	}
	return 0;
}

void net_svc_addressed(void)
{
	net_stub.addressed++;
}
