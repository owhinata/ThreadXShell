/* shared-command: optional BOARD_HAS_KV_NETX */
/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    kv_cfg.c
 * @brief   The shared boot-time configuration steps (owhinata/wio-lite-ai#37;
 *          moved out of wio-lite-ai's src/kv_boot.c by issue #214).
 *
 * See kv_cfg.h.  EVERY STEP IS INDEPENDENT AND FAIL-SOFT.  A missing key means
 * "do not do this" (or the board's default), never "guess"; a step that fails is
 * reported and the next one still runs; and none of them can stop the shell
 * coming up, because every one of these actions can be typed by hand afterwards.
 * A configuration store that turns a mistyped setting into a board you cannot
 * talk to would be worse than no store at all.
 *
 * [!] OWNS NO STORAGE, and has no board in it.  The lines it reports are the
 * ones wio-lite-ai's configuration thread wrote to its log before it was shared.
 */
#include "kv_cfg.h"

#include "cli.h"        /* cli_parse_ipv4() */
#include "kv.h"
#include "net_core.h"   /* the shared address / DHCP operations (issue #220) */
#include "net_tty.h"    /* the telnet console's port (issue #221) */

#include "tx_api.h"     /* tx_thread_sleep(): the DHCP wait */

#include <string.h>

#define KV_CFG_FX  kv_cfg_board_facts

int kv_cfg_open(void)
{
	if (kv_init() != KV_OK) {
		KV_CFG_FX.report(KV_CFG_REPORT_WRN, "configuration store unavailable: %s",
		                 kv_state_str());
		return -1;
	}
	return 0;
}

/* ------------------------------------------------------------------ *
 *  Setting readers
 * ------------------------------------------------------------------ */

int kv_cfg_bool(const char *key, int dflt)
{
	int v;
	int rc = kv_get_bool(key, &v);

	if (rc == KV_OK)
		return v;
	if (rc != KV_ERR_NOTFOUND)
		KV_CFG_FX.report(KV_CFG_REPORT_WRN, "%s: ignored (%s)", key,
		                 (rc == KV_ERR_TYPE) ? "not a bool" : "unreadable");
	return dflt;
}

uint32_t kv_cfg_u32(const char *key, uint32_t dflt)
{
	uint32_t v;
	int rc = kv_get_u32(key, &v);

	if (rc == KV_OK)
		return v;
	if (rc != KV_ERR_NOTFOUND)
		KV_CFG_FX.report(KV_CFG_REPORT_WRN, "%s: ignored (%s)", key,
		                 (rc == KV_ERR_TYPE) ? "not a u32" : "unreadable");
	return dflt;
}

int kv_cfg_str(const char *key, char *buf, uint32_t buflen)
{
	int rc = kv_get_str(key, buf, buflen);

	if (rc == KV_OK)
		return 1;
	if (rc != KV_ERR_NOTFOUND)
		KV_CFG_FX.report(KV_CFG_REPORT_WRN, "%s: ignored (%s)", key,
		                 (rc == KV_ERR_TYPE) ? "not a string" :
		                 (rc == KV_ERR_TRUNC) ? "too long" : "unreadable");
	buf[0] = '\0';
	return 0;
}

/* ------------------------------------------------------------------ *
 *  The telnet console's port
 * ------------------------------------------------------------------ */

void kv_cfg_tty_port(void)
{
	uint32_t port = NET_TTY_PORT_DEFAULT;

	/* A store that did not open has nothing to say, and has already said so
	 * (kv_cfg_open()): the default, without a line per key. */
	if (kv_state() == KV_STATE_READY)
		port = kv_cfg_u32("net.shell.port", NET_TTY_PORT_DEFAULT);
	if (port == 0u || port > 0xFFFFu) {
		KV_CFG_FX.report(KV_CFG_REPORT_WRN,
		                 "net.shell.port=%lu is not a port -- telnet stays on %u",
		                 (unsigned long)port, (unsigned)NET_TTY_PORT_DEFAULT);
		port = NET_TTY_PORT_DEFAULT;
	}
	net_tty_port_set((uint16_t)port);
}

/* ------------------------------------------------------------------ *
 *  net.mode
 * ------------------------------------------------------------------ */

int kv_cfg_net_mode(char *mode, size_t size)
{
	int have = 0;

	/* Only a store that opened is read; the readers would otherwise report
	 * every key as unreadable. */
	if (kv_state() == KV_STATE_READY)
		have = kv_cfg_str("net.mode", mode, (uint32_t)size);
	if (!have) {
		if (KV_CFG_FX.net_default != KV_CFG_NET_DHCP || size < sizeof "dhcp") {
			mode[0] = '\0';
			return 0;
		}
		/* Said once, so `dmesg` tells a DHCP the board chose apart from one
		 * net.mode asked for (a board whose default is NONE never says it). */
		KV_CFG_FX.report(KV_CFG_REPORT_INF,
		                 "no usable net.mode -- DHCP, this board's default");
		memcpy(mode, "dhcp", sizeof "dhcp");
		return 1;
	}
	return strcmp(mode, "manual") != 0;
}

/* Wait for the host stack to hold an address.  Returns 1 when it does. */
static int kv_cfg_wait_ip(unsigned budget_ms)
{
	struct net_svc_addr a;
	struct net_svc_result r;
	unsigned waited = 0u;

	for (;;) {
		net_core_addr_get(&a, &r);
		if (r.status == NET_SVC_OK && a.ip_valid)
			return 1;
		if (waited >= budget_ms)
			return 0;
		tx_thread_sleep(KV_CFG_POLL_MS);
		waited += KV_CFG_POLL_MS;
	}
}

static void kv_cfg_dhcp(void)
{
	struct net_svc_result r;

	/* Make DHCP the source and let the shared link worker start the
	 * client once the link is up (issue #220).  Unlike `net dhcp` it is not
	 * refused while the link record is still catching up with the
	 * association, so the order does not rest on thread priorities. */
	net_core_dhcp_source(&r);
	if (r.status != NET_SVC_OK) {
		KV_CFG_FX.report(KV_CFG_REPORT_WRN, "DHCP cannot be the source (no DHCP client)");
		return;
	}
	if (!kv_cfg_wait_ip(KV_CFG_DHCP_WAIT_MS)) {
		KV_CFG_FX.report(KV_CFG_REPORT_WRN, "no DHCP lease within %u s",
		                 KV_CFG_DHCP_WAIT_MS / 1000u);
		return;
	}
	KV_CFG_FX.report(KV_CFG_REPORT_INF, "DHCP lease acquired");
}

/* A mode that could not be applied: the board's default, if it has one. */
static void kv_cfg_fall_back(void)
{
	if (KV_CFG_FX.net_default == KV_CFG_NET_DHCP) {
		KV_CFG_FX.report(KV_CFG_REPORT_INF,
		                 "falling back to DHCP, this board's default");
		kv_cfg_dhcp();
	}
}

void kv_cfg_net_apply(const char *mode)
{
	char ip[16], mask[16], gw[16];
	struct net_svc_result r;
	uint32_t a, m, g;

	if (strcmp(mode, "dhcp") == 0) {
		kv_cfg_dhcp();
	} else if (strcmp(mode, "static") == 0) {
		/* All three or none: a static address with a guessed netmask is a
		 * quietly broken network, so an incomplete configuration is refused
		 * rather than completed with defaults. */
		if (!kv_cfg_str("net.ip", ip, sizeof ip) ||
		    !kv_cfg_str("net.mask", mask, sizeof mask) ||
		    !kv_cfg_str("net.gw", gw, sizeof gw)) {
			KV_CFG_FX.report(KV_CFG_REPORT_WRN,
			                 "net.mode=static needs net.ip, net.mask and net.gw");
			kv_cfg_fall_back();
			return;
		}
		if (cli_parse_ipv4(ip, &a) != 0 || cli_parse_ipv4(mask, &m) != 0 ||
		    cli_parse_ipv4(gw, &g) != 0) {
			KV_CFG_FX.report(KV_CFG_REPORT_WRN,
			                 "net.ip/mask/gw: not all are dotted quads");
			kv_cfg_fall_back();
			return;
		}
		net_core_set_static(a, m, g, &r);
		if (r.status != NET_SVC_OK) {
			KV_CFG_FX.report(KV_CFG_REPORT_WRN,
			                 "the host stack refused the static address");
			kv_cfg_fall_back();
			return;
		}
		KV_CFG_FX.report(KV_CFG_REPORT_INF, "static address applied");
	} else {
		KV_CFG_FX.report(KV_CFG_REPORT_WRN,
		                 "net.mode=%s is not one of dhcp/static/manual", mode);
		kv_cfg_fall_back();
	}
}
