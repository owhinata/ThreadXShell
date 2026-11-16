/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Wio Lite AI ThreadX Shell Project
 */
/**
 * @file    kv_boot.h
 * @brief   The configuration thread (owhinata/wio-lite-ai#37).  The store it opens
 *          is svc/kv/kv.h (issue #214).
 */
#ifndef APP_KV_BOOT_H
#define APP_KV_BOOT_H

#include <stdbool.h>

#include "wifi_auto.h"   /* enum wifi_auto_setting */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Create the configuration thread (src/kv_boot.c), which opens the store once the
 * scheduler is running.  Pure ThreadX object creation -- call it from
 * tx_application_define().  Returns 0 on success.
 */
int kv_boot_init(void);

struct cli_instance;

/**
 * `wifi reconnect` (issue #238): apply the saved WiFi settings and then
 * net.mode, with the boot's own implementation, reporting to @p sh as well as
 * to the log.  wifi.enable and wifi.autoconnect are not read; there must be a
 * wifi.ssid.  net.mode is applied only after a connection.  Refused while the
 * boot configuration or another `wifi reconnect` runs.  The caller has already
 * refused the telnet console and a background job.  Returns 0 when connected
 * (whatever net.mode then did), 1 otherwise; every refusal is printed.
 */
int kv_boot_reconnect(struct cli_instance *sh);

/**
 * wifi.autoreconnect (issue #240): what the store holds now
 * (src/wifi_auto.h; only WIFI_AUTO_SET_ON is on).  A store that is not open is
 * not read.  With @p warn, a key that is there but not taken is reported as
 * `wifi.autoreconnect: ignored (<why>)` to the log (and to a running `wifi
 * reconnect`'s console).  *@p store_state (when not NULL) is kv_state_str()
 * for WIFI_AUTO_SET_NO_STORE, "" otherwise.  Called through
 * wifi_auto_setting_read(): at the start of every association, and by `wifi
 * status`, from any thread; it takes the store's hold for the one read (up to
 * 5 s) and gives it back.
 */
enum wifi_auto_setting kv_boot_autoreconnect_read(bool warn, const char **store_state);

#ifdef __cplusplus
}
#endif

#endif /* APP_KV_BOOT_H */
