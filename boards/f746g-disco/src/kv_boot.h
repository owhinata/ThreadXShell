/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    kv_boot.h
 * @brief   The configuration thread (issue #214): opens the key-value store
 *          (svc/kv/kv.h) and applies the telnet port and `net.mode` from it.
 */
#ifndef F746_KV_BOOT_H
#define F746_KV_BOOT_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Create the store's lock (port/flashdb/fal_qspi.h) and the configuration
 * thread (src/kv_boot.c), which opens the store once the scheduler runs.  Pure
 * ThreadX object creation -- call it from tx_application_define(), after the
 * network bring-up, with what that bring-up produced:
 *
 * @param have_netx  nonzero when NetX is up: the thread applies `net.mode`,
 *                   with DHCP as this board's default
 * @param have_tty   nonzero when the telnet console's listener exists: the
 *                   thread hands it its port (`net.shell.port`, or 23), once
 *
 * @return 0 when the thread exists -- from then on IT hands over the port and
 *         makes DHCP the source, on every path, once.  -1 when it could not be
 *         created: then the caller does both (23, DHCP), once.
 */
int kv_boot_init(int have_netx, int have_tty);

#ifdef __cplusplus
}
#endif

#endif /* F746_KV_BOOT_H */
