/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Host test: the shared boot-time configuration steps (shell/cmds/kv_cfg.c,
 * issue #214) over a stand-in store, stand-in address operations and a stand-in
 * telnet console, all defined here; cli_parse_ipv4() is the real one.  Built
 * twice: with the board default NONE (wio-lite-ai's) and with DHCP
 * (-DKV_CFG_TEST_DEFAULT_DHCP, what f746g-disco will declare).
 *
 * What it holds, since nothing on a console shows it:
 *  - the telnet port is handed over exactly once on every path: the stored one,
 *    or 23 for a store that did not open, a missing key, a key of another type,
 *    an unreadable one, 0 and anything over 65535;
 *  - net.mode: what is applied for a missing, mistyped, too long or unreadable
 *    key, `manual`, and a store that did not open, under each default;
 *  - dhcp: the source is set once, the wait ends at the first address or after
 *    30 s of 250 ms polls; static: all three keys parsed and handed over, each
 *    refusal reported, and on the DHCP default followed by DHCP, once;
 *  - every line reported, word for word, at its weight (the lines wio-lite-ai's
 *    configuration thread wrote to `dmesg` before they were shared).
 */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "kv.h"
#include "kv_cfg.h"
#include "net_core.h"
#include "net_tty.h"

static int failures;

#define CHECK(cond)                                                         \
	do {                                                                    \
		if (!(cond)) {                                                      \
			fprintf(stderr, "test_kv_cfg: %s:%d: %s\n", __FILE__, __LINE__, \
			        #cond);                                                 \
			failures++;                                                     \
		}                                                                   \
	} while (0)

/* ---- the board's facts and its log ------------------------------------------ */

#define REPORT_MAX 16
static struct { unsigned level; char text[160]; } rep[REPORT_MAX];
static int nrep;

static void t_report(unsigned level, const char *fmt, ...)
{
	va_list ap;

	if (nrep >= REPORT_MAX)
		return;
	rep[nrep].level = level;
	va_start(ap, fmt);
	vsnprintf(rep[nrep].text, sizeof rep[nrep].text, fmt, ap);
	va_end(ap);
	nrep++;
}

const struct kv_cfg_facts kv_cfg_board_facts = {
	.report = t_report,
#ifdef KV_CFG_TEST_DEFAULT_DHCP
	.net_default = KV_CFG_NET_DHCP,
#else
	.net_default = KV_CFG_NET_NONE,
#endif
};

#ifdef KV_CFG_TEST_DEFAULT_DHCP
#define DFLT_DHCP 1
#else
#define DFLT_DHCP 0
#endif

/* ---- the stand-in store -------------------------------------------------- */

#define KEYS 8
static struct {
	int ready, init_rc;
	unsigned n;
	struct { const char *key; int rc; uint8_t type; char s[64]; uint32_t u; } k[KEYS];
} kv;

static void kv_reset(void)
{
	memset(&kv, 0, sizeof kv);
	kv.ready = 1;
}

/* A key that answers rc, or holds a value of @p type. */
static void put(const char *key, int rc, uint8_t type, const char *s, uint32_t u)
{
	kv.k[kv.n].key = key;
	kv.k[kv.n].rc = rc;
	kv.k[kv.n].type = type;
	if (s)
		strcpy(kv.k[kv.n].s, s);
	kv.k[kv.n].u = u;
	kv.n++;
}

static int look(const char *key, uint8_t want, unsigned *at)
{
	unsigned i;

	if (!kv.ready)
		return KV_ERR_STATE;
	for (i = 0; i < kv.n; i++) {
		if (strcmp(kv.k[i].key, key) != 0)
			continue;
		if (kv.k[i].rc != KV_OK)
			return kv.k[i].rc;
		if (kv.k[i].type != want)
			return KV_ERR_TYPE;
		*at = i;
		return KV_OK;
	}
	return KV_ERR_NOTFOUND;
}

int kv_init(void) { return kv.init_rc; }
int kv_state(void) { return kv.ready ? KV_STATE_READY : KV_STATE_CORRUPT; }
const char *kv_state_str(void) { return "TESTSTATE"; }

int kv_get_bool(const char *key, int *out)
{
	unsigned i;
	int rc = look(key, KV_TYPE_BOOL, &i);

	if (rc == KV_OK)
		*out = (int)kv.k[i].u;
	return rc;
}

int kv_get_u32(const char *key, uint32_t *out)
{
	unsigned i;
	int rc = look(key, KV_TYPE_U32, &i);

	if (rc == KV_OK)
		*out = kv.k[i].u;
	return rc;
}

int kv_get_str(const char *key, char *buf, uint32_t buflen)
{
	unsigned i;
	int rc = look(key, KV_TYPE_STR, &i);

	if (rc != KV_OK)
		return rc;
	if (strlen(kv.k[i].s) + 1u > buflen)
		return KV_ERR_TRUNC;
	strcpy(buf, kv.k[i].s);
	return KV_OK;
}

/* ---- the stand-in net and telnet ----------------------------------------- */

static struct {
	int ports;                /* net_tty_port_set calls */
	uint16_t port;
	int dhcp_calls, static_calls, addr_calls;
	enum net_svc_status dhcp_rc, static_rc;
	int ip_after;             /* addr_get calls until an address, -1 never */
	uint32_t ip, mask, gw;
	unsigned long slept;      /* ticks */
} net;

static void net_reset(void)
{
	memset(&net, 0, sizeof net);
	net.dhcp_rc = NET_SVC_OK;
	net.static_rc = NET_SVC_OK;
	net.ip_after = 0;
}

void net_tty_port_set(uint16_t port)
{
	net.ports++;
	net.port = port;
}

void net_core_dhcp_source(struct net_svc_result *res)
{
	net.dhcp_calls++;
	res->status = net.dhcp_rc;
	res->why = NULL;
}

void net_core_set_static(uint32_t ip, uint32_t mask, uint32_t gw,
                         struct net_svc_result *res)
{
	net.static_calls++;
	net.ip = ip; net.mask = mask; net.gw = gw;
	res->status = net.static_rc;
	res->why = NULL;
}

void net_core_addr_get(struct net_svc_addr *out, struct net_svc_result *res)
{
	memset(out, 0, sizeof *out);
	res->status = NET_SVC_OK;
	res->why = NULL;
	out->ip_valid = (net.ip_after >= 0 && net.addr_calls >= net.ip_after);
	net.addr_calls++;
}

UINT tx_thread_sleep(ULONG ticks)
{
	net.slept += ticks;
	return 0;
}

/* ---- helpers --------------------------------------------------------------- */

static void reset(void)
{
	kv_reset();
	net_reset();
	nrep = 0;
}

static int said(unsigned level, const char *text)
{
	int i;

	for (i = 0; i < nrep; i++)
		if (rep[i].level == level && strcmp(rep[i].text, text) == 0)
			return 1;
	return 0;
}

/* ---- tests ----------------------------------------------------------------- */

static void t_open(void)
{
	reset();
	CHECK(kv_cfg_open() == 0 && nrep == 0);
	kv.init_rc = KV_ERR_STATE;
	CHECK(kv_cfg_open() == -1);
	CHECK(nrep == 1 && said(KV_CFG_REPORT_WRN, "configuration store unavailable: TESTSTATE"));
}

static void t_port_is(uint16_t want, const char *line)
{
	kv_cfg_tty_port();
	CHECK(net.ports == 1 && net.port == want);
	if (line)
		CHECK(nrep == 1 && said(KV_CFG_REPORT_WRN, line));
	else
		CHECK(nrep == 0);
}

static void t_port(void)
{
	reset(); kv.ready = 0;
	t_port_is(23, NULL);                         /* store not open: silent */
	reset();
	t_port_is(23, NULL);                         /* missing: silent */
	reset(); put("net.shell.port", KV_OK, KV_TYPE_U32, NULL, 2323u);
	t_port_is(2323, NULL);
	reset(); put("net.shell.port", KV_OK, KV_TYPE_U32, NULL, 65535u);
	t_port_is(65535, NULL);
	reset(); put("net.shell.port", KV_OK, KV_TYPE_STR, "2323", 0u);
	t_port_is(23, "net.shell.port: ignored (not a u32)");
	reset(); put("net.shell.port", KV_ERR_IO, KV_TYPE_U32, NULL, 0u);
	t_port_is(23, "net.shell.port: ignored (unreadable)");
	reset(); put("net.shell.port", KV_OK, KV_TYPE_U32, NULL, 0u);
	t_port_is(23, "net.shell.port=0 is not a port -- telnet stays on 23");
	reset(); put("net.shell.port", KV_OK, KV_TYPE_U32, NULL, 65536u);
	t_port_is(23, "net.shell.port=65536 is not a port -- telnet stays on 23");
}

static void t_mode_is(int want_rc, const char *want_mode, const char *line)
{
	char mode[16];

	memset(mode, 'x', sizeof mode);
	CHECK(kv_cfg_net_mode(mode, sizeof mode) == want_rc);
	if (want_mode)
		CHECK(strcmp(mode, want_mode) == 0);
	if (line)
		CHECK(nrep == 1 && said(KV_CFG_REPORT_WRN, line));
	else
		CHECK(nrep == 0);
}

static void t_mode(void)
{
	reset();
	t_mode_is(DFLT_DHCP, DFLT_DHCP ? "dhcp" : "", NULL);       /* missing */
	reset(); kv.ready = 0;
	t_mode_is(DFLT_DHCP, DFLT_DHCP ? "dhcp" : "", NULL);       /* not open */
	reset(); put("net.mode", KV_OK, KV_TYPE_STR, "manual", 0u);
	t_mode_is(0, "manual", NULL);
	reset(); put("net.mode", KV_OK, KV_TYPE_STR, "static", 0u);
	t_mode_is(1, "static", NULL);
	reset(); put("net.mode", KV_OK, KV_TYPE_STR, "dhcp", 0u);
	t_mode_is(1, "dhcp", NULL);
	reset(); put("net.mode", KV_OK, KV_TYPE_U32, NULL, 1u);
	t_mode_is(DFLT_DHCP, DFLT_DHCP ? "dhcp" : "", "net.mode: ignored (not a string)");
	reset(); put("net.mode", KV_OK, KV_TYPE_STR, "a-much-too-long-mode", 0u);
	t_mode_is(DFLT_DHCP, DFLT_DHCP ? "dhcp" : "", "net.mode: ignored (too long)");
	reset(); put("net.mode", KV_ERR_FORMAT, KV_TYPE_STR, NULL, 0u);
	t_mode_is(DFLT_DHCP, DFLT_DHCP ? "dhcp" : "", "net.mode: ignored (unreadable)");
	/* None of these touched the network. */
	CHECK(net.dhcp_calls == 0 && net.static_calls == 0);
}

static void t_dhcp(void)
{
	reset(); net.ip_after = 5;
	kv_cfg_net_apply("dhcp");
	CHECK(net.dhcp_calls == 1 && net.slept == 5u * KV_CFG_POLL_MS);
	CHECK(nrep == 1 && said(KV_CFG_REPORT_INF, "DHCP lease acquired"));

	reset(); net.ip_after = -1;
	kv_cfg_net_apply("dhcp");
	CHECK(net.dhcp_calls == 1 && net.slept == KV_CFG_DHCP_WAIT_MS);
	CHECK(net.addr_calls == (int)(KV_CFG_DHCP_WAIT_MS / KV_CFG_POLL_MS) + 1);
	CHECK(nrep == 1 && said(KV_CFG_REPORT_WRN, "no DHCP lease within 30 s"));

	reset(); net.dhcp_rc = NET_SVC_FAILED;
	kv_cfg_net_apply("dhcp");
	CHECK(net.dhcp_calls == 1 && net.addr_calls == 0);
	CHECK(nrep == 1 && said(KV_CFG_REPORT_WRN, "DHCP cannot be the source (no DHCP client)"));
}

/* After a refusal: the default DHCP once, or nothing. */
static void after_refusal(const char *line)
{
	CHECK(said(KV_CFG_REPORT_WRN, line));
	CHECK(net.dhcp_calls == DFLT_DHCP);
	CHECK(nrep == 1 + DFLT_DHCP);
	if (DFLT_DHCP)
		CHECK(said(KV_CFG_REPORT_INF, "DHCP lease acquired"));
}

static void t_static(void)
{
	reset();
	put("net.ip", KV_OK, KV_TYPE_STR, "192.168.1.20", 0u);
	put("net.mask", KV_OK, KV_TYPE_STR, "255.255.255.0", 0u);
	put("net.gw", KV_OK, KV_TYPE_STR, "192.168.1.1", 0u);
	kv_cfg_net_apply("static");
	CHECK(net.static_calls == 1 && net.dhcp_calls == 0);
	CHECK(net.ip == 0xC0A80114u && net.mask == 0xFFFFFF00u && net.gw == 0xC0A80101u);
	CHECK(nrep == 1 && said(KV_CFG_REPORT_INF, "static address applied"));

	reset();
	put("net.ip", KV_OK, KV_TYPE_STR, "192.168.1.20", 0u);
	put("net.mask", KV_OK, KV_TYPE_STR, "255.255.255.0", 0u);
	kv_cfg_net_apply("static");
	CHECK(net.static_calls == 0);
	after_refusal("net.mode=static needs net.ip, net.mask and net.gw");

	reset();
	put("net.ip", KV_OK, KV_TYPE_STR, "192.168.1", 0u);
	put("net.mask", KV_OK, KV_TYPE_STR, "255.255.255.0", 0u);
	put("net.gw", KV_OK, KV_TYPE_STR, "192.168.1.1", 0u);
	kv_cfg_net_apply("static");
	CHECK(net.static_calls == 0);
	after_refusal("net.ip/mask/gw: not all are dotted quads");

	reset();
	put("net.ip", KV_OK, KV_TYPE_STR, "10.0.0.2", 0u);
	put("net.mask", KV_OK, KV_TYPE_STR, "255.0.0.0", 0u);
	put("net.gw", KV_OK, KV_TYPE_STR, "10.0.0.1", 0u);
	net.static_rc = NET_SVC_FAILED;
	kv_cfg_net_apply("static");
	CHECK(net.static_calls == 1);
	after_refusal("the host stack refused the static address");

	reset();
	kv_cfg_net_apply("auto");
	CHECK(net.static_calls == 0);
	after_refusal("net.mode=auto is not one of dhcp/static/manual");
}

static void t_bool(void)
{
	reset(); put("wifi.enable", KV_OK, KV_TYPE_BOOL, NULL, 1u);
	CHECK(kv_cfg_bool("wifi.enable", 0) == 1 && nrep == 0);
	reset();
	CHECK(kv_cfg_bool("wifi.enable", 0) == 0 && nrep == 0);
	reset(); put("wifi.enable", KV_OK, KV_TYPE_STR, "yes", 0u);
	CHECK(kv_cfg_bool("wifi.enable", 0) == 0 &&
	      said(KV_CFG_REPORT_WRN, "wifi.enable: ignored (not a bool)"));
	reset(); put("wifi.enable", KV_ERR_IO, KV_TYPE_BOOL, NULL, 0u);
	CHECK(kv_cfg_bool("wifi.enable", 1) == 1 &&
	      said(KV_CFG_REPORT_WRN, "wifi.enable: ignored (unreadable)"));
	{
		char b[8];

		reset(); put("wifi.ssid", KV_OK, KV_TYPE_STR, "home", 0u);
		CHECK(kv_cfg_str("wifi.ssid", b, sizeof b) == 1 && strcmp(b, "home") == 0);
		reset();
		memset(b, 'x', sizeof b);
		CHECK(kv_cfg_str("wifi.ssid", b, sizeof b) == 0 && b[0] == '\0' && nrep == 0);
	}
}

int main(void)
{
	t_open();
	t_port();
	t_mode();
	t_dhcp();
	t_static();
	t_bool();
	if (failures != 0) {
		fprintf(stderr, "test_kv_cfg (default %s): %d failure(s)\n",
		        DFLT_DHCP ? "dhcp" : "none", failures);
		return 1;
	}
	printf("test_kv_cfg (default %s): OK\n", DFLT_DHCP ? "dhcp" : "none");
	return 0;
}
