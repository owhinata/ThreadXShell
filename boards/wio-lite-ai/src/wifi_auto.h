/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/*
 * Wio Lite AI (STM32H725AEI6) -- host-side automatic re-association
 * (owhinata/wio-lite-ai#32).
 *
 * When the AP disappears, is switched off or deauthenticates us, the association goes and
 * nothing brings it back.  This file is the policy that does: the credentials of the last
 * successful association that was allowed to keep them (wifi.autoreconnect, read by
 * that association), a backoff, and the one eRPC call that re-associates.  The DETECTION and the SEQUENCING live in app/link_bridge.c, which already
 * samples the association every NXN_REFRESH_MS with the coarse link mutex in hand; this
 * file never claims anything and never decides when it is safe to talk to the module.
 *
 * ---- why not the module's own autoreconnect ------------------------------------
 *
 * `wifi_set_autoreconnect()` installs a handler that runs wifi_connect() and then
 * LwIP_DHCP(0, START), falling back to LwIP_AUTOIP when nothing answers (disassembly of
 * lib_arduino.a:wifi_conf.o, owhinata/wio-lite-ai#31).  While the L2 bridge is in, not one
 * byte reaches the module's lwIP, so that DHCP always times out and AutoIP puts 169.254.x.x
 * on xnetif[0] -- and the WLAN driver's netif_is_valid_IP() returns 1 unconditionally ONLY
 * while the netif address is zero.  The moment it is not, host-bound unicast IP is dropped
 * before it ever reaches netif_rx().  Enabling the module's feature therefore kills the
 * bridge silently, which is why the host re-issues rpc_wifi_connect itself: the module's
 * DHCP never runs, the netif address stays zero, and the tap is untouched.
 *
 * (wifi_connect()'s own success path only calls netif_set_link_up() and
 * restore_wifi_info_to_flash() -- and the latter early-returns while
 * p_write_reconnect_ptr is NULL, which it is precisely because we never call
 * wifi_set_autoreconnect.  So a re-association writes no module flash either.)
 *
 * ---- the credentials -----------------------------------------------------------
 *
 * Re-associating needs the SSID and the passphrase, so they are kept in host RAM.
 *
 * THAT IS AN EXPOSURE -- `devmem` and the telnet console can both read RAM -- so it is
 * OFF unless the stored wifi.autoreconnect reads true (issue #240).  The key is read at
 * the start of every association (wifi_connect_run()), and only a successful
 * association with it true keeps anything; with it false or absent the credentials
 * held from before are dropped by that same association and nothing new is kept.
 * (Until issue #240 it was on by default, `wifi autoreconnect off` turned it off for
 * the session, and a reset turned it back on.)
 *
 * Measure the cost against what is already there: an operator who connects with the
 * saved settings (`wifi.ssid` / `wifi.psk`, at boot or by `wifi reconnect`) already
 * keeps the same passphrase IN THE CLEAR on the external NOR, which `kv get` and
 * `nor read` can show.  Turning this on adds a copy in RAM; it is the operator who
 * types `wifi connect <ssid> <pw>` by hand and keeps nothing in the store for whom it
 * is a new place the passphrase lives.
 *
 * The credentials are captured by a successful association rather than dug out of the
 * module, and they are zeroed on every path that ends the association or changes the
 * module underneath us (see wifi_auto_disarm() and the forget-generation backstop below).
 *
 * ---- threading -----------------------------------------------------------------
 *
 * Two threads touch this state: a CLI thread running the `wifi` commands, and the bridge
 * owner thread (`nx-net`, app/link_bridge.c) running an attempt.  Updates are made under
 * a short PRIMASK critical section so that no reader can catch the credentials, the
 * armed flag and the backoff half-updated (the same reason link_bridge.c's
 * nxn_mod_accumulate() masks).  The one exception is wifi_auto_armed(), which is polled
 * roughly every millisecond from erpc_call_ex()'s abort hook and therefore only ever
 * reads a single volatile bool.
 *
 * No clock/RCC/register work of its own (clock-safe).  Clean-room design.
 */
#ifndef APP_WIFI_AUTO_H
#define APP_WIFI_AUTO_H

#include <stdbool.h>
#include <stdint.h>

#include "wifi_rpc.h"    /* struct wifi_rpc_opts */

struct cli_instance;

/* Module-imposed bounds (rpc_wifi_connect / wifi_connect take no longer strings). */
#define WIFI_AUTO_SSID_MAX 32u
#define WIFI_AUTO_PASS_MAX 64u

/*
 * Backoff after a failed attempt, in milliseconds.  The first loss is retried at once --
 * an AP that bounced is back before the second sample -- and only a run of failures backs
 * off, so a genuinely absent AP costs one attempt a minute rather than one every 8 s.
 */
#define WIFI_AUTO_BACKOFF_MIN_MS 8000u
#define WIFI_AUTO_BACKOFF_MAX_MS 60000u

/*
 * What the stored wifi.autoreconnect says (issue #240).  Only ON turns the
 * re-association on; every other value is off, each for its own reason, so
 * `wifi status` can say which.  NO_KV is a build without the store.
 */
enum wifi_auto_setting {
	WIFI_AUTO_SET_ON = 1,       /* reads true                              */
	WIFI_AUTO_SET_FALSE,        /* reads false                             */
	WIFI_AUTO_SET_MISSING,      /* no such key                             */
	WIFI_AUTO_SET_NOT_BOOL,     /* stored as another type (`kv set` without `bool`) */
	WIFI_AUTO_SET_UNREADABLE,   /* the store is open but the read failed   */
	WIFI_AUTO_SET_NO_STORE,     /* the store is not open (starting, down, damaged) */
	WIFI_AUTO_SET_NO_KV,        /* this build has no store                 */
};

/*
 * Read wifi.autoreconnect now.  @warn: report a key that is there but not taken
 * (`wifi.autoreconnect: ignored (<why>)`, to the log) -- an association does, `wifi
 * status` does not (it says so in its own lines, and would repeat the warning every
 * time).  *@store_state, when @store_state is not NULL, is the store's state in words
 * when the answer is NO_STORE, "" otherwise.  May wait for the NOR (up to the store's
 * 5 s), so never call it with the link held.
 */
enum wifi_auto_setting wifi_auto_setting_read(bool warn, const char **store_state);

/*
 * Remember what an association just joined, so an attempt can repeat it -- only when
 * @keep, the wifi.autoreconnect THIS association read at its start (issue #240): that
 * is what makes keeping the passphrase opt-in.  The value is passed in rather than
 * left somewhere shared, because the boot's association and one typed on the USB
 * console can both be between their read and this call.  With @keep false nothing is
 * kept and what was held is dropped, in the same critical section.  @password may be
 * NULL (open network).  Returns true when the credentials are now held.
 */
bool wifi_auto_arm(const char *ssid, const char *password, uint32_t security, bool keep);

/*
 * Forget the credentials and stop attempting.  @why is remembered for `wifi status`.
 *
 * This is ALSO the abort: an attempt in flight sees wifi_auto_armed() go false through its
 * should_abort hook and returns within about a millisecond.  Making disarm and abort the
 * same act is what removes the ordering hazard a separate cancel flag would have -- there
 * is no window in which a command has asked to stop and an attempt starts anyway.
 *
 * Callable from any thread; takes no lock a command could be holding.
 */
void wifi_auto_disarm(const char *why);

/* Polled from erpc_call_ex()'s abort hook: a single volatile read, no generation check. */
bool wifi_auto_armed(void);

/* Armed, the module still the one we armed against, and the backoff expired. */
bool wifi_auto_should_try(void);

/*
 * Re-issue rpc_wifi_connect with the stored credentials and fold the outcome into the
 * statistics and the backoff.  The caller owns the coarse link mutex and has already
 * extended the module's bridge hold; @o carries the timeout, the abort hook and the diag
 * block.  Returns 0 when the module reported success, -4 when the attempt was aborted
 * (the caller must then issue NO further eRPC and release the link at once -- the module
 * is still inside wifi_connect() and holding its serial mutex), or another negative value
 * for a transport failure or a module error.
 */
int wifi_auto_attempt(const struct wifi_rpc_opts *o);

/*
 * What `wifi status` says about the re-association (issue #240), as a pure function
 * of the stored setting and whether credentials are held: the heading (one "%s", the
 * store's state in words, used by NO_STORE only) and the line that says what happens
 * next when the two disagree, or what to do (NULL for none).  test/test_wifi_auto.c
 * walks every combination.
 */
struct wifi_auto_status_text {
	const char *heading;
	const char *next;
};
struct wifi_auto_status_text wifi_auto_status_text(enum wifi_auto_setting s, bool armed);

/*
 * The end of `wifi status` (issue #240): the setting as the store holds it NOW, then
 * what is actually held, the tally and the backoff, then the line above.  Reads the
 * store, so it is called after the link is given back.
 */
void wifi_auto_print(struct cli_instance *sh);

#endif /* APP_WIFI_AUTO_H */
