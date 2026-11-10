/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Host stand-in for tx_api.h, for test_kv_qspi.c (issue #214): what
 * port/flashdb/fal_qspi.c and src/kv_boot.c use -- a mutex, the calling
 * thread, a clock and a sleep.  The test defines the functions: the "current
 * thread" is a pointer it switches, the clock only moves when something sleeps
 * or waits, and a wait the mutex cannot satisfy in one thread spends its budget
 * and times out (a wait without limit is counted as a deadlock).
 */
#ifndef KV_SHIM_TX_API_H
#define KV_SHIM_TX_API_H

typedef unsigned long ULONG;
typedef unsigned int  UINT;
typedef unsigned char UCHAR;
typedef void          VOID;
typedef char          CHAR;

typedef struct TX_THREAD_STRUCT {
	const char *name;
} TX_THREAD;

typedef struct TX_MUTEX_STRUCT {
	const char *name;
	TX_THREAD  *owner;
	unsigned    count;
	int         created;
} TX_MUTEX;

#define TX_NULL            ((void *)0)
#define TX_SUCCESS         0x00u
#define TX_NOT_AVAILABLE   0x1Du
#define TX_NOT_OWNED       0x1Eu
#define TX_MUTEX_ERROR     0x1Cu
#define TX_WAIT_FOREVER    0xFFFFFFFFUL
#define TX_NO_WAIT         0u
#define TX_INHERIT         1u
#define TX_NO_TIME_SLICE   0u
#define TX_AUTO_START      1u

UINT       tx_mutex_create(TX_MUTEX *m, CHAR *name, UINT inherit);
UINT       tx_mutex_get(TX_MUTEX *m, ULONG wait);
UINT       tx_mutex_put(TX_MUTEX *m);
TX_THREAD *tx_thread_identify(void);
UINT       tx_thread_sleep(ULONG ticks);
ULONG      tx_time_get(void);

#endif /* KV_SHIM_TX_API_H */
