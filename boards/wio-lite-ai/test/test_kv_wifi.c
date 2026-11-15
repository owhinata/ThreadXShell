/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Host test for what the stored WiFi settings say to do (issue #238,
 * src/kv_wifi.c).
 *
 * The function is driven the way src/kv_boot.c drives it: every key starts
 * UNREAD, each READ_* answer is served from a fake store and recorded, and the
 * walk ends on the first answer that is not a read.  Every combination of the
 * five keys being found or not is walked, for both callers and with the store
 * open and closed, and two things are checked for each:
 *
 *   - the ANSWER, against the table of the plan of #238 for a console command,
 *     and against the boot sequence as kv_boot.c wrote it before #238 (written
 *     out again below as old_boot(), straight from that code) for the boot;
 *   - the READS, in order: a read the boot did not do would add a warning line
 *     to `dmesg` when that key is unreadable, so the boot must read exactly
 *     what it read before, and the console must never read wifi.enable or
 *     wifi.autoconnect.  Every read asked for must be of a key still unread,
 *     which is what bounds the caller's loop at KV_WIFI_KEYS.
 *
 * [!] WHAT IT DOES NOT COVER: the lines kv_boot.c prints for each answer, and
 * that it passes the read values on to wifi_connect_run().
 */
#include <stdio.h>
#include <string.h>

#include "kv_wifi.h"
#include "wifi_rpc.h"

static int fails;
static int checks;

#define EQ(got, want, what)                                                  \
	do {                                                                 \
		unsigned long g_ = (unsigned long)(got);                     \
		unsigned long w_ = (unsigned long)(want);                    \
		checks++;                                                    \
		if (g_ != w_) {                                              \
			printf("  FAIL %s:%d %s: got 0x%lx, wanted 0x%lx\n", \
			       __func__, __LINE__, what, g_, w_);            \
			fails++;                                             \
		}                                                            \
	} while (0)

#define STREQ(got, want, what)                                               \
	do {                                                                 \
		checks++;                                                    \
		if (strcmp((got), (want)) != 0) {                            \
			printf("  FAIL %s:%d %s: read \"%s\", wanted \"%s\"\n", \
			       __func__, __LINE__, what, (got), (want));     \
			fails++;                                             \
		}                                                            \
	} while (0)

/* The keys, as bits of a "found" mask, and the letter each read is logged as. */
enum { K_ENABLE, K_AUTOCONNECT, K_SSID, K_PSK, K_SECURITY, K_N };
static const char key_letter[K_N] = { 'E', 'A', 'S', 'P', 'T' };

#define FOUND(mask, k) (((mask) >> (k)) & 1u)

struct walk {
	struct kv_wifi_answer a;
	char reads[K_N * 2 + 1];   /* room for a misbehaving walk to show itself */
};

/* Drive kv_wifi_next() as kv_boot.c does, over a store that finds the keys
 * whose bits are set in `found`. */
static struct walk walk(enum kv_wifi_caller caller, int store_open, unsigned found)
{
	struct kv_wifi_in in;
	struct walk w;
	size_t n = 0;
	enum kv_wifi_seen *slot;
	int k;

	memset(&in, 0, sizeof in);
	memset(&w, 0, sizeof w);
	in.caller = caller;
	in.store_open = store_open;

	for (;;) {
		w.a = kv_wifi_next(&in);
		switch (w.a.step) {
		case KV_WIFI_READ_ENABLE:      k = K_ENABLE;      slot = &in.enable;      break;
		case KV_WIFI_READ_AUTOCONNECT: k = K_AUTOCONNECT; slot = &in.autoconnect; break;
		case KV_WIFI_READ_SSID:        k = K_SSID;        slot = &in.ssid;        break;
		case KV_WIFI_READ_PSK:         k = K_PSK;         slot = &in.psk;         break;
		case KV_WIFI_READ_SECURITY:    k = K_SECURITY;    slot = &in.security;    break;
		default:
			return w;
		}
		/* A read of a key already read would never end the walk. */
		EQ(*slot, KV_WIFI_UNREAD, "a read is asked for only once");
		if (n >= K_N * 2) {
			printf("  FAIL walk: more than %d reads\n", K_N * 2);
			fails++;
			return w;
		}
		w.reads[n++] = key_letter[k];
		*slot = kv_wifi_seen_of((int)FOUND(found, k));
	}
}

/*
 * The boot as src/kv_boot.c's cfg_wifi() wrote it before #238 (main 8c2a776),
 * reduced to its reads and its outcome.  kv_boot_entry() only called it with
 * the store open.
 */
static struct walk old_boot(unsigned found)
{
	struct walk w;
	size_t n = 0;

	memset(&w, 0, sizeof w);
	w.reads[n++] = 'E';                               /* kv_cfg_bool("wifi.enable") */
	if (!FOUND(found, K_ENABLE)) {
		w.a.step = KV_WIFI_RADIO_OFF;
		return w;
	}
	w.reads[n++] = 'A';                               /* kv_cfg_bool("wifi.autoconnect") */
	if (!FOUND(found, K_AUTOCONNECT)) {
		w.a.step = KV_WIFI_POWER_ONLY;
		return w;
	}
	w.reads[n++] = 'S';                               /* kv_cfg_str("wifi.ssid") */
	if (!FOUND(found, K_SSID)) {
		w.a.step = KV_WIFI_NO_SSID;
		return w;
	}
	w.reads[n++] = 'P';                               /* have_psk = kv_cfg_str("wifi.psk") */
	w.reads[n++] = 'T';                               /* kv_get_u32("wifi.security") */
	w.a.step = KV_WIFI_CONNECT;
	w.a.default_security = FOUND(found, K_PSK) ? WIFI_RPC_SEC_WPA2_AES_PSK
	                                           : WIFI_RPC_SEC_OPEN;
	return w;
}

/* The boot, every combination: the same reads in the same order and the same
 * answer as before #238. */
static void test_boot_unchanged(void)
{
	unsigned found;

	for (found = 0; found < (1u << K_N); found++) {
		struct walk got = walk(KV_WIFI_BOOT, 1, found);
		struct walk want = old_boot(found);

		EQ(got.a.step, want.a.step, "boot answer");
		EQ(got.a.default_security, want.a.default_security, "boot security");
		STREQ(got.reads, want.reads, "boot reads");
	}
}

/* The console command, every combination: the table of the plan of #238. */
static void test_console_table(void)
{
	unsigned found;

	for (found = 0; found < (1u << K_N); found++) {
		struct walk got = walk(KV_WIFI_CONSOLE, 1, found);

		if (!FOUND(found, K_SSID)) {
			/* No saved network: refused after one read, whatever
			 * wifi.enable and wifi.autoconnect hold. */
			EQ(got.a.step, KV_WIFI_NO_SSID, "console, no wifi.ssid");
			EQ(got.a.default_security, 0, "console, no wifi.ssid: no security");
			STREQ(got.reads, "S", "console, no wifi.ssid: reads");
			continue;
		}
		/* wifi.enable unset or false and wifi.autoconnect false do not
		 * stop it: those two are never read. */
		EQ(got.a.step, KV_WIFI_CONNECT, "console connects");
		EQ(got.a.default_security,
		   FOUND(found, K_PSK) ? WIFI_RPC_SEC_WPA2_AES_PSK : WIFI_RPC_SEC_OPEN,
		   "console default security");
		STREQ(got.reads, "SPT", "console reads");
	}
}

/* The store not open: refused before any read, for either caller. */
static void test_store_closed(void)
{
	unsigned found;

	for (found = 0; found < (1u << K_N); found++) {
		struct walk b = walk(KV_WIFI_BOOT, 0, found);
		struct walk c = walk(KV_WIFI_CONSOLE, 0, found);

		EQ(b.a.step, KV_WIFI_NO_STORE, "boot, store closed");
		STREQ(b.reads, "", "boot, store closed: no reads");
		EQ(c.a.step, KV_WIFI_NO_STORE, "console, store closed");
		STREQ(c.reads, "", "console, store closed: no reads");
	}
}

/* A few rows spelled out, so a reader sees them without the loops. */
static void test_rows(void)
{
	struct kv_wifi_in in;
	struct kv_wifi_answer a;

	/* Console: wifi.enable false, wifi.autoconnect false, wifi.ssid and a psk
	 * present, no wifi.security -> connect, WPA2-AES. */
	memset(&in, 0, sizeof in);
	in.caller = KV_WIFI_CONSOLE;
	in.store_open = 1;
	in.enable = KV_WIFI_NO;
	in.autoconnect = KV_WIFI_NO;
	in.ssid = KV_WIFI_YES;
	in.psk = KV_WIFI_YES;
	in.security = KV_WIFI_NO;
	a = kv_wifi_next(&in);
	EQ(a.step, KV_WIFI_CONNECT, "console row: connects with enable/autoconnect off");
	EQ(a.default_security, WIFI_RPC_SEC_WPA2_AES_PSK, "console row: WPA2-AES");

	/* The same keys at boot -> the radio stays off. */
	in.caller = KV_WIFI_BOOT;
	a = kv_wifi_next(&in);
	EQ(a.step, KV_WIFI_RADIO_OFF, "boot row: radio off");
	EQ(a.default_security, 0, "boot row: no security");

	/* Boot, enabled but not autoconnect -> power only. */
	in.enable = KV_WIFI_YES;
	a = kv_wifi_next(&in);
	EQ(a.step, KV_WIFI_POWER_ONLY, "boot row: power only");

	/* Boot, autoconnect, no psk, no wifi.security -> connect, open. */
	in.autoconnect = KV_WIFI_YES;
	in.psk = KV_WIFI_NO;
	a = kv_wifi_next(&in);
	EQ(a.step, KV_WIFI_CONNECT, "boot row: connects");
	EQ(a.default_security, WIFI_RPC_SEC_OPEN, "boot row: open");
}

/* What it does with a call it does not understand: nothing. */
static void test_bad_call(void)
{
	struct kv_wifi_in in;

	EQ(kv_wifi_next(NULL).step, KV_WIFI_BAD_CALL, "NULL input");

	memset(&in, 0, sizeof in);
	in.store_open = 1;
	in.caller = (enum kv_wifi_caller)0;
	EQ(kv_wifi_next(&in).step, KV_WIFI_BAD_CALL, "caller 0");
	in.caller = (enum kv_wifi_caller)3;
	EQ(kv_wifi_next(&in).step, KV_WIFI_BAD_CALL, "caller 3");

	in.caller = KV_WIFI_CONSOLE;
	in.ssid = (enum kv_wifi_seen)3;
	EQ(kv_wifi_next(&in).step, KV_WIFI_BAD_CALL, "a key state out of range");
	in.ssid = KV_WIFI_UNREAD;
	in.enable = (enum kv_wifi_seen)7;   /* not read by the console, still checked */
	EQ(kv_wifi_next(&in).step, KV_WIFI_BAD_CALL, "an unread-by-console key out of range");
}

int main(void)
{
	test_boot_unchanged();
	test_console_table();
	test_store_closed();
	test_rows();
	test_bad_call();

	if (fails) {
		printf("test_kv_wifi: %d of %d checks FAILED\n", fails, checks);
		return 1;
	}
	printf("test_kv_wifi: OK (%d checks)\n", checks);
	return 0;
}
