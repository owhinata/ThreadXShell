/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * What the stub `net` adapter (net_svc_stub.c) answers, and what it saw
 * (issue #219).  Test-only state; the shared command under test owns none.
 *
 * The stub is also the test's clock: tx_time_get() returns net_stub.now, every
 * adapter call adds net_stub.call_ms to it, and net_stub_sleep_hook() (the
 * host cli_sleep()'s hook) adds the ticks the command asked to sleep.
 */
#ifndef NET_SVC_STUB_H
#define NET_SVC_STUB_H

#include <stdint.h>

#include "tx_api.h"   /* ULONG: the clock is as wide as ThreadX's */
#include "net_svc.h"

struct cli_instance;

#define NET_STUB_SEQ_MAX   16
#define NET_STUB_LINES_MAX 4
/* More adapter calls than this in one command is a runaway wait: the stub
 * answers DOWN from then on, so a broken deadline ends the test instead of
 * hanging it. */
#define NET_STUB_RUNAWAY   5000

/* The answers of one operation, in call order; the last one repeats. */
struct net_stub_seq {
	enum net_svc_status st[NET_STUB_SEQ_MAX];
	int n;              /* 0: always NET_SVC_OK */
};

struct net_stub {
	/* answers */
	struct net_stub_seq set_static, dhcp, ping, addr;
	struct net_svc_addr address;    /* what addr_get answers with OK      */
	ULONG addr_valid_at;            /* ip_valid from this clock value on  */
	int addr_valid_late;            /* ... when set (else address as is)  */
	unsigned rtt;                   /* a reply's round trip               */
	unsigned call_ms;               /* each call takes this long          */
	const char *lines[NET_STUB_LINES_MAX];
	int nlines;
	int fail_after_lines;           /* the console fails after N lines    */
	/* the clock and Ctrl+C */
	ULONG now;
	int ctrl_c_at_sleep;            /* inject 0x03 in this sleep (1-based) */
	/* what was called */
	int calls_static, calls_dhcp, calls_ping, calls_addr, calls_lines;
	int applied_static, applied_dhcp;   /* calls that got past BUSY       */
	int sleeps;
	int runaway;
	uint32_t last_ip, last_mask, last_gw;
	unsigned last_timeout_ms;
	int lines_written, write_rc_bad;
};

extern struct net_stub net_stub;

void net_stub_reset(void);

/* The cli_sleep() hook: advance the clock, count, maybe inject a Ctrl+C. */
void net_stub_sleep_hook(struct cli_instance *sh, void *arg);

/* The transport the Ctrl+C is injected into (the test's). */
void net_stub_set_transport(void *tr);

#endif /* NET_SVC_STUB_H */
