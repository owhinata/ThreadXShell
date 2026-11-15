/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Wio Lite AI ThreadX Shell Project
 */
/*
 * Wio Lite AI -- what the stored WiFi settings say to do (issue #238).  The rules
 * and the order of the reads are in kv_wifi.h; this is the function.
 */
#include "kv_wifi.h"
#include "wifi_rpc.h"   /* WIFI_RPC_SEC_*: constants only */

#include <stddef.h>

static int seen_ok(enum kv_wifi_seen s)
{
	return s == KV_WIFI_UNREAD || s == KV_WIFI_NO || s == KV_WIFI_YES;
}

static struct kv_wifi_answer step(enum kv_wifi_step s)
{
	struct kv_wifi_answer a = { s, 0u };

	return a;
}

struct kv_wifi_answer kv_wifi_next(const struct kv_wifi_in *in)
{
	struct kv_wifi_answer a;

	if (in == NULL ||
	    (in->caller != KV_WIFI_BOOT && in->caller != KV_WIFI_CONSOLE) ||
	    !seen_ok(in->enable) || !seen_ok(in->autoconnect) || !seen_ok(in->ssid) ||
	    !seen_ok(in->psk) || !seen_ok(in->security))
		return step(KV_WIFI_BAD_CALL);

	if (!in->store_open)
		return step(KV_WIFI_NO_STORE);

	/* Only the boot asks these two: a console command connects on the
	 * operator's word, whatever they hold. */
	if (in->caller == KV_WIFI_BOOT) {
		if (in->enable == KV_WIFI_UNREAD)
			return step(KV_WIFI_READ_ENABLE);
		if (in->enable != KV_WIFI_YES)
			return step(KV_WIFI_RADIO_OFF);
		if (in->autoconnect == KV_WIFI_UNREAD)
			return step(KV_WIFI_READ_AUTOCONNECT);
		if (in->autoconnect != KV_WIFI_YES)
			return step(KV_WIFI_POWER_ONLY);
	}

	if (in->ssid == KV_WIFI_UNREAD)
		return step(KV_WIFI_READ_SSID);
	if (in->ssid != KV_WIFI_YES)
		return step(KV_WIFI_NO_SSID);
	if (in->psk == KV_WIFI_UNREAD)
		return step(KV_WIFI_READ_PSK);
	if (in->security == KV_WIFI_UNREAD)
		return step(KV_WIFI_READ_SECURITY);

	a = step(KV_WIFI_CONNECT);
	a.default_security = (in->psk == KV_WIFI_YES) ? WIFI_RPC_SEC_WPA2_AES_PSK
	                                              : WIFI_RPC_SEC_OPEN;
	return a;
}
