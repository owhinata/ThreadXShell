/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
#ifndef MALLOC_LOCK_H
#define MALLOC_LOCK_H

/* Create the mutex that serialises the newlib heap.  Call once from
   tx_application_define(), before any thread can allocate.  Fail-stops rather
   than returning an error; see malloc_lock.c. */
void malloc_lock_init(void);

#endif /* MALLOC_LOCK_H */
