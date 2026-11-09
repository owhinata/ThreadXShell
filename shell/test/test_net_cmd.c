/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Host test: the shared `net` command (shell/cmds/cmd_net.c, issue #219)
 * through the REAL dispatcher, over a stub adapter (net_svc_stub.c) that is
 * also the clock, once per shape (net_cfg/net_svc_config.h).
 *
 * What is pinned (nothing gates what a command prints, and the exit value does
 * not reach the console):
 *  - the table: shared order, the board's subcommands last, no name twice;
 *  - N1: a bad count is refused before anything is sent, the default is 4;
 *  - N2: no reply at all is exit 1 (also after a Ctrl+C), and the totals line;
 *  - a reason to stop on the first probe prints no heading; a later one stops
 *    the run with the totals so far;
 *  - BUSY is called again within ONE deadline that a slow adapter does not
 *    stretch, an operation behind BUSY takes effect once, and a Ctrl+C in the
 *    wait calls nothing more;
 *  - N3: the DHCP wait is 30 s and ends in exit 1; an address on the way is
 *    two lines and exit 0; the renew sentence;
 *  - N4: the two address lines, gw none, the STALE mark (every board, #220);
 *  - every refusal of an argument comes before the adapter is called;
 *  - the board's lines arrive through the writer one line per write, and a
 *    failed write stops them.
 */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cli_instance.h"
#include "cli_internal.h"
#include "cli_backend_dummy.h"
#include "host_glue.h"
#include "net_svc.h"
#include "net_svc_config.h"
#include "net_svc_stub.h"
#include "net_table_check.h"

#if defined(NET_TEST_SHAPE_EXTRA)
#define SHAPE "extra (board: extra)"
#elif defined(NET_TEST_SHAPE_BARE)
#define SHAPE "bare (no board subcommand)"
#else
#define SHAPE "dup (extra, and the board also registers `info`)"
#endif

#if !defined(NET_TEST_SHAPE_BARE)
/* The board's subcommand (NET_SVC_BOARD_DECLS). */
static int board_calls;
int test_net_board_cmd(struct cli_instance *sh, int argc, char **argv)
{
	(void)sh; (void)argc; (void)argv;
	board_calls++;
	return 0;
}

const struct cli_cmd test_net_extra_subcmds[] = {
	CLI_CMD(leaf, NULL, "board: a leaf", test_net_board_cmd),
	CLI_SUBCMD_SET_END,
};
#endif

/* ---- harness --------------------------------------------------------------- */

CLI_BACKEND_DUMMY_DEFINE(tr0);
static struct cli_instance sh0;

static void reset(void)
{
	memset(&sh0, 0, sizeof sh0);
	sh0.tr = &tr0;
	tr0.sh = &sh0;
	strcpy(sh0.prompt, "> ");
	cli_dummy_clear_output(&tr0);
	cli_dummy_clear_rx(&tr0);
	cli_dummy_reset_stats(&tr0);
	cli_dummy_set_tx_fail(&tr0, 0);
	cli_dummy_set_tx_cap(&tr0, 0);
	cli_test_set_tx_wait_hook(NULL, NULL);
	cli_test_set_sleep_hook(net_stub_sleep_hook, NULL);
	net_stub_reset();
	net_stub_set_transport(&tr0);
#if !defined(NET_TEST_SHAPE_BARE)
	board_calls = 0;
#endif
}

/* Run @p line; returns the command's exit value. */
static int run(const char *line)
{
	cli_dummy_clear_output(&tr0);
	cli_dummy_inject(&tr0, line, strlen(line));
	cli_test_pump(&sh0);
	return sh0.last_result;
}

#if !defined(NET_TEST_SHAPE_DUP)
static int has(const char *needle)
{
	return strstr(cli_dummy_output_str(&tr0), needle) != NULL;
}

/* @p a appears, and before @p b. */
static int before(const char *a, const char *b)
{
	const char *out = cli_dummy_output_str(&tr0);
	const char *pa = strstr(out, a), *pb = strstr(out, b);

	return pa != NULL && pb != NULL && pa < pb;
}

static void seq(struct net_stub_seq *q, int n, const enum net_svc_status *st)
{
	q->n = n;
	for (int i = 0; i < n; i++)
		q->st[i] = st[i];
}

#endif /* !NET_TEST_SHAPE_DUP */

#define SEQ(q, ...) do {                                                   \
	static const enum net_svc_status s_[] = { __VA_ARGS__ };           \
	seq(&(q), (int)(sizeof s_ / sizeof s_[0]), s_);                    \
} while (0)

static int failures;

static void expect(const char *what, int ok)
{
	printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
	if (!ok) {
		printf("       output was: \"%s\"\n", cli_dummy_output_str(&tr0));
		failures++;
	}
}

/* ---- the table ----------------------------------------------------------------- */

static void check_table(void)
{
	const struct cli_cmd *t = net_table();

	expect("the `net` root is registered", t != NULL);
	if (t == NULL)
		return;
#if defined(NET_TEST_SHAPE_DUP)
	/* The check must SEE a shadowing board entry, or a clean answer from the
	   real boards means nothing. */
	expect("a board entry reusing a shared name is caught",
	       net_table_dups(t) == 1 && net_table_index(t, "info") == 0);
	reset();
	run("net info\r");
	expect("... and it is unreachable: `net info` is the shared one",
	       board_calls == 0 && net_stub.calls_addr == 1);
#else
	expect("no subcommand name appears twice", net_table_dups(t) == 0);
	expect("shared order: info ip dhcp ping",
	       net_table_index(t, "info") == 0 && net_table_index(t, "ip") == 1 &&
	       net_table_index(t, "dhcp") == 2 && net_table_index(t, "ping") == 3);
#if !defined(NET_TEST_SHAPE_BARE)
	expect("the board's subcommand comes last", net_table_index(t, "extra") == 4 &&
	       t[5].name == NULL);
	reset();
	run("net extra leaf\r");
	expect("`net extra leaf` reaches the board", board_calls == 1);
#else
	expect("no board subcommand", t[4].name == NULL);
#endif
#endif
}

#if !defined(NET_TEST_SHAPE_DUP)
/* ---- net ping ---------------------------------------------------------------------- */

static void check_ping(void)
{
	int rc;

	/* N1: the three refusals, before anything is sent. */
	static const char *const bad[] = { "0", "101", "abc" };
	for (unsigned i = 0; i < sizeof bad / sizeof bad[0]; i++) {
		char line[64], want[64];

		reset();
		snprintf(line, sizeof line, "net ping 10.0.0.1 %s\r", bad[i]);
		snprintf(want, sizeof want, "net: bad count '%s' (1..100)\r\n", bad[i]);
		rc = run(line);
		expect(want, has(want) && rc == 1 && net_stub.calls_ping == 0 &&
		       !has("PING"));
	}
	reset();
	rc = run("net ping 1.2.3\r");
	expect("a bad address is refused before a probe",
	       has("net: bad address '1.2.3'\r\n") && rc == 1 && net_stub.calls_ping == 0);

	/* The default is 4, one probe a second, the f746 shape of the lines. */
	reset();
	rc = run("net ping 10.0.0.1\r");
	expect("default 4 probes: heading, replies, totals, exit 0",
	       net_stub.calls_ping == 4 && rc == 0 &&
	       has("PING 10.0.0.1, 4 probes:\r\n") && has("  reply 1: 4 ms\r\n") &&
	       has("  reply 4: 7 ms\r\n") &&
	       has("4/4 received, rtt min/avg/max 4/5/7 ms\r\n") &&
	       before("PING", "reply 1"));
	expect("each probe waits 1000 ms, to the address typed",
	       net_stub.last_timeout_ms == 1000u && net_stub.last_ip == 0x0A000001u);
	expect("three gaps between four probes", net_stub.sleeps == 3);
	reset();
	rc = run("net ping 10.0.0.1 1\r");
	expect("count 1: one probe, no gap", net_stub.calls_ping == 1 &&
	       net_stub.sleeps == 0 && rc == 0 && has("1/1 received"));
	reset();
	rc = run("net ping 10.0.0.1 100\r");
	expect("count 100 is taken", net_stub.calls_ping == 100 && rc == 0 &&
	       has("100/100 received"));

	/* N2 */
	reset();
	SEQ(net_stub.ping, NET_SVC_TIMEOUT);
	rc = run("net ping 10.0.0.1\r");
	expect("no reply at all: exit 1 and `0/4 received`",
	       rc == 1 && has("  probe 1: timeout\r\n") && has("0/4 received\r\n") &&
	       !has("rtt"));
	reset();
	SEQ(net_stub.ping, NET_SVC_REFUSED);
	rc = run("net ping 10.0.0.1 2\r");
	expect("NetX's refusal is a probe line and the run goes on",
	       rc == 1 && has("  probe 1: NetX error\r\n") &&
	       has("  probe 2: NetX error\r\n") && has("0/2 received"));
	reset();
	SEQ(net_stub.ping, NET_SVC_OK, NET_SVC_TIMEOUT, NET_SVC_OK, NET_SVC_TIMEOUT);
	rc = run("net ping 10.0.0.1\r");
	expect("some replies: exit 0 and the totals of the replies",
	       rc == 0 && has("2/4 received, rtt min/avg/max 4/5/6 ms"));

	/* A reason to stop on the first probe: no heading, no totals. */
	static const enum net_svc_status stops[] = {
		NET_SVC_DOWN, NET_SVC_LINK, NET_SVC_FAILED,
	};
	static const char *const stop_why[] = {
		"net: stub: down\r\n", "net: stub: link down\r\n", "net: stub: failed\r\n",
	};
	for (unsigned i = 0; i < 3; i++) {
		reset();
		seq(&net_stub.ping, 1, &stops[i]);
		rc = run("net ping 10.0.0.1\r");
		expect(stop_why[i], rc == 1 && has(stop_why[i]) && !has("PING") &&
		       !has("received") && net_stub.calls_ping == 1);
	}
	/* ... and on a later one: the totals so far, exit 1. */
	reset();
	SEQ(net_stub.ping, NET_SVC_OK, NET_SVC_LINK);
	rc = run("net ping 10.0.0.1\r");
	expect("a later LINK stops the run with the totals so far, exit 1",
	       rc == 1 && net_stub.calls_ping == 2 && has("  reply 1: 4 ms\r\n") &&
	       before("reply 1", "net: stub: link down") &&
	       before("net: stub: link down", "1/1 received"));
	reset();
	SEQ(net_stub.ping, NET_SVC_OK, NET_SVC_BUSY);
	rc = run("net ping 10.0.0.1\r");
	expect("a later BUSY stops the run (it is not waited for)",
	       rc == 1 && net_stub.calls_ping == 2 && has("net: stub: busy\r\n") &&
	       has("1/1 received"));

	/* The first probe waits out BUSY. */
	reset();
	SEQ(net_stub.ping, NET_SVC_BUSY, NET_SVC_BUSY, NET_SVC_OK);
	rc = run("net ping 10.0.0.1\r");
	expect("BUSY then a reply: the run goes on, the heading once",
	       rc == 0 && net_stub.calls_ping == 6 && has("4/4 received") &&
	       !has("busy"));
	reset();
	SEQ(net_stub.ping, NET_SVC_BUSY);
	{
		ULONG t0 = net_stub.now;

		rc = run("net ping 10.0.0.1\r");
		expect("BUSY throughout: 20 s, then the board's reason, no heading",
		       rc == 1 && has("net: stub: busy\r\n") && !has("PING") &&
		       !net_stub.runaway && net_stub.calls_ping == 200 &&
		       net_stub.now - t0 == 20000u);
	}
	/* A slow adapter does not stretch the deadline: 1.5 s a call + 0.1 s a
	   gap, so the 13th call is the first to end past 20 s. */
	reset();
	SEQ(net_stub.ping, NET_SVC_BUSY);
	net_stub.call_ms = 1500u;
	{
		ULONG t0 = net_stub.now;

		rc = run("net ping 10.0.0.1\r");
		expect("a slow adapter: still one 20 s deadline (13 calls)",
		       rc == 1 && !net_stub.runaway && net_stub.calls_ping == 13 &&
		       net_stub.now - t0 < 20000u + 1600u);
	}

	/* Ctrl+C */
	reset();
	SEQ(net_stub.ping, NET_SVC_BUSY);
	net_stub.ctrl_c_at_sleep = 3;
	rc = run("net ping 10.0.0.1\r");
	expect("Ctrl+C in the BUSY wait: nothing is called after it",
	       rc == CLI_DISPATCH_CANCELLED && net_stub.calls_ping == 3 && !has("PING"));
	reset();
	net_stub.ctrl_c_at_sleep = 2;
	rc = run("net ping 10.0.0.1 10\r");
	/* A Ctrl+C is the core's "cancelled" whatever the handler returns (and
	   what it prints after the Ctrl+C is dropped), so only the probes count. */
	expect("Ctrl+C between probes: no probe after it",
	       rc == CLI_DISPATCH_CANCELLED && net_stub.calls_ping == 2);
}

/* ---- net ip ------------------------------------------------------------------------- */

static void check_ip(void)
{
	int rc;

	reset();
	rc = run("net ip 10.0.0.5\r");
	expect("an address without /mask is refused before the adapter",
	       rc == 1 && has("net: bad address '10.0.0.5' (use a.b.c.d/mask)\r\n") &&
	       net_stub.calls_static == 0);
	reset();
	rc = run("net ip 10.0.0.5/24 10.0.0\r");
	expect("a bad gateway is refused before the adapter",
	       rc == 1 && has("net: bad gateway '10.0.0'\r\n") &&
	       net_stub.calls_static == 0);

	reset();
	net_stub.address.dhcp_mode = false;
	rc = run("net ip 10.0.0.5/24 10.0.0.1\r");
	expect("set: the arguments reach the adapter",
	       net_stub.last_ip == 0x0A000005u && net_stub.last_mask == 0xFFFFFF00u &&
	       net_stub.last_gw == 0x0A000001u);
	expect("set: the sentence, the notice, then the two lines, exit 0",
	       rc == 0 && net_stub.addressed == 1 &&
	       before("net: static address set\r\n", "ip:    192.168.1.10/24\r\n") &&
	       has("gw:    192.168.1.1 (static)\r\n"));
	reset();
	rc = run("net ip 10.0.0.5/24\r");
	expect("no gateway is 0", rc == 0 && net_stub.last_gw == 0u);

	reset();
	SEQ(net_stub.set_static, NET_SVC_REFUSED);
	rc = run("net ip 10.0.0.5/24\r");
	expect("REFUSED: `NetX refused the static address`, no notice",
	       rc == 1 && has("net: NetX refused the static address\r\n") &&
	       net_stub.addressed == 0 && !has("ip:"));
	reset();
	SEQ(net_stub.set_static, NET_SVC_DOWN);
	rc = run("net ip 10.0.0.5/24\r");
	expect("DOWN: the board's reason", rc == 1 && has("net: stub: down\r\n"));

	reset();
	SEQ(net_stub.set_static, NET_SVC_BUSY, NET_SVC_BUSY, NET_SVC_OK);
	rc = run("net ip 10.0.0.5/24\r");
	expect("BUSY twice then OK: called 3 times, applied once",
	       rc == 0 && net_stub.calls_static == 3 && net_stub.applied_static == 1);
	reset();
	SEQ(net_stub.set_static, NET_SVC_BUSY);
	rc = run("net ip 10.0.0.5/24\r");
	expect("BUSY throughout: the reason after 20 s, nothing applied",
	       rc == 1 && has("net: stub: busy\r\n") && net_stub.applied_static == 0 &&
	       net_stub.calls_static == 200 && !net_stub.runaway);
	reset();
	SEQ(net_stub.set_static, NET_SVC_BUSY);
	net_stub.ctrl_c_at_sleep = 2;
	rc = run("net ip 10.0.0.5/24\r");
	expect("Ctrl+C in the BUSY wait: no call after it, nothing applied",
	       rc == CLI_DISPATCH_CANCELLED && net_stub.calls_static == 2 && net_stub.applied_static == 0 &&
	       net_stub.addressed == 0);
}

/* ---- net dhcp ------------------------------------------------------------------------ */

static void check_dhcp(void)
{
	int rc;

	reset();
	rc = run("net dhcp now\r");
	expect("an extra argument is refused before the adapter",
	       rc != 0 && net_stub.calls_dhcp == 0);

	reset();
	net_stub.addr_valid_late = 1;
	net_stub.addr_valid_at = net_stub.now + 5000u;
	rc = run("net dhcp\r");
	expect("a lease on the way: the sentence, the notice, two lines, exit 0",
	       rc == 0 && net_stub.addressed == 1 &&
	       before("net: acquiring an address via DHCP (up to 30 s, Ctrl+C to stop)...\r\n",
	              "ip:    192.168.1.10/24\r\n") &&
	       has("gw:    192.168.1.1 (dhcp)\r\n"));

	reset();
	net_stub.addr_valid_late = 1;
	net_stub.addr_valid_at = net_stub.now + 3600000u;
	{
		ULONG t0 = net_stub.now;

		rc = run("net dhcp\r");
		expect("no lease: `no DHCP lease after 30 s`, exit 1, no notice (N3)",
		       rc == 1 && has("net: no DHCP lease after 30 s\r\n") &&
		       net_stub.addressed == 0 && net_stub.now - t0 == 30000u);
	}
	reset();
	net_stub.addr_valid_late = 1;
	net_stub.addr_valid_at = net_stub.now + 3600000u;
	net_stub.call_ms = 900u;
	{
		ULONG t0 = net_stub.now;

		rc = run("net dhcp\r");
		expect("a slow adapter does not stretch the 30 s",
		       rc == 1 && has("no DHCP lease after 30 s") &&
		       net_stub.now - t0 < 30000u + 900u + 900u + 100u);
	}

	reset();
	SEQ(net_stub.dhcp, NET_SVC_RENEWED);
	rc = run("net dhcp\r");
	expect("RENEWED: the renew sentence, then the two lines",
	       rc == 0 && before("net: renewing the existing lease (kept meanwhile)...\r\n",
	                         "ip:    192.168.1.10/24") && !has("acquiring"));
	reset();
	SEQ(net_stub.dhcp, NET_SVC_REFUSED);
	rc = run("net dhcp\r");
	expect("REFUSED: `NetX DHCP client would not start`",
	       rc == 1 && has("net: NetX DHCP client would not start\r\n") &&
	       net_stub.calls_addr == 0);
	reset();
	SEQ(net_stub.dhcp, NET_SVC_FAILED);
	rc = run("net dhcp\r");
	expect("FAILED: the board's reason", rc == 1 && has("net: stub: failed\r\n"));
	reset();
	SEQ(net_stub.dhcp, NET_SVC_BUSY, NET_SVC_OK);
	rc = run("net dhcp\r");
	expect("BUSY then OK: started once", rc == 0 && net_stub.calls_dhcp == 2 &&
	       net_stub.applied_dhcp == 1);

	reset();
	net_stub.addr_valid_late = 1;
	net_stub.addr_valid_at = net_stub.now + 3600000u;
	net_stub.ctrl_c_at_sleep = 3;
	rc = run("net dhcp\r");
	expect("Ctrl+C in the lease wait: cancelled, no notice, started once",
	       rc == CLI_DISPATCH_CANCELLED && net_stub.addressed == 0 && net_stub.calls_dhcp == 1 &&
	       net_stub.calls_addr == 3);
	reset();
	SEQ(net_stub.addr, NET_SVC_DOWN);
	rc = run("net dhcp\r");
	expect("the stack goes away in the wait: its reason, exit 1",
	       rc == 1 && has("net: stub: down\r\n") && net_stub.calls_addr == 1);
}

/* ---- net info -------------------------------------------------------------------------- */

static void check_info(void)
{
	int rc;

	reset();
	net_stub.address.lease_stale = true;
	net_stub.lines[0] = "a: 1\r\n";
	net_stub.lines[1] = "b: 2\r\n";
	net_stub.nlines = 2;
	rc = run("net info\r");
	expect("ip then gw then the board's lines, exit 0",
	       rc == 0 && before("ip:    192.168.1.10/24\r\n", "gw:    192.168.1.1 (dhcp)") &&
	       before("gw:    192.168.1.1 (dhcp)", "a: 1\r\nb: 2\r\n") &&
	       net_stub.lines_written == 2 && net_stub.write_rc_bad == 0);
	expect("STALE after the gw",
	       has("gw:    192.168.1.1 (dhcp)  <-- STALE (link went down; run `net dhcp`)\r\n"));
	reset();
	run("net info\r");
	expect("no STALE without the mark",
	       has("gw:    192.168.1.1 (dhcp)\r\n") && !has("STALE"));

	reset();
	net_stub.address.gw = 0u;
	net_stub.address.dhcp_mode = false;
	run("net info\r");
	expect("gw 0 is `none (static)`", has("gw:    none (static)\r\n"));
	reset();
	net_stub.address.ip_valid = false;
	run("net info\r");
	expect("no address under DHCP: one line", has("ip:    none (dhcp, no lease)\r\n") &&
	       !has("gw:"));
	reset();
	net_stub.address.ip_valid = false;
	net_stub.address.dhcp_mode = false;
	run("net info\r");
	expect("no address, static: `none (unset)`", has("ip:    none (unset)\r\n"));

	reset();
	SEQ(net_stub.addr, NET_SVC_DOWN);
	net_stub.lines[0] = "a: 1\r\n";
	net_stub.nlines = 1;
	rc = run("net info\r");
	expect("DOWN: `ip: none (reason)`, the board's lines, exit 0",
	       rc == 0 && before("ip:    none (stub: down)\r\n", "a: 1\r\n"));
	reset();
	SEQ(net_stub.addr, NET_SVC_BUSY);
	rc = run("net info\r");
	expect("BUSY is not waited for", rc == 0 && net_stub.calls_addr == 1 &&
	       net_stub.sleeps == 0 && has("ip:    none (stub: busy)\r\n"));

	reset();
	net_stub.lines[0] = "a: 1\r\n";
	net_stub.lines[1] = "b: 2\r\n";
	net_stub.lines[2] = "c: 3\r\n";
	net_stub.nlines = 3;
	net_stub.fail_after_lines = 1;
	rc = run("net info\r");
	expect("a write that fails after line 1 stops the lines, exit 1",
	       rc == 1 && net_stub.lines_written == 1);

	reset();
	rc = run("net info x\r");
	expect("an extra argument is refused before the adapter",
	       rc != 0 && net_stub.calls_addr == 0 && net_stub.calls_lines == 0);
}

#endif /* !NET_TEST_SHAPE_DUP */

int main(void)
{
	printf("test_net_cmd (%s):\n", SHAPE);
	reset();
	check_table();
#if !defined(NET_TEST_SHAPE_DUP)
	check_ping();
	check_ip();
	check_dhcp();
	check_info();
#endif
	if (failures != 0) {
		printf("test_net_cmd: %d failure(s)\n", failures);
		return 1;
	}
	printf("test_net_cmd: all passed\n");
	return 0;
}
