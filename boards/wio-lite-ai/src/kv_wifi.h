/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Wio Lite AI ThreadX Shell Project
 */
/*
 * Wio Lite AI -- what the stored WiFi settings say to do, as one pure function
 * (issue #238).
 *
 * The configuration thread (src/kv_boot.c) reads wifi.enable, wifi.autoconnect,
 * wifi.ssid, wifi.psk and wifi.security and, from what it found, leaves the radio
 * off, powers it only, or associates.  That decision is here, with no ThreadX, no
 * store and no module behind it, so the host test (test/test_kv_wifi.c) can walk
 * every answer.  The reads, the lines they print and the actions stay with the
 * caller.
 *
 * THE KEYS ARE READ ONE AT A TIME, AS THE FUNCTION ASKS FOR THEM.  A read that
 * finds a key unreadable or of the wrong type prints a warning (shell/cmds/kv_cfg.c),
 * so reading a key the answer does not depend on would add a line to `dmesg` that
 * the boot never printed.  The caller therefore starts with every key UNREAD and
 * calls kv_wifi_next() again after each read it is told to do; the function only
 * asks for a key when the answer depends on it, in the order the boot always read
 * them: wifi.enable, wifi.autoconnect, wifi.ssid, wifi.psk, wifi.security.  Every
 * read it asks for is of a key still UNREAD, so a caller is done after at most
 * KV_WIFI_KEYS reads.
 *
 * TWO CALLERS.  KV_WIFI_BOOT is the configuration thread: wifi.enable unset leaves
 * the radio off, wifi.autoconnect unset powers it only.  KV_WIFI_CONSOLE is a
 * command an operator typed to connect with the saved settings: it is not asked
 * about those two keys at all -- the operator has said to connect, and the
 * association powers the module itself -- and is refused when the store is not
 * open or there is no wifi.ssid.
 */
#ifndef APP_KV_WIFI_H
#define APP_KV_WIFI_H

#include <stdint.h>

#include "wifi_auto.h"   /* enum wifi_auto_setting */

#ifdef __cplusplus
extern "C" {
#endif

enum kv_wifi_caller {
	KV_WIFI_BOOT    = 1,   /* the configuration thread at boot          */
	KV_WIFI_CONSOLE = 2,   /* a console command, on the operator's word */
};

/* What one key's read found.  A bool key is YES when it reads true; a string key
 * when it is present and readable; wifi.security when it reads as a u32.  Anything
 * else -- missing, false, unreadable, the wrong type -- is NO. */
enum kv_wifi_seen {
	KV_WIFI_UNREAD = 0,
	KV_WIFI_NO     = 1,
	KV_WIFI_YES    = 2,
};

struct kv_wifi_in {
	enum kv_wifi_caller caller;
	int                 store_open;    /* kv_init() succeeded */
	enum kv_wifi_seen   enable;        /* wifi.enable         */
	enum kv_wifi_seen   autoconnect;   /* wifi.autoconnect    */
	enum kv_wifi_seen   ssid;          /* wifi.ssid           */
	enum kv_wifi_seen   psk;           /* wifi.psk            */
	enum kv_wifi_seen   security;      /* wifi.security       */
};

/* The number of keys above: no caller reads more than this many times. */
#define KV_WIFI_KEYS 5

enum kv_wifi_step {
	/* Read this key, record what it found, and ask again. */
	KV_WIFI_READ_ENABLE = 1,
	KV_WIFI_READ_AUTOCONNECT,
	KV_WIFI_READ_SSID,
	KV_WIFI_READ_PSK,
	KV_WIFI_READ_SECURITY,

	/* The answer. */
	KV_WIFI_RADIO_OFF,      /* boot: wifi.enable is not set -- leave it off  */
	KV_WIFI_POWER_ONLY,     /* boot: powered, wifi.autoconnect is not set   */
	KV_WIFI_CONNECT,        /* associate with wifi.ssid                      */
	KV_WIFI_NO_STORE,       /* the store is not open: nothing to read        */
	KV_WIFI_NO_SSID,        /* there is no wifi.ssid to associate with       */
	KV_WIFI_BAD_CALL,       /* an unknown caller or key state: do nothing    */
};

struct kv_wifi_answer {
	enum kv_wifi_step step;
	/* KV_WIFI_CONNECT only (0 otherwise): the security to use when
	 * wifi.security is not set -- WPA2-AES with a wifi.psk, open without. */
	uint32_t          default_security;
};

struct kv_wifi_answer kv_wifi_next(const struct kv_wifi_in *in);

/*
 * wifi.autoreconnect (issue #240): is the host's automatic re-association on?
 *
 * ON ONLY WHEN THE KEY READS TRUE.  A false, a missing key, a value of another
 * type (`kv set wifi.autoreconnect 1` stores the string "1"), a read that
 * failed and a store that is not open are all OFF: holding the passphrase in
 * RAM is the operator's choice, so anything short of a clear yes says no.
 *
 * @store_open is whether the store was open when the caller looked; when it
 * was not, the caller does not read and @rc and @value are ignored.  @rc is
 * what kv_get_bool() returned (a KV_* code, svc/kv/kv.h) and @value what it
 * stored.  `setting` says which (src/wifi_auto.h; only WIFI_AUTO_SET_ON is
 * on).  `ignored` is NULL, or why a key that is there was not taken -- the
 * words the shared readers use (shell/cmds/kv_cfg.c) -- for a caller that
 * warns to report as "wifi.autoreconnect: ignored (<why>)".  A missing key and
 * a store that is not open are not warned about: they are the ordinary "off".
 *
 * Read by src/kv_boot.c's kv_boot_autoreconnect_read(), at the start of every
 * association and by `wifi status`; test/test_kv_wifi.c walks every answer.
 */
struct kv_wifi_auto {
	enum wifi_auto_setting setting;
	const char            *ignored;
};

struct kv_wifi_auto kv_wifi_autoreconnect_of(int store_open, int rc, int value);

/* A read's result as the input wants it: nonzero is YES. */
static inline enum kv_wifi_seen kv_wifi_seen_of(int found)
{
	return found ? KV_WIFI_YES : KV_WIFI_NO;
}

#ifdef __cplusplus
}
#endif

#endif /* APP_KV_WIFI_H */
