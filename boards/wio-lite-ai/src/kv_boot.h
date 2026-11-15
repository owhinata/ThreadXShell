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

#ifdef __cplusplus
}
#endif

#endif /* APP_KV_BOOT_H */
