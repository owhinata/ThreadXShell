/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    kv_cfg.h
 * @brief   The boot-time configuration steps every board with a key-value store
 *          and a NetX stack shares (owhinata/wio-lite-ai#37; shared since issue
 *          #214): open the store, hand the telnet console its port, apply
 *          `net.mode`, and the setting readers behind them.
 *
 * shell/cmds/kv_cfg.c holds the steps; it needs the store (svc/kv/kv.h), the
 * shared address operations (net_core.h) and the telnet console (net_tty.h), so
 * it is under shell/cmds/, on the property "has a KV store AND NetX" that the
 * board decides (BOARD_HAS_KV_NETX).
 *
 * [!] THE BOARD OWNS THE THREAD AND THE ORDER (issue #214, decision D3).  Its
 * configuration thread calls these steps; nothing here calls back into the
 * board.  Whatever only one board does between them (wio-lite-ai: the radio,
 * and "ignore net.mode while the WiFi session is down") stays in that thread.
 *
 * [!] OWNS NO STORAGE (cmake/check_no_mutable_storage.py, per board).  The
 * board defines kv_cfg_board_facts, the const table below; every line these
 * steps leave in the log goes through its report function (the log is the
 * board's).
 *
 * THREAD CONTEXT ONLY, after the scheduler has started: opening the store can
 * erase, and the DHCP wait sleeps.
 */
#ifndef KV_CFG_H
#define KV_CFG_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The weight of a line handed to kv_cfg_facts.report(). */
#define KV_CFG_REPORT_ERR  0u
#define KV_CFG_REPORT_WRN  1u
#define KV_CFG_REPORT_INF  2u

/** What `net.mode` is taken to be when it says nothing usable. */
enum kv_cfg_net_default {
	/** Leave the address alone (wio-lite-ai: the operator sets it). */
	KV_CFG_NET_NONE = 0,
	/** Make DHCP the source. */
	KV_CFG_NET_DHCP = 1,
};

/** How long kv_cfg_net_apply() waits for a DHCP lease before moving on. */
#define KV_CFG_DHCP_WAIT_MS  30000u
#define KV_CFG_POLL_MS       250u

/** The board's constant facts for kv_cfg.c. */
struct kv_cfg_facts {
	/** One line for the board's log, at KV_CFG_REPORT_* weight. */
	void (*report)(unsigned level, const char *fmt, ...)
		__attribute__((format(printf, 2, 3)));
	/** enum kv_cfg_net_default: what an absent, unreadable or invalid
	 *  `net.mode` -- or a store that did not open -- means on this board. */
	uint8_t net_default;
};

/** The board's table (wio-lite-ai: src/kv_boot.c). */
extern const struct kv_cfg_facts kv_cfg_board_facts;

/**
 * Open the store (kv_init()).  0 when it is ready; otherwise reports
 * "configuration store unavailable: <state>" and returns -1.
 */
int kv_cfg_open(void);

/*
 * Setting readers.  Each returns the configured value or the supplied default.
 * A key that is absent and a key that is present but of the wrong type are
 * treated the same way -- as "not configured" -- but only the second is
 * reported, because a missing key is the normal case and a mistyped one is a
 * mistake somebody wants to hear about.
 */
int      kv_cfg_bool(const char *key, int dflt);
uint32_t kv_cfg_u32(const char *key, uint32_t dflt);
/** 1 when @p buf holds a configured string; 0 (and "" in @p buf) otherwise. */
int      kv_cfg_str(const char *key, char *buf, uint32_t buflen);

/**
 * Hand the telnet console its port, ONCE: `net.shell.port` from the open store,
 * or NET_TTY_PORT_DEFAULT when the store is not open or the key is missing,
 * unreadable or not a port.  Call it only on a board whose telnet listener
 * exists, and only once.
 */
void kv_cfg_tty_port(void);

/**
 * Read `net.mode` into @p mode (at least 16 bytes).  Returns 1 when there is
 * something to apply (then pass @p mode to kv_cfg_net_apply()), 0 when there is
 * not: `manual`, or nothing usable with the board's default NONE.  An absent or
 * unreadable `net.mode`, or a store that is not open, becomes "dhcp" on a board
 * whose default is DHCP.
 */
int kv_cfg_net_mode(char *mode, size_t size);

/**
 * Apply a `net.mode` read by kv_cfg_net_mode(): dhcp (make DHCP the source and
 * wait up to KV_CFG_DHCP_WAIT_MS for an address) or static (net.ip, net.mask and
 * net.gw, all three).  Anything that cannot be applied is reported; on a board
 * whose default is DHCP it then falls back to DHCP, once.  Call it only when the
 * board has NetX, and only once.
 */
void kv_cfg_net_apply(const char *mode);

#ifdef __cplusplus
}
#endif

#endif /* KV_CFG_H */
