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

#ifdef __cplusplus
}
#endif

#endif /* APP_KV_BOOT_H */
