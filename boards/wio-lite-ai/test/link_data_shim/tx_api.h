/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Host-test shim for tx_api.h, as src/link_data.c includes it (issue #218).
 *
 * link_data.c calls nothing from ThreadX itself: it reaches tx_event_flags_set()
 * only through erpc_data_posted(), which test_link_send_gen.c defines.  So this
 * header is empty on purpose.
 */
#ifndef LINK_DATA_SHIM_TX_API_H
#define LINK_DATA_SHIM_TX_API_H
#endif /* LINK_DATA_SHIM_TX_API_H */
