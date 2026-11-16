/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/*
 * Host-side automatic re-association (owhinata/wio-lite-ai#32).  See app/wifi_auto.h for why the
 * module's own wifi_set_autoreconnect() cannot be used, and for the threading rules.
 */
#include "wifi_auto.h"

#include <string.h>

#include "tx_api.h"
#include "stm32h7xx_hal.h"   /* __get_PRIMASK / __disable_irq / __set_PRIMASK */

#include "cli.h"
#define LOG_TAG "wauto"
#include "log.h"
#include "rtl_link.h"        /* rtl_link_forget_gen: the module identity this is armed against */
#if BSP_ENABLE_KV
#include "kv_boot.h"         /* kv_boot_autoreconnect_read(): wifi.autoreconnect (issue #240) */
#endif

/* ---- state ------------------------------------------------------------------- */

/*
 * `armed` is read from erpc_call_ex()'s abort hook about once a millisecond, so it is the
 * one field with a lock-free reader and therefore the one that must be volatile.  Every
 * writer below still updates it inside the critical section with the rest, so a reader
 * that sees it false is guaranteed to be looking at a fully-disarmed state.
 */
static volatile bool wa_armed;

/*
 * Whether to keep credentials at all is the stored wifi.autoreconnect (issue #240): on
 * only when it reads true.  Holding a passphrase in host RAM (the cost is stated at
 * wifi_auto.h) is the operator's choice, made once in the store, so anything short of a
 * clear yes -- false, no key, another type, a failed read, no store in this build --
 * is off, and with nothing in the store the default is off.  Until issue #240 it was ON
 * at boot, with `wifi autoreconnect off` the only way out for the session; that command
 * is gone.  It had been made a default because a choice typed at the console was lost
 * at every reset, so it was off in most of the outages it exists for.  A choice kept in
 * the store survives the reset, which removes that reason.
 *
 * There is no on / off flag here.  Each association reads the key at its start and
 * passes what it read to wifi_auto_arm() (the boot's association and one typed on the
 * USB console can both be between their read and their arm at once, so a shared flag
 * would hand one of them the other's answer), and `wifi status` reads the key itself.
 * A change of the key therefore shows at once in `wifi status` and takes effect at the
 * next `wifi connect` or `wifi reconnect` (or at the boot's association).
 */
static char     wa_ssid[WIFI_AUTO_SSID_MAX + 1u];
static char     wa_pass[WIFI_AUTO_PASS_MAX + 1u];
static bool     wa_open;                 /* no passphrase (open network) */
static uint32_t wa_security;

/* The module identity the credentials belong to.  rtl_link_forget_module() advances this
 * counter on every path that changes what is in front of us -- a CHIP_EN move, a flash
 * session, a fresh power-on -- so a mismatch means these credentials are for a module that
 * is no longer there.  The shell disarms explicitly on all of those paths; this is the
 * backstop for one that is ever missed. */
static uint32_t wa_forget_gen;

/* Backoff and tally. */
static ULONG    wa_next_try;             /* tx_time_get() deadline */
static uint32_t wa_backoff_ms;
static uint32_t wa_attempts, wa_ok, wa_failed;
static ULONG    wa_last_try;
static bool     wa_last_try_valid;
static int      wa_last_rc;
static int32_t  wa_last_result;
static const char *wa_why = "no association since boot";

/* ---- the critical section ---------------------------------------------------- */

/*
 * Short PRIMASK sections, not a mutex: every one of these is a handful of stores, and the
 * state has to be consistent for a reader that cannot block (the abort hook) and for one
 * that runs on the owner thread while a CLI thread is disarming.  Same instrument, and the
 * same reason, as link_bridge.c's nxn_mod_accumulate().
 */
static uint32_t wa_lock(void)
{
	uint32_t prim = __get_PRIMASK();
	__disable_irq();
	return prim;
}

static void wa_unlock(uint32_t prim)
{
	__set_PRIMASK(prim);
}

/* Wipe the credentials.  Call with the section held. */
static void wa_clear_locked(const char *why)
{
	memset(wa_ssid, 0, sizeof wa_ssid);
	memset(wa_pass, 0, sizeof wa_pass);
	wa_open      = false;
	wa_security  = 0u;
	wa_armed     = false;
	wa_backoff_ms = 0u;
	if (why != NULL)
		wa_why = why;
}

/* ---- enable / arm / disarm --------------------------------------------------- */

enum wifi_auto_setting wifi_auto_setting_read(bool warn, const char **store_state)
{
	if (store_state != NULL)
		*store_state = "";
#if BSP_ENABLE_KV
	return kv_boot_autoreconnect_read(warn, store_state);
#else
	(void)warn;
	return WIFI_AUTO_SET_NO_KV;
#endif
}

/*
 * @keep is what THIS association read at its start (issue #240 review): the boot's
 * association and one typed on the USB console can both be between their read and
 * this call, so nothing shared may stand in for it.
 */
bool wifi_auto_arm(const char *ssid, const char *password, uint32_t security, bool keep)
{
	uint32_t prim;
	bool was;

	if (ssid == NULL || strlen(ssid) > WIFI_AUTO_SSID_MAX ||
	    (password != NULL && strlen(password) > WIFI_AUTO_PASS_MAX))
		return false;                    /* `wifi connect` bounds these too */

	prim = wa_lock();
	if (!keep) {
		/* Opt-in: this association read no yes, so no passphrase in RAM -- and
		 * none held from an association that ended before this one did: the
		 * module is now joined to what THIS one asked for. */
		was = wa_armed;
#if BSP_ENABLE_KV
		wa_clear_locked("wifi.autoreconnect was not true at the last association");
#else
		wa_clear_locked("this build has no configuration store, so nothing is kept");
#endif
		wa_unlock(prim);
		if (was)
			LOG_INF("disarmed (wifi.autoreconnect was not true)");
		return false;
	}
	wa_clear_locked(NULL);
	memcpy(wa_ssid, ssid, strlen(ssid));
	if (password != NULL)
		memcpy(wa_pass, password, strlen(password));
	else
		wa_open = true;
	wa_security   = security;
	wa_forget_gen = rtl_link_forget_gen();
	wa_next_try   = tx_time_get();
	wa_backoff_ms = 0u;
	wa_why        = "";
	wa_armed      = true;
	wa_unlock(prim);
	LOG_INF("armed for \"%s\"", ssid);
	return true;
}

void wifi_auto_disarm(const char *why)
{
	uint32_t prim = wa_lock();
	bool was = wa_armed;

	wa_clear_locked((why != NULL) ? why : "disarmed");
	wa_unlock(prim);
	if (was)
		LOG_INF("disarmed (%s)", (why != NULL) ? why : "disarmed");
}

bool wifi_auto_armed(void)
{
	return wa_armed;
}

/*
 * The forget-generation check, on the paths that may take a lock and print -- never on
 * wifi_auto_armed().  Returns true if the credentials are still for the module in front
 * of us; disarms and returns false if they are not.
 */
static bool wa_check_gen(void)
{
	uint32_t prim;
	bool stale;

	if (!wa_armed)
		return false;
	prim  = wa_lock();
	stale = (wa_armed && wa_forget_gen != rtl_link_forget_gen());
	if (stale)
		wa_clear_locked("the module was power-cycled or reflashed");
	wa_unlock(prim);
	if (stale)
		LOG_INF("disarmed (the module was power-cycled or reflashed)");
	return !stale && wa_armed;
}

bool wifi_auto_should_try(void)
{
	uint32_t prim;
	bool go;

	if (!wa_check_gen())
		return false;
	prim = wa_lock();
	go = wa_armed && (int32_t)(tx_time_get() - wa_next_try) >= 0;
	wa_unlock(prim);
	return go;
}

/* ---- the attempt ------------------------------------------------------------- */

int wifi_auto_attempt(const struct wifi_rpc_opts *o)
{
	char ssid[WIFI_AUTO_SSID_MAX + 1u], pass[WIFI_AUTO_PASS_MAX + 1u];
	uint32_t security, prim;
	int32_t result = -1;
	bool open;
	int rc;

	/* Take a private copy under the section: the call below blocks for many seconds and
	 * a CLI thread may zero the originals while it does (that IS the abort). */
	prim = wa_lock();
	if (!wa_armed) {
		wa_unlock(prim);
		return -1;
	}
	memcpy(ssid, wa_ssid, sizeof ssid);
	memcpy(pass, wa_pass, sizeof pass);
	open     = wa_open;
	security = wa_security;
	wa_attempts++;
	wa_last_try       = tx_time_get();
	wa_last_try_valid = true;
	wa_unlock(prim);

	rc = wifi_rpc_connect(o, ssid, open ? NULL : pass, security, &result);
	memset(pass, 0, sizeof pass);        /* no passphrase left on the owner's stack */

	prim = wa_lock();
	wa_last_rc     = rc;
	wa_last_result = result;
	if (rc == -4) {
		/* Aborted, which means somebody disarmed us: not a network failure, so it
		 * neither counts nor backs off.  The caller must now leave the link alone --
		 * the module is still inside wifi_connect(). */
		wa_unlock(prim);
		return -4;
	}
	if (rc == 0 && result == WIFI_RPC_OK) {
		wa_ok++;
		wa_backoff_ms = 0u;
		wa_next_try   = tx_time_get();
		wa_why        = "";
		wa_unlock(prim);
		LOG_INF("re-associated with \"%s\"", ssid);
		return 0;
	}
	wa_failed++;
	wa_backoff_ms = (wa_backoff_ms == 0u) ? WIFI_AUTO_BACKOFF_MIN_MS
	                                      : (wa_backoff_ms * 2u);
	if (wa_backoff_ms > WIFI_AUTO_BACKOFF_MAX_MS)
		wa_backoff_ms = WIFI_AUTO_BACKOFF_MAX_MS;
	wa_next_try = tx_time_get() + (ULONG)wa_backoff_ms;
	wa_unlock(prim);
	LOG_WRN("attempt failed (rc %d, result %ld), retrying in %u ms",
	        rc, (long)result, (unsigned)wa_backoff_ms);
	return (rc != 0) ? rc : -1;
}

/* ---- the report -------------------------------------------------------------- */

/*
 * The text of the setting, against what is held (issue #240).  The heading is the
 * store as it is NOW; the second line, when there is one, says what the next
 * association does about the difference, or how to turn it on.  A store that cannot
 * be used is said to be so, and nothing it would refuse (`kv set`, `wifi reconnect`)
 * is suggested.
 */
struct wifi_auto_status_text wifi_auto_status_text(enum wifi_auto_setting s, bool armed)
{
	static const char next_keeps[] =
		"  the next successful `wifi connect` / `wifi reconnect` keeps the credentials\r\n";
	/* What is held stays until the next association, which always drops it first
	 * (and then keeps its own only if the key reads true then); until that one, a
	 * lost link is re-joined with it.  Said without claiming that a re-association
	 * is running now, or what the next association will read. */
	static const char next_drops[] =
		"  held until the next `wifi connect` / `wifi reconnect`; if the link is lost "
		"before then, it re-associates with it\r\n";
	static const char turn_on[] =
		"  `kv set wifi.autoreconnect true bool`, then `wifi reconnect` (or "
		"`wifi connect`) to arm it\r\n";
	struct wifi_auto_status_text t = { NULL, NULL };

	switch (s) {
	case WIFI_AUTO_SET_ON:
		t.heading = "wifi: autoreconnect on (wifi.autoreconnect is true)\r\n";
		t.next = armed ? NULL : next_keeps;
		return t;
	case WIFI_AUTO_SET_FALSE:
		t.heading = "wifi: autoreconnect off (wifi.autoreconnect is false)\r\n";
		t.next = armed ? next_drops : turn_on;
		return t;
	case WIFI_AUTO_SET_MISSING:
		t.heading = "wifi: autoreconnect off (wifi.autoreconnect is not set)\r\n";
		t.next = armed ? next_drops : turn_on;
		return t;
	case WIFI_AUTO_SET_NOT_BOOL:
		t.heading = "wifi: autoreconnect off (wifi.autoreconnect is not a bool -- "
		            "it was stored without `bool`)\r\n";
		t.next = armed ? next_drops : turn_on;
		return t;
	case WIFI_AUTO_SET_UNREADABLE:
		t.heading = "wifi: autoreconnect off (wifi.autoreconnect could not be read "
		            "-- the store is busy or failing)\r\n";
		t.next = armed ? next_drops : NULL;
		return t;
	case WIFI_AUTO_SET_NO_STORE:
		t.heading = "wifi: autoreconnect off (the configuration store is not open: "
		            "%s)\r\n";
		t.next = armed ? next_drops : NULL;
		return t;
	case WIFI_AUTO_SET_NO_KV:
		t.heading = "wifi: autoreconnect off\r\n";
		/* Nothing is ever held in this build (every association reads off). */
		t.next = "  this build has no configuration store, so it stays off\r\n";
		return t;
	}
	/* Not a value of the enum: say off, suggest nothing. */
	t.heading = "wifi: autoreconnect off (unknown setting)\r\n";
	return t;
}

/*
 * The end of `wifi status` (issue #240; until then the report of `wifi autoreconnect`,
 * which is gone).  Printed on every way `wifi status` answers -- the module powered off
 * included -- after the link is given back: the store is read here, and that read may
 * wait for the NOR.  The read does not warn (the heading says it), so `wifi status`
 * adds nothing to dmesg.
 */
void wifi_auto_print(struct cli_instance *sh)
{
	uint32_t prim;
	char ssid[WIFI_AUTO_SSID_MAX + 1u];
	bool armed, open;
	uint32_t security, backoff, attempts, ok, failed;
	ULONG now, next, last;
	bool last_valid;
	int last_rc;
	int32_t last_result;
	const char *why, *store_state = "";
	enum wifi_auto_setting setting;
	struct wifi_auto_status_text text;

	/* First, so the snapshot below is not older than a wait for the store. */
	setting = wifi_auto_setting_read(false, &store_state);
	(void)wa_check_gen();

	prim = wa_lock();
	armed       = wa_armed;
	open        = wa_open;
	security    = wa_security;
	backoff     = wa_backoff_ms;
	attempts    = wa_attempts;
	ok          = wa_ok;
	failed      = wa_failed;
	next        = wa_next_try;
	last        = wa_last_try;
	last_valid  = wa_last_try_valid;
	last_rc     = wa_last_rc;
	last_result = wa_last_result;
	why         = wa_why;
	memcpy(ssid, wa_ssid, sizeof ssid);
	wa_unlock(prim);
	now = tx_time_get();

	text = wifi_auto_status_text(setting, armed);
	cli_print(sh, text.heading, (store_state != NULL) ? store_state : "");
	if (armed)
		cli_print(sh, "  armed for \"%s\" (%s, sec 0x%08lX)\r\n", ssid,
		          open ? "open" : "passphrase held in RAM", (unsigned long)security);
	else
		cli_print(sh, "  not armed%s%s\r\n", (why && why[0]) ? " -- " : "",
		          (why != NULL) ? why : "");

	cli_print(sh, "  attempts %lu, ok %lu, failed %lu\r\n",
	          (unsigned long)attempts, (unsigned long)ok, (unsigned long)failed);
	if (last_valid)
		cli_print(sh, "  last attempt %lu s ago (rc %d, result %ld)\r\n",
		          (unsigned long)((now - last) / 1000u), last_rc, (long)last_result);
	if (armed && backoff != 0u) {
		int32_t left = (int32_t)(next - now);
		cli_print(sh, "  backoff %u s, next try in %ld s\r\n",
		          (unsigned)(backoff / 1000u), (long)((left > 0) ? left / 1000 : 0));
	}
	if (text.next != NULL)
		cli_print(sh, "%s", text.next);
}
