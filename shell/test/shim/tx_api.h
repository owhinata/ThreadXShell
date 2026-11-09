/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Host-only ThreadX type shim for the Shell core unit test.
 *
 * The core splits its ThreadX use into cli_core.c (calls tx_* APIs, firmware
 * only) and cli_session.c (line/dispatch logic, no tx_* calls).  To compile and
 * test cli_session.c on the host we still need `struct cli_instance` -- which
 * embeds ThreadX objects by value -- to be a complete type.  This shim provides
 * just the type names as small dummy structs and the base typedefs; it is found
 * ahead of the real tx_api.h via the test include path.  One ThreadX function
 * is declared: tx_time_get(), which the shared `net` command reads its waits
 * from (shell/cmds/cmd_net.c, issue #219); the test that links that command
 * defines it as its own clock (net_svc_stub.c).  TX_TIMER_TICKS_PER_SECOND is
 * deliberately NOT defined here: that test passes it, so the command's
 * _Static_assert still has to find the board's value.
 *
 * The shared NetX operations (shell/cmds/net_core.c, issue #220) also take a
 * mutex and mask interrupts: the mutex calls and the TX_DISABLE / TX_RESTORE
 * section are declared below as calls into the test (test_net_core.c defines
 * them and records what was held when), and nothing else uses them.
 */
#ifndef TX_API_H
#define TX_API_H

#include <stdint.h>

typedef unsigned long  ULONG;
typedef unsigned int   UINT;
typedef unsigned char  UCHAR;
typedef char           CHAR;
#define VOID void

/* Opaque control blocks, sized only so they can be embedded by value. */
typedef struct { void *opaque; } TX_THREAD;
typedef struct { void *opaque; } TX_EVENT_FLAGS_GROUP;
typedef struct { void *opaque; } TX_MUTEX;

#define TX_SUCCESS ((UINT)0)
#define TX_WAIT_FOREVER ((ULONG)0xFFFFFFFFUL)
#define TX_INHERIT      ((UINT)1)

ULONG tx_time_get(void);

UINT tx_mutex_create(TX_MUTEX *mutex_ptr, CHAR *name_ptr, UINT inherit);
UINT tx_mutex_get(TX_MUTEX *mutex_ptr, ULONG wait_option);
UINT tx_mutex_put(TX_MUTEX *mutex_ptr);

/* The interrupt-masked section, as calls the test counts. */
UINT tx_shim_disable(void);
void tx_shim_restore(UINT saved);
#define TX_INTERRUPT_SAVE_AREA UINT interrupt_save;
#define TX_DISABLE             interrupt_save = tx_shim_disable();
#define TX_RESTORE             tx_shim_restore(interrupt_save);

#endif /* TX_API_H */
