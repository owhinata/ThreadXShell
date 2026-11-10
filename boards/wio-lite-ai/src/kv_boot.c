/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Wio Lite AI ThreadX Shell Project
 */
/**
 * @file    kv_boot.c
 * @brief   The configuration thread: opens the KV store and applies it
 * (owhinata/wio-lite-ai#37).
 *
 * A thread exists here for one reason: this work waits on hardware.  Opening the
 * store can erase (a first boot formats a blank partition), associating blocks on
 * the module for up to 22 s, and a DHCP lease takes seconds -- all of which need a
 * scheduler.  tx_application_define() runs before there is one, so everything in
 * it must be pure object creation; the work that has to wait belongs on a thread,
 * and this is that thread.
 *
 * The steps that are not this board's -- opening the store, the telnet port,
 * net.mode, and the setting readers -- are the shared shell/cmds/kv_cfg.c (issue
 * #214); this thread owns the order and calls them, and keeps what only this
 * board does: the radio, and "net.mode waits for the WiFi session".
 *
 * WHAT IT DOES, and what each step is allowed to assume:
 *
 *   kv_init()                  open the store (may format a blank partition)
 *   net.shell.port     the telnet console's port     : 23 (issue #221)
 *   wifi.enable      ? power the radio               : leave it off
 *   wifi.autoconnect ? associate with wifi.ssid      : leave it to the operator
 *   net.mode           dhcp / static / manual
 *
 * The telnet console is resident from boot (issue #221) and is not armed from here any
 * more: `net.shell.autoarm` is not read, whatever it holds.  It only waits for its port,
 * which this thread hands over ONCE, first -- the configured one, or 23 when the key is
 * missing, unreadable or not a port, or when the store does not open.
 *
 * EVERY STEP IS INDEPENDENT AND FAIL-SOFT.  A missing key means "do not do this",
 * never "guess"; a step that fails is logged and the next one still runs; and none
 * of them can stop the shell coming up, because every one of these actions can be
 * typed by hand afterwards.  A configuration store that turns a mistyped setting
 * into a board you cannot talk to would be worse than no store at all.
 *
 * WHY THE OUTPUT GOES THROUGH A SHELL INSTANCE: the wifi/net modules this drives
 * report through `struct cli_instance`, and there is no console attached at boot.
 * Rather than make four modules tolerate a NULL instance, the thread owns an
 * instance on the log transport (shell/backend/cli_backend_log.h), so the whole
 * account of what the sequence did lands in `dmesg` -- which is the only place it
 * could be read afterwards anyway.  cli_init() without cli_start(): there is
 * nothing to read, so the instance needs no thread of its own.
 *
 * One known wart in that arrangement: the association claims the console through
 * rtl_link_begin(), and cli_console_claim() raises a GLOBAL cli_xfer_active, not a
 * per-instance one.  So for the length of a boot association every console's
 * TX-wait path stops draining RX -- a Ctrl+C typed on USB CDC during those seconds
 * is noticed late.  It is bounded and it only happens when autoconnect is on, so it
 * is accepted here rather than fixed by changing what that flag means; if a second
 * unattended claimant ever appears, that is the point to make the flag per-console.
 *
 * Priority 15: above the shell (16) so the configuration is settled before a user
 * can reasonably type, but below the USB pump (8) and the IWDG petter (5), neither
 * of which may ever wait behind a flash erase or a 22 s association.
 */
/* Before anything else: cli_backend_log.h pulls in log.h, which defaults LOG_TAG
 * when it is not already set -- so this has to come first to win. */
#define LOG_TAG "cfg"
#include "log.h"

#include "kv.h"        /* the shared store (svc/kv, issue #214) */
#include "kv_boot.h"
#include "kv_cfg.h"    /* the shared boot steps (shell/cmds, issue #214) */

#include "cli.h"
#include "cli_backend_log.h"
#include "link_bridge.h"
#include "rtl8720.h"
#include "wifi_auto.h"
#include "wifi_connect.h"
#include "wifi_rpc.h"

#include "tx_api.h"

#include <stdarg.h>
#include <string.h>
#include "mem_sections.h"  /* DTCM_BSS: CPU-only data out of AXI-SRAM (issue #46) */

#define KV_BOOT_PRIORITY    15u

/* 4 KB.  A kv_get_str() costs about 700 bytes of stack on its own -- a whole record
 * buffer plus the decoded copy -- and this thread then calls the association
 * sequence, which nests several modules' frames underneath it.  The shell threads
 * that run the same sequence get 4 KB; there is no reason for this one to have
 * less, and a stack overflow here would be a boot-time fault with no console up. */
#define KV_BOOT_STACK_SIZE  4096u

static TX_THREAD kv_boot_thread;
static UCHAR     kv_boot_stack[KV_BOOT_STACK_SIZE] DTCM_BSS __attribute__((aligned(8)));

/* The console this sequence reports through: every line ends up in `dmesg`. */
CLI_BACKEND_LOG_DEFINE(kv_boot_tr, "cfg");
static struct cli_instance kv_boot_sh = {
	.tr     = &kv_boot_tr,
	.prompt = "cfg> ",
	/* No stack: cli_start() is never called (see the file header), so the
	 * instance thread that would use one does not exist. */
};

/* ------------------------------------------------------------------ *
 *  The shared steps' facts (shell/cmds/kv_cfg.h)
 * ------------------------------------------------------------------ */

/* Their lines go to the log under this file's tag, at the levels they were
 * written at here before they were shared. */
_Static_assert(KV_CFG_REPORT_ERR == LOG_LEVEL_ERR && KV_CFG_REPORT_WRN == LOG_LEVEL_WRN &&
               KV_CFG_REPORT_INF == LOG_LEVEL_INF,
               "kv_cfg report weights must be this board's log levels");

static void kv_boot_report(unsigned level, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	log_vwrite(level, LOG_TAG, fmt, ap);
	va_end(ap);
}

const struct kv_cfg_facts kv_cfg_board_facts = {
	.report      = kv_boot_report,
	/* No net.mode: leave the address to the operator. */
	.net_default = KV_CFG_NET_NONE,
};

/* ------------------------------------------------------------------ *
 *  Steps
 * ------------------------------------------------------------------ */

static void cfg_wifi(void)
{
	char ssid[WIFI_AUTO_SSID_MAX + 1];
	char psk[WIFI_AUTO_PASS_MAX + 1];
	uint32_t security;
	int have_psk, security_explicit;

	if (!kv_cfg_bool("wifi.enable", 0)) {
		/* Nothing to do, and deliberately so: rtl8720_init() has already driven
		 * CHIP_EN low from tx_application_define(), so the module is held OFF
		 * rather than left floating.  That call is unconditional -- it is what
		 * makes the module's state defined at all -- and only powering it ON is
		 * what this setting gates. */
		LOG_INF("wifi.enable is not set -- radio stays off");
		return;
	}

	if (!kv_cfg_bool("wifi.autoconnect", 0)) {
		/*
		 * Power-only.  The boot wait is ours to do here because the paths that
		 * would otherwise do it -- rtl_link_begin() and `wifi ver` -- only wait
		 * when THEY moved CHIP_EN, and both see a module that is already
		 * powered.  Nothing else is racing us to the UART, but leaving the
		 * module mid-boot with the host believing it is ready is the kind of
		 * state that fails once in twenty and is never reproduced.
		 */
		rtl8720_power(true);
		tx_thread_sleep(1500u);
		LOG_INF("radio powered (no autoconnect)");
		return;
	}

	/*
	 * Autoconnect: do NOT power the module here.  rtl_link_begin(sh, true) inside
	 * the sequence powers it AND waits ~1.5 s for it to boot -- but only when it
	 * is the one that moved CHIP_EN (app/rtl_link.c).  Powering it first would
	 * therefore skip the boot wait entirely and put the first eRPC frame on the
	 * wire while the module was still starting.
	 */
	if (!kv_cfg_str("wifi.ssid", ssid, sizeof ssid)) {
		LOG_WRN("wifi.autoconnect is set but wifi.ssid is not -- not associating");
		return;
	}
	have_psk = kv_cfg_str("wifi.psk", psk, sizeof psk);
	security_explicit = (kv_get_u32("wifi.security", &security) == KV_OK);
	if (!security_explicit)
		security = have_psk ? WIFI_RPC_SEC_WPA2_AES_PSK : WIFI_RPC_SEC_OPEN;

	/*
	 * Arm the re-association policy BEFORE associating, not after
	 * (owhinata/wio-lite-ai#32).  wifi_connect_run() captures the credentials on success
	 * by calling wifi_auto_arm(), and that call is a no-op while the policy is disabled --
	 * so enabling it afterwards would leave it armed with nothing, and the first
	 * AP outage would find no credentials to retry with.
	 */
	if (kv_cfg_bool("wifi.autoreconnect", 0))
		wifi_auto_set_enabled(true);

	/*
	 * This used to fail every single time, and owhinata/wio-lite-ai#40 explains why: the
	 * radio's associations fail intermittently (about half of them, against the 5 GHz AP
	 * it was measured on), and this path is the only one that never got a second go.  An
	 * operator retypes `wifi connect` without thinking of it as a retry; here there is
	 * nobody to do that.  wifi_connect_run() now retries by itself, which is what makes
	 * automatic association work -- there is nothing left for this file to wait for.
	 *
	 * In particular it is NOT a delay problem, so do not "fix" a future recurrence by
	 * growing rtl_link_begin()'s 1.5 s boot wait: a join issued 20 s after CHIP_EN goes
	 * high fails just as readily as one at 1.7 s (measured).
	 */
	LOG_INF("associating with \"%s\"", ssid);
	if (wifi_connect_run(&kv_boot_sh, ssid, have_psk ? psk : NULL, security,
	                     security_explicit, /* earn_gen */ true) != 0)
		LOG_WRN("automatic association failed -- `wifi connect` by hand");

	/* The passphrase is on the stack here and in host RAM inside wifi_auto after
	 * this; wipe our copy on the way out so it is not left in a thread stack that
	 * `devmem` can read.  This is hygiene, not protection -- the value is stored
	 * in the clear on the external flash regardless (see kv.h). */
	memset(psk, 0, sizeof psk);
}

/* net.mode, through the shared steps.  Between reading it and applying it is the
 * one check that is this board's (decision D4 of issue #214). */
static void cfg_net(void)
{
	char mode[16];

	if (!kv_cfg_net_mode(mode, sizeof mode))
		return;

	if (!link_bridge_is_up()) {
		/* Not a failure of this step: without an association there is no host
		 * stack to address, and saying so is more useful than a DHCP timeout. */
		LOG_WRN("net.mode=%s ignored -- the host stack is not up", mode);
		return;
	}

	kv_cfg_net_apply(mode);
}

/* ------------------------------------------------------------------ *
 *  Thread
 * ------------------------------------------------------------------ */
static void kv_boot_entry(ULONG arg)
{
	(void)arg;

	if (kv_cfg_open() != 0) {
		kv_cfg_tty_port();   /* the default, once */
		return;   /* nothing to apply; every command still works by hand */
	}

	/* Before the association, so telnet listens as soon as an address comes. */
	kv_cfg_tty_port();
	cfg_wifi();
	cfg_net();

	/* The sequence may have ended mid-line (a module that printed without a
	 * newline); flush it so nothing is left sitting in the assembler. */
	cli_backend_log_flush(&kv_boot_tr);

	/* Done.  Returning leaves the thread TERMINATED rather than parked on a
	 * sleep it would never wake from; its stack is not reclaimed either way, but
	 * `thread` then shows what is true instead of an idle thread that will never
	 * run again. */
}

int kv_boot_init(void)
{
	UINT rc;

	/* The reporting instance: objects only (output mutex + event flags), no
	 * thread.  A failure here is not fatal -- but the sequence reports through
	 * this instance, so without it the wifi/net steps have nowhere to write and
	 * are skipped rather than run blind. */
	if (cli_init(&kv_boot_sh) != 0)
		return -1;

	rc = tx_thread_create(&kv_boot_thread, "cfg", kv_boot_entry, 0,
	                      kv_boot_stack, sizeof kv_boot_stack,
	                      KV_BOOT_PRIORITY, KV_BOOT_PRIORITY,
	                      TX_NO_TIME_SLICE, TX_AUTO_START);

	return (rc == TX_SUCCESS) ? 0 : -1;
}
