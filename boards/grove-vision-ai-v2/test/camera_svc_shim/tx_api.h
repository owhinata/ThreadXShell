/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Host stand-in for the ThreadX names port/camera/camera_svc_grove.c uses
 * (issue #237, test_camera_svc_grove.c): the critical section is empty on
 * the host and the current thread is one fixed value.
 */
#ifndef TX_API_H
#define TX_API_H

typedef struct TX_THREAD_STRUCT { int unused; } TX_THREAD;

#define TX_INTERRUPT_SAVE_AREA
#define TX_DISABLE
#define TX_RESTORE

TX_THREAD *tx_thread_identify(void);

#endif /* TX_API_H */
