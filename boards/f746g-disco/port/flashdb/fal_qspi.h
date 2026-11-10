/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    fal_qspi.h
 * @brief   This board's side of the shared key-value store (issue #214): the
 *          lock that serialises the store, created once.
 *
 * port/flashdb/fal_qspi.c defines the store's state and facts (svc/kv/
 * kv_board.h) and the FAL device over the first megabyte of the QSPI NOR.  Its
 * hold -- what every entry point of svc/kv/kv.c takes around its whole
 * operation -- is kv's own mutex (waited for) and then the blob store's busy
 * flag (src/blob.h, tried until a deadline), so that every FlashDB access on
 * this board, reads included, runs under that flag: FlashDB rewrites a record's
 * state while it reads (lib/flashdb/src/fdb_kvdb.c read_kv()), and the rule on
 * this board is that the QSPI is programmed and erased under the flag only.
 */
#ifndef F746_FAL_QSPI_H
#define F746_FAL_QSPI_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Create the store's lock.  Object creation only: call it once from
 * tx_application_define(), before any thread can reach the store.  0, or -1
 * when the lock could not be created -- every entry point of the store then
 * answers "not ready" (KV_ERR_STATE) and never touches the flag or the flash.
 */
int kv_qspi_init(void);

/**
 * Which thread kv's record names as holding the busy flag for the store, or
 * NULL when the store holds nothing.  Reads the record only (for the host test
 * and a diagnostic; the FAL device's write and erase compare it with the
 * calling thread themselves).
 */
const void *kv_qspi_holder(void);

#ifdef __cplusplus
}
#endif

#endif /* F746_FAL_QSPI_H */
