/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    kv_boot.c
 * @brief   The configuration thread: opens the key-value store and applies it
 *          (issue #214; the same shape as wio-lite-ai's src/kv_boot.c).
 *
 * A thread exists here because this work waits on the flash: opening the store
 * can erase (a first boot formats a blank partition, about 75 s on this part)
 * and a DHCP lease takes seconds.  tx_application_define() runs before there
 * is a scheduler, so it only creates this thread.
 *
 * The steps are the shared shell/cmds/kv_cfg.c; this thread owns the order:
 *
 *   kv_init()          open the store (may format a blank partition)
 *   net.shell.port     the telnet console's port            : 23
 *   net.mode           dhcp / static / manual               : dhcp
 *
 * [!] WHO HANDS OVER THE DEFAULTS (issue #214).  Once this thread exists it
 * hands the telnet console its port and settles the address source, ONCE, on
 * every path: the store does not open, a key is missing, mistyped or out of
 * range -- each ends at port 23 and DHCP (kv_cfg_board_facts.net_default), the
 * board's boot default before the store existed.  `manual` leaves the address
 * alone.  The port is handed over only when the listener exists, and net.mode
 * is applied only when NetX is up (kv_boot_init()'s two flags); when the thread
 * cannot be created, src/main.c does both instead.
 *
 * The first boot on a blank partition formats it before anything else, so the
 * port and DHCP come after the format, about 75 s late.
 *
 * Every step is fail-soft (kv_cfg.h): a step that fails is reported to `dmesg`
 * (tag "cfg", and "kv" for the store's own lines) and the next one still runs.
 *
 * Priority 15: above the shells (16), so the configuration is settled before a
 * user can reasonably type, and below the IWDG petter (5), the camera (10) and
 * the IP / DHCP threads (12, 13), none of which may wait behind a flash erase.
 */
#define LOG_TAG "cfg"
#include "log.h"

#include "kv_boot.h"
#include "kv_cfg.h"      /* the shared boot steps (shell/cmds) */
#include "fal_qspi.h"    /* kv_qspi_init() */

#include "tx_api.h"

#include <stdarg.h>

#define KV_BOOT_PRIORITY    15u

/* 4 KB, as wio-lite-ai's: a kv_get_str() costs about 700 bytes on its own (a
 * record buffer and the decoded copy), FlashDB's init and format nest under
 * kv_init(), and the static address goes through the shared net operations.
 * A stack overflow here would be a boot-time fault before any console is up. */
#define KV_BOOT_STACK_SIZE  4096u

#define KV_BOOT_HAVE_NETX   0x1u
#define KV_BOOT_HAVE_TTY    0x2u

static TX_THREAD kv_boot_thread;
static UCHAR     kv_boot_stack[KV_BOOT_STACK_SIZE] __attribute__((aligned(8)));

/* ------------------------------------------------------------------ *
 *  The shared steps' facts (shell/cmds/kv_cfg.h)
 * ------------------------------------------------------------------ */

_Static_assert(KV_CFG_REPORT_ERR == LOG_LEVEL_ERR && KV_CFG_REPORT_WRN == LOG_LEVEL_WRN &&
               KV_CFG_REPORT_INF == LOG_LEVEL_INF,
               "kv_cfg report weights must be this board's log levels");

static void kv_boot_report(unsigned level, const char *fmt, ...)
{
	va_list ap;

	if (level > LOG_COMPILE_LEVEL)   /* the LOG_*() macros' build-time floor */
		return;
	va_start(ap, fmt);
	log_vwrite(level, LOG_TAG, fmt, ap);
	va_end(ap);
}

const struct kv_cfg_facts kv_cfg_board_facts = {
	.report      = kv_boot_report,
	/* No usable net.mode, or no store: DHCP, as this board booted before the
	 * store existed (decision D2). */
	.net_default = KV_CFG_NET_DHCP,
};

/* ------------------------------------------------------------------ *
 *  Thread
 * ------------------------------------------------------------------ */

static void kv_boot_entry(ULONG arg)
{
	/* Whether or not it opens: a store that did not open has reported why,
	 * and every step below then takes its default. */
	(void)kv_cfg_open();

	/* Before net.mode, so telnet listens as soon as an address comes. */
	if (arg & KV_BOOT_HAVE_TTY)
		kv_cfg_tty_port();

	if (arg & KV_BOOT_HAVE_NETX) {
		char mode[16];

		if (kv_cfg_net_mode(mode, sizeof mode))
			kv_cfg_net_apply(mode);
	}

	/* Done.  Returning leaves the thread TERMINATED, which `thread` then
	 * shows as what is true. */
}

int kv_boot_init(int have_netx, int have_tty)
{
	ULONG arg = (have_netx ? KV_BOOT_HAVE_NETX : 0u) |
	            (have_tty ? KV_BOOT_HAVE_TTY : 0u);

	/* Without its lock the store answers "not ready" to everything, which the
	 * thread then reports and takes the defaults on -- so this is not a reason
	 * to give the defaults back to main.c. */
	if (kv_qspi_init() != 0)
		LOG_ERR("kv: store lock create failed -- the store stays unavailable");

	return (tx_thread_create(&kv_boot_thread, "cfg", kv_boot_entry, arg,
	                         kv_boot_stack, sizeof kv_boot_stack,
	                         KV_BOOT_PRIORITY, KV_BOOT_PRIORITY,
	                         TX_NO_TIME_SLICE, TX_AUTO_START) == TX_SUCCESS)
	       ? 0 : -1;
}
