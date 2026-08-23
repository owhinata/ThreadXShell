/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    cli_console.h
 * @brief   Core-internal: the thread->instance registry entry type and the pure
 *          scan behind cli_console_snapshot() (issue #28).
 *
 * The registry TABLE lives in cli_core.c, which alone mutates it and alone owns
 * the interrupt-disabled critical section around it.  Only the entry type and
 * the scan live here, so the scan -- which decides WHICH instances count as
 * consoles, and how a too-small caller array is reported -- compiles on the host
 * and is unit-tested there (shell/test/test_console.c), exactly like the rest of
 * the tx_*-free core.
 *
 * Unrelated to cli_console_claim() / cli_console_release() (cli_core.c), which
 * are about OWNING the console for a binary transfer, not enumerating consoles.
 */
#ifndef CLI_CONSOLE_H
#define CLI_CONSOLE_H

#include <stddef.h>

#include "cli_instance.h"   /* struct cli_instance, struct cli_console_stat, TX_THREAD */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * One entry of the thread->instance registry (cli_core.c).  `thread` is the key
 * cli_current_instance() matches on; `sh` is the owning instance and doubles as
 * the slot-in-use marker (published last, retracted first).
 */
struct cli_thread_map {
	TX_THREAD           *thread;
	struct cli_instance *sh;
};

/**
 * Walk @p map_len registry entries and copy one row per RUNNING INTERACTIVE
 * console -- an occupied slot (sh != NULL) whose instance is a foreground one
 * (sh->fg == NULL).  See cli_console_snapshot() in cli_instance.h for what that
 * predicate does and does not include; this is its implementation.
 *
 * Pure: calls no tx_* API and takes no lock, because the CALLER holds the
 * critical section that keeps the table still.  Everything it does inside that
 * section is a bounded walk and plain copies -- no string functions, no
 * formatting -- so the section stays short.
 *
 * Duplicate entries are copied as duplicate rows.  Nothing on the current paths
 * registers one live instance twice (cli_start() once per console, cli_job.c
 * once per launch), so folding them would hide a damaged registry.
 *
 * @param map     registry entries to scan (may be NULL only when @p map_len is 0)
 * @param map_len number of entries in @p map
 * @param out     array receiving up to @p cap rows (may be NULL when @p cap is 0)
 * @param cap     capacity of @p out in rows
 * @param found   optional out: total matching consoles seen, which EXCEEDS the
 *                return value when @p out was too small
 * @return number of rows written (0..cap)
 */
size_t cli_console_collect(const struct cli_thread_map *map, size_t map_len,
                           struct cli_console_stat *out, size_t cap,
                           size_t *found);

#ifdef __cplusplus
}
#endif

#endif /* CLI_CONSOLE_H */
