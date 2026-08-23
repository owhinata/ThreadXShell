/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    cli_registry.h
 * @brief   Core-internal: the thread->instance registry entry type and the
 *          decisions taken over that table (issues #28, #81).
 *
 * The registry TABLE lives in cli_core.c, which alone owns the
 * interrupt-disabled critical section around it.  What lives here is every
 * DECISION taken inside that section: which slot a registration may take and
 * why one is refused (#81), which entries a removal clears (#81), and which
 * entries count as running consoles (#28).  None of it calls a tx_* service, so
 * it compiles on the host and is unit-tested there (shell/test/test_registry.c,
 * shell/test/test_console.c) -- cli_core.c itself cannot be, because it reaches
 * for ThreadX and for an MRS on IPSR.
 *
 * These helpers are deterministic and ThreadX/hardware-free, but NOT pure: they
 * write the caller's table.  The caller owns the mutual exclusion.
 *
 * Unrelated to cli_console_claim() / cli_console_release() (cli_core.c), which
 * are about OWNING the console for a binary transfer.
 */
#ifndef CLI_REGISTRY_H
#define CLI_REGISTRY_H

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
 * Add (@p t, @p sh) to @p map, or say why it cannot be added (issue #81).
 *
 * The table's invariant is that a thread appears at most once and an instance
 * appears at most once among the OCCUPIED entries, and until #81 it was kept by
 * the CALLERS rather than by this function.  Occupied means `sh != NULL`: that
 * has always been the slot marker, so a slot with a stale `thread` and no `sh`
 * is free, is reused (which repairs it), and is not a duplicate of anything.  A duplicate thread is what leaves an orphan: removal matches
 * on the thread, so only one of the two entries goes, and the survivor holds a
 * slot for a thread that is gone.  A duplicate instance does not orphan by
 * itself -- each thread clears its own entry -- but both are refused, because
 * every caller passes an instance together with the TX_THREAD embedded in it,
 * so the two conditions coincide and refusing both keeps each reader of this
 * table unambiguous.
 *
 * [!] The reason is decided from the WHOLE table, not from the first match.
 * The same pair matches both conditions, and a damaged table can hold a thread
 * duplicate and an instance duplicate in different entries -- returning
 * whichever was seen first would make the reason depend on entry order.  The
 * scan therefore collects the first free slot and both duplicate flags, then
 * decides in this fixed order:
 *
 *     bad argument -> duplicate thread -> duplicate instance -> full -> added
 *
 * so "full AND duplicate" never reports "full", and the same pair always
 * reports a duplicate thread.  Both duplicate tests look only at occupied
 * entries, per the paragraph above.
 *
 * On CLI_REG_OK the entry is written with `sh` LAST (see cli_reg_remove() for
 * the mirror).  On every refusal the table is left byte-for-byte unchanged.
 *
 * The outcome type is the PUBLIC enum cli_reg_status (cli_instance.h), which
 * carries the ordering contract -- one set of names rather than an internal
 * enum plus a mapping, so the two cannot drift apart.
 */
enum cli_reg_status cli_reg_add(struct cli_thread_map *map, size_t map_len,
                                TX_THREAD *t, struct cli_instance *sh);

/**
 * One-line reason for @p st, for a caller that has to tell a human why a
 * registration failed.  Never NULL.
 */
const char *cli_reg_strerror(enum cli_reg_status st);

/**
 * Clear EVERY entry of @p map whose `.thread` is @p t (issue #81).
 *
 * Postcondition: on return no entry has `.thread == t`.  That is the whole
 * contract -- it does not depend on cli_reg_add() having kept the table clean,
 * which is the point: the two functions face one invariant instead of relying
 * on each other's.  Entries that do not match are left byte-for-byte unchanged;
 * a NULL @p t and a thread that is not present are no-ops.
 *
 * Sweeping is unreachable from healthy firmware, since cli_reg_add() refuses to
 * create a second entry for a thread.  It is still checkable, because a test
 * can build the damaged table by hand -- which is why this is not a gate nobody
 * has seen fail.
 *
 * [!] Each entry is retracted `sh` first, `thread` second (cli_reg_add()
 * publishes in the opposite order).  A unit test cannot observe that -- it sees
 * only the state on return -- so the ordering is an implementation rule held by
 * review, kept for a future reader that scans without the lock.
 *
 * @return how many entries were cleared (0 or, on a damaged table, more than 1)
 */
size_t cli_reg_remove(struct cli_thread_map *map, size_t map_len,
                      const TX_THREAD *t);

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

#endif /* CLI_REGISTRY_H */
