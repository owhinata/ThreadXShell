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
 *     wifi.ssid / wifi.psk / wifi.security   what to associate with
 *   net.mode           dhcp / static / manual
 *
 * and one key read elsewhere (issue #240):
 *
 *   wifi.autoreconnect ? the host re-associates by itself : it does not
 *
 * read by kv_boot_autoreconnect_read() below at the start of EVERY
 * association -- this thread's, `wifi reconnect`'s and a typed `wifi
 * connect`'s -- because that is the only moment it decides anything: the
 * credentials are kept by a successful association or not at all.  `wifi
 * status` reads it too, to show what it holds now.  Only a bool true turns it on
 * (`kv set wifi.autoreconnect true bool`; without `bool` the value is stored
 * as a string and ignored, with a warning).
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
 * The association claims this log instance through rtl_link_begin(); since issue
 * #229 that claim is counted on this instance alone, so USB CDC printf and Ctrl+C
 * stay live during a boot association (it used to be one global flag).
 *
 * `wifi reconnect` (issue #238) runs the same WiFi and net.mode steps
 * (kv_apply()) from a console: it reports to that console as well as to the log,
 * does not read wifi.enable / wifi.autoconnect (src/kv_wifi.h), and applies
 * net.mode only after a connection.  It and this thread never run at once
 * (kv_apply_owner).
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
#include "kv_wifi.h"   /* what the wifi.* keys say to do (issue #238) */

#include "cli.h"
#include "cli_backend_log.h"
#include "fmt.h"         /* fmt_vsnformat(): a report line for the console */
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

/* ------------------------------------------------------------------ *
 *  Who is applying the configuration (issue #238)
 * ------------------------------------------------------------------ */

/*
 * The boot sequence and `wifi reconnect` are one implementation (kv_apply()
 * below), and only one of them runs at a time: this word says which.  FREE at
 * reset (DTCM_BSS is zeroed by SystemInit()); kv_boot_init() sets BOOT just
 * before it creates the thread -- the scheduler is not running yet, so no
 * command can have taken it first -- and puts it back if the thread was not
 * created.  The thread gives it back on every way out.  A command takes it
 * with one test-and-set with interrupts masked and holds it until its net.mode
 * step (up to KV_CFG_DHCP_WAIT_MS) has finished.
 *
 * It separates these two and nothing else: no other command reads it.  A
 * `wifi reconnect` runs in the USB console's foreground only (telnet and `&`
 * are refused first), so nothing else is typed on that console meanwhile, and
 * what telnet can still run beside it is what can run beside `wifi connect` or
 * the boot sequence today.
 */
enum { KV_APPLY_FREE = 0u, KV_APPLY_BOOT = 1u, KV_APPLY_CONSOLE = 2u };
static volatile uint8_t kv_apply_owner DTCM_BSS;

/* The console a `wifi reconnect` is running on, or NULL: the report below also
 * writes every line there.  Only the owner of kv_apply_owner writes it, and it
 * is cleared BEFORE the word is given back, so a line can never land on a
 * console whose command has ended. */
static struct cli_instance *volatile kv_report_sh DTCM_BSS;

/*
 * Every line this file and the shared steps report: to the log, as before, and
 * -- while a `wifi reconnect` runs -- to its console too.  The arguments are
 * walked twice, each with its own va_start, so the second walk never reads a
 * list the first one consumed.
 */
static void kv_boot_report(unsigned level, const char *fmt, ...)
{
	struct cli_instance *sh = kv_report_sh;
	va_list ap;

	if (level <= LOG_COMPILE_LEVEL) {   /* the LOG_*() macros' build-time floor */
		va_start(ap, fmt);
		log_vwrite(level, LOG_TAG, fmt, ap);
		va_end(ap);
	}
	if (sh != NULL) {
		char line[LOG_MSG_MAX + 1];   /* what one log record holds */

		va_start(ap, fmt);
		(void)fmt_vsnformat(line, sizeof line, fmt, ap);
		va_end(ap);
		if (level == KV_CFG_REPORT_ERR)
			(void)cli_error(sh, "%s\r\n", line);
		else if (level == KV_CFG_REPORT_WRN)
			(void)cli_warn(sh, "%s\r\n", line);
		else
			(void)cli_print(sh, "%s\r\n", line);
	}
}

#define KV_INF(...) kv_boot_report(KV_CFG_REPORT_INF, __VA_ARGS__)
#define KV_WRN(...) kv_boot_report(KV_CFG_REPORT_WRN, __VA_ARGS__)

const struct kv_cfg_facts kv_cfg_board_facts = {
	.report      = kv_boot_report,
	/* No net.mode: leave the address to the operator. */
	.net_default = KV_CFG_NET_NONE,
};

/* ------------------------------------------------------------------ *
 *  wifi.autoreconnect (issue #240)
 * ------------------------------------------------------------------ */

/*
 * Read at the start of every association (wifi_connect_run(), src/wifi_connect.c):
 * the boot's, a `wifi reconnect`'s and a typed `wifi connect`'s -- and by every
 * `wifi status`, from any console or a background job.  The answer is
 * src/kv_wifi.c's; this does the read and, when @warn, the warning (an
 * association warns; `wifi status` says it in its heading instead of adding a
 * line to dmesg each time).
 *
 * Not through kv_cfg_bool(): that one says "unreadable" for a store that is not
 * open, and this is called on every typed `wifi connect`, including during the
 * boot's kv_init() and on a board whose store never opened -- where "off" is the
 * whole answer and a warning per connect would say nothing new.
 *
 * noinline: the read costs a whole record buffer and a decoded value (about
 * 700 B, see KV_BOOT_STACK_SIZE).  Kept out of line, that stack is given back
 * before the association goes any deeper; folded into wifi_connect_run() by LTO,
 * it would sit under every eRPC call of the association instead.
 */
__attribute__((noinline)) enum wifi_auto_setting
kv_boot_autoreconnect_read(bool warn, const char **store_state)
{
	int open = (kv_state() == KV_STATE_READY);
	int value = 0;
	int rc = KV_ERR_STATE;
	struct kv_wifi_auto a;

	if (open)
		rc = kv_get_bool("wifi.autoreconnect", &value);
	a = kv_wifi_autoreconnect_of(open, rc, value);
	if (warn && a.ignored != NULL)
		KV_WRN("wifi.autoreconnect: ignored (%s)", a.ignored);
	if (store_state != NULL)
		*store_state = (a.setting == WIFI_AUTO_SET_NO_STORE) ? kv_state_str() : "";
	return a.setting;
}

/* ------------------------------------------------------------------ *
 *  Steps
 * ------------------------------------------------------------------ */

/* How the WiFi step ended, for the caller that decides whether net.mode follows. */
enum cfg_wifi_end {
	CFG_WIFI_CONNECTED = 0,   /* associated, and the host stack came up       */
	CFG_WIFI_NOT_CONNECTED,   /* nothing to do, refused, or the sequence failed */
	CFG_WIFI_CANCELLED,       /* Ctrl+C was asked for on the caller's console  */
};

/*
 * The WiFi step.  @sh is the shell the association sequence reports through and
 * is cancelled from: the log instance at boot, the operator's console for
 * `wifi reconnect`.  @caller decides what src/kv_wifi.h asks for: the boot
 * reads wifi.enable and wifi.autoconnect and may power the radio only; a
 * console caller does neither -- the power-only path takes no lock and does not
 * forget the module, which is only right straight after reset.
 */
static enum cfg_wifi_end cfg_wifi(struct cli_instance *sh, enum kv_wifi_caller caller,
                                  int store_open)
{
	char ssid[WIFI_AUTO_SSID_MAX + 1];
	char psk[WIFI_AUTO_PASS_MAX + 1];
	uint32_t security = 0u;
	struct kv_wifi_in in = { .caller = caller, .store_open = store_open };
	struct kv_wifi_answer a;
	unsigned reads;
	int rc;

	/* Read the keys one at a time, as src/kv_wifi.h asks for them: a key the
	 * answer does not depend on is never read, so it never warns. */
	for (reads = 0u;; reads++) {
		a = kv_wifi_next(&in);
		if (reads >= KV_WIFI_KEYS)
			break;
		if (a.step == KV_WIFI_READ_ENABLE)
			in.enable = kv_wifi_seen_of(kv_cfg_bool("wifi.enable", 0));
		else if (a.step == KV_WIFI_READ_AUTOCONNECT)
			in.autoconnect = kv_wifi_seen_of(kv_cfg_bool("wifi.autoconnect", 0));
		else if (a.step == KV_WIFI_READ_SSID)
			in.ssid = kv_wifi_seen_of(kv_cfg_str("wifi.ssid", ssid, sizeof ssid));
		else if (a.step == KV_WIFI_READ_PSK)
			in.psk = kv_wifi_seen_of(kv_cfg_str("wifi.psk", psk, sizeof psk));
		else if (a.step == KV_WIFI_READ_SECURITY)
			in.security = kv_wifi_seen_of(kv_get_u32("wifi.security",
			                                         &security) == KV_OK);
		else
			break;
	}

	if (a.step == KV_WIFI_RADIO_OFF) {
		/* Nothing to do, and deliberately so: rtl8720_init() has already driven
		 * CHIP_EN low from tx_application_define(), so the module is held OFF
		 * rather than left floating.  That call is unconditional -- it is what
		 * makes the module's state defined at all -- and only powering it ON is
		 * what this setting gates. */
		KV_INF("wifi.enable is not set -- radio stays off");
		return CFG_WIFI_NOT_CONNECTED;
	}

	if (a.step == KV_WIFI_POWER_ONLY) {
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
		KV_INF("radio powered (no autoconnect)");
		return CFG_WIFI_NOT_CONNECTED;
	}

	/*
	 * Autoconnect: do NOT power the module here.  rtl_link_begin(sh, true) inside
	 * the sequence powers it AND waits ~1.5 s for it to boot -- but only when it
	 * is the one that moved CHIP_EN (app/rtl_link.c).  Powering it first would
	 * therefore skip the boot wait entirely and put the first eRPC frame on the
	 * wire while the module was still starting.
	 */
	if (a.step == KV_WIFI_NO_SSID) {
		if (caller == KV_WIFI_BOOT)
			KV_WRN("wifi.autoconnect is set but wifi.ssid is not -- not associating");
		else
			(void)cli_error(sh, "wifi reconnect: no saved network (wifi.ssid is "
			                "not set)\r\n");
		return CFG_WIFI_NOT_CONNECTED;
	}
	if (a.step == KV_WIFI_NO_STORE) {
		/* Only a console caller gets here: the boot runs this step only when
		 * the store opened. */
		(void)cli_error(sh, "wifi reconnect: the configuration store is not open "
		                "(%s)\r\n", kv_state_str());
		return CFG_WIFI_NOT_CONNECTED;
	}
	if (a.step != KV_WIFI_CONNECT)
		return CFG_WIFI_NOT_CONNECTED;   /* not reached: see test/test_kv_wifi.c */
	if (in.security != KV_WIFI_YES)
		security = a.default_security;

	/*
	 * wifi.autoreconnect is not read here (issue #240): wifi_connect_run()
	 * reads it at the start of every association, this one included, through
	 * kv_boot_autoreconnect_read() above.  Until #240 this step turned the
	 * re-association on when the key was true and never off.
	 */

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
	/*
	 * The firmware generation is earned here for both callers (decision 2 of
	 * issue #238): a console caller after `wifi flash` faces a module whose
	 * generation the host has forgotten, exactly as the boot does.  The pre-N2
	 * heap hazard of the version query (owhinata/wio-lite-ai#20) is the one the
	 * boot already takes on every start.
	 */
	KV_INF("associating with \"%s\"", ssid);
	rc = wifi_connect_run(sh, ssid, (in.psk == KV_WIFI_YES) ? psk : NULL,
	                      security, in.security == KV_WIFI_YES, /* earn_gen */ true);

	/* The passphrase is on the stack here and, when wifi.autoreconnect is true, in
	 * host RAM inside wifi_auto after this (issue #240); wipe our copy on the way
	 * out so it is not left in a thread stack that `devmem` can read.  This is
	 * hygiene, not protection -- the value is stored in the clear on the external
	 * flash regardless (see kv.h). */
	memset(psk, 0, sizeof psk);

	if (caller == KV_WIFI_BOOT) {
		if (rc != 0)
			KV_WRN("automatic association failed -- `wifi connect` by hand");
		return (rc == 0) ? CFG_WIFI_CONNECTED : CFG_WIFI_NOT_CONNECTED;
	}
	/*
	 * wifi_connect_run() answers a Ctrl+C and a failure alike (1), so a cancel
	 * is told apart by the console's request, asked straight after it returns
	 * and on success too: the request stays set for the rest of the command.
	 * It says Ctrl+C was pressed, not that it caused the failure.  A cancel
	 * while the host stack was coming up stops only the wait; the bring-up
	 * goes on in the background, as after `wifi connect`.
	 */
	if (cli_cancel_requested(sh)) {
		KV_INF("wifi reconnect: stopped by Ctrl+C -- net.mode not applied");
		return CFG_WIFI_CANCELLED;
	}
	if (rc != 0) {
		KV_WRN("wifi reconnect: not connected -- net.mode not applied");
		return CFG_WIFI_NOT_CONNECTED;
	}
	return CFG_WIFI_CONNECTED;
}

/* net.mode, through the shared steps.  Between reading it and applying it is the
 * one check that is this board's (decision D4 of issue #214). */
static void cfg_net(enum kv_wifi_caller caller)
{
	char mode[16];

	if (!kv_cfg_net_mode(mode, sizeof mode))
		return;

	if (!link_bridge_is_up()) {
		/* Not a failure of this step: without an association there is no host
		 * stack to address, and saying so is more useful than a DHCP timeout. */
		KV_WRN("net.mode=%s ignored -- the host stack is not up", mode);
		return;
	}

	/* The DHCP wait does not answer Ctrl+C (the shared step has no way to ask),
	 * so a console caller is told how long it can take before it starts.  The
	 * boot says nothing new: its log stays what it was. */
	if (caller != KV_WIFI_BOOT && strcmp(mode, "dhcp") == 0)
		KV_INF("net.mode=dhcp: waiting up to %u s for a lease (Ctrl+C does not "
		       "stop this wait)", KV_CFG_DHCP_WAIT_MS / 1000u);

	kv_cfg_net_apply(mode);
}

/*
 * The configuration, applied: the WiFi step, then net.mode.  One implementation
 * for the boot thread and for `wifi reconnect` (issue #238); the caller holds
 * kv_apply_owner.  The boot runs net.mode whatever the WiFi step did, as it
 * always has (with the host stack down it says so and skips); a console caller
 * runs it only after a connection, so a failed or cancelled association never
 * walks into a 30 s wait that cannot be stopped.
 */
static enum cfg_wifi_end kv_apply(struct cli_instance *sh, enum kv_wifi_caller caller,
                                  int store_open)
{
	enum cfg_wifi_end end = cfg_wifi(sh, caller, store_open);

	if (caller == KV_WIFI_BOOT || end == CFG_WIFI_CONNECTED)
		cfg_net(caller);
	return end;
}

/* ------------------------------------------------------------------ *
 *  Thread
 * ------------------------------------------------------------------ */
static void kv_boot_entry(ULONG arg)
{
	(void)arg;

	if (kv_cfg_open() != 0) {
		kv_cfg_tty_port();   /* the default, once */
		kv_apply_owner = KV_APPLY_FREE;
		return;   /* nothing to apply; every command still works by hand */
	}

	/* Before the association, so telnet listens as soon as an address comes. */
	kv_cfg_tty_port();
	(void)kv_apply(&kv_boot_sh, KV_WIFI_BOOT, /* store_open */ 1);

	/* The sequence may have ended mid-line (a module that printed without a
	 * newline); flush it so nothing is left sitting in the assembler. */
	cli_backend_log_flush(&kv_boot_tr);

	/* The boot never sets kv_report_sh, so there is nothing to clear first. */
	kv_apply_owner = KV_APPLY_FREE;

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
		return -1;   /* kv_apply_owner untouched: FREE, nothing to give back */

	/* Before the thread exists, and before the scheduler runs: no command can
	 * have taken the word first, and the thread can only start with it set. */
	kv_apply_owner = KV_APPLY_BOOT;
	rc = tx_thread_create(&kv_boot_thread, "cfg", kv_boot_entry, 0,
	                      kv_boot_stack, sizeof kv_boot_stack,
	                      KV_BOOT_PRIORITY, KV_BOOT_PRIORITY,
	                      TX_NO_TIME_SLICE, TX_AUTO_START);
	if (rc != TX_SUCCESS) {
		kv_apply_owner = KV_APPLY_FREE;
		return -1;
	}
	return 0;
}

/* ------------------------------------------------------------------ *
 *  `wifi reconnect` (issue #238)
 * ------------------------------------------------------------------ */
int kv_boot_reconnect(struct cli_instance *sh)
{
	TX_INTERRUPT_SAVE_AREA
	uint8_t was;
	enum cfg_wifi_end end;

	/* Test and set in one masked section: the boot thread gives the word back
	 * from a lower priority, and another console could be asking too. */
	TX_DISABLE
	was = kv_apply_owner;
	if (was == KV_APPLY_FREE)
		kv_apply_owner = KV_APPLY_CONSOLE;
	TX_RESTORE

	if (was == KV_APPLY_BOOT) {
		(void)cli_error(sh, "wifi reconnect: the boot configuration is still running "
		                "-- see dmesg; retry when it ends\r\n");
		return 1;
	}
	if (was != KV_APPLY_FREE) {
		(void)cli_error(sh, "wifi reconnect: already running\r\n");
		return 1;
	}

	kv_report_sh = sh;
	/* So the log tells this run apart from the boot's, whose lines it repeats. */
	KV_INF("wifi reconnect: applying the saved settings");
	end = kv_apply(sh, KV_WIFI_CONSOLE, kv_state() == KV_STATE_READY);
	/* The console first, then the word: a line reported after the word is free
	 * must never find this command's console. */
	kv_report_sh = NULL;
	kv_apply_owner = KV_APPLY_FREE;

	return (end == CFG_WIFI_CONNECTED) ? 0 : 1;
}
