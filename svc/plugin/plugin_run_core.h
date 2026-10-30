/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    plugin_run_core.h
 * @brief   The loader's board-facing API with its lease checks, for an
 *          Armv7-M board that stages a container in RAM (wio-lite-ai since
 *          issue #110, f746g-disco since #159).
 *
 * The loader itself is svc/plugin/plugin_exec.c.  Above it, wio-lite-ai's
 * port/plugin/plugin_run.c held two kinds of thing: what touches its hardware
 * (the MPU read-back and the cache maintenance) and what does not -- the plugin
 * lease checked before a load or an unload and again at the branch into
 * entry(), the name of that refusal, the load's log lines, and the check that
 * the image lies inside the staging region the caller was handed.
 * f746g-disco needs the second kind word for word, so since issue #159 it is
 * here, once; each board keeps only its binding:
 *
 *   - plugin_run_env: its state (static, permanent), its reservation (the
 *     linker's __plugin_start / __plugin_end -- the board's fact, stated in its
 *     initialiser, see plugin_exec.h) and its port, built with PLUGIN_RUN_PORT()
 *     around its cache maintenance;
 *   - plugin_run_mpu_ok(): the MPU snapshot, read under a critical section and
 *     judged by svc/plugin/plugin_mpu_v7m.c;
 *   - plugin_run_log(): where the loader's lines go (the board's log);
 *   - plugin_run_note_entry(): where entry()'s stack depth is recorded (its nn
 *     layer).
 *
 * grove-vision-ai-v2 keeps its own plugin_run.c: it reads a container through a
 * memory-mapped flash window it holds a lease over, and its MPU is Armv8-M's.
 *
 * [!] THE CALLER STATES THE STAGING REGION; NOTHING HERE ASKS FOR IT.  The first
 * wio-lite-ai version re-queried the backend's load region from inside the
 * check, and the backend is DOUBLE-SLOTTED: it hands out the INACTIVE slot, so
 * once a reload adopted the staged model the query answered with the other one
 * and every container was refused on hardware.  The fact travels from the
 * caller that was handed it.
 *
 * No mutable storage: the state is the board's.
 */
#ifndef PLUGIN_RUN_CORE_H
#define PLUGIN_RUN_CORE_H

#include <stdint.h>

#include "plugin_abi.h"
#include "plugin_exec.h"
#include "plugin_load.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- the board-facing API -------------------------------------------------- */

/**
 * @brief  Copy, verify and start the plugin @p v describes.
 *
 * @param v          the validated view, from plugin_parse()
 * @param container  where the container lies: inside the staging region below
 * @param stage      the staging region the caller was handed by its backend,
 *                   and @p cap its size.  Passed in, not looked up -- above
 * @param cap        the size of that region
 * @param base       the vtable handed to the plugin; must outlive the plugin
 *
 * @return PLUGIN_RUN_OK, or a reason.  PLUGIN_RUN_NO_PLUGIN when the container
 *         carries only a model -- a legal container, and the caller carries on.
 *         PLUGIN_RUN_NOT_HELD when the caller does not hold the plugin lease.
 *
 * Logs its own refusals (plugin_run_log()): plugin_exec.c returns diagnostics
 * rather than printing them, precisely so that the board decides where they go.
 */
enum plugin_run_result plugin_run_load(const struct plugin_view *v,
                                       const void *container,
                                       const void *stage, uint32_t cap,
                                       const struct plugin_base_api *base);

/**
 * plugin_run_load()'s own refusal: the caller does not hold the plugin lease
 * (svc/plugin/plugin_lease_api.h, issue #130).  Never returned by the shared
 * loader, whose enum this cannot extend; outside its range on purpose, so it
 * cannot be mistaken for any of them -- and the same value as
 * grove-vision-ai-v2's.  Before the loader runs it means nothing was touched --
 * the previous plugin is still published; from the exec_ok hook it means
 * entry() was not called.  plugin_run_why() names it.
 */
#define PLUGIN_RUN_NOT_HELD ((enum plugin_run_result)0x40)

/** plugin_run_strerror(), plus PLUGIN_RUN_NOT_HELD. */
const char *plugin_run_why(enum plugin_run_result r);

/** Forget the active plugin.  Idempotent.  The caller must hold the plugin
 *  lease (issue #130); without it this does nothing and counts the refusal
 *  (plugin_lease_note_unheld()). */
void plugin_run_unload(void);

/** Is a plugin loaded and started? */
int plugin_run_active(void);

/** The address of one entry point of the active plugin, or NULL. */
void *plugin_run_slot(unsigned slot);

/**
 * @brief  Name the plugin an address belongs to, for the fault reporter.
 *
 * [!] SAFE TO CALL FROM AN EXCEPTION -- see plugin_exec.h.  The environment is
 * the board's file-scope `const` over a file-scope `static` state object, so
 * reaching it from a fault handler allocates nothing and initialises nothing.
 * Not inlined: wio-lite-ai's ITCM residency gate names it as the one call its
 * fault handler may make out of ITCM.
 */
const char *plugin_run_attribute(uint32_t pc, uint32_t *off);

/** Where the reservation is, for `nn info` and the board README's numbers. */
uint32_t plugin_run_res_base(void);
uint32_t plugin_run_res_len(void);

/* ---- what the board supplies -------------------------------------------------- */

/** The board's binding.  DEFINED BY THE BOARD (port/plugin/plugin_run.c): a
 *  `const` over its static state, its port and its reservation. */
extern const struct plugin_exec_env plugin_run_env;

/**
 * Is [lo, hi) executable, readable and writable right now?  DEFINED BY THE
 * BOARD: a consistent MPU snapshot, read under a critical section, judged by
 * svc/plugin/plugin_mpu_v7m.c.  0 when it is; non-zero with @p why set.
 */
int plugin_run_mpu_ok(uint32_t lo, uint32_t hi, const char **why);

/** One line of the loader's, for the board's log.  DEFINED BY THE BOARD.
 *  @p error non-zero for a refusal (the board's error level), zero for the
 *  "loaded" line.  @p text is terminated and lives for the call only. */
void plugin_run_log(int error, const char *text);

/**
 * Record entry()'s stack depth: @p sp was read in the exec_ok hook, in the
 * frame entry() is then called from.  DEFINED BY THE BOARD's nn layer, which
 * keeps the depth records (issue #126).
 */
void plugin_run_note_entry(uintptr_t sp);

/* ---- the port's hooks -------------------------------------------------------- */

/**
 * The exec_ok hook: entry()'s depth, then the lease asked again at the branch,
 * then the board's MPU verdict.  Called by plugin_exec.c from the frame entry()
 * is called from.  A caller without the lease gets -1 and the one reason only
 * this file returns, by which plugin_run_load() names the refusal NOT_HELD
 * rather than letting the loader's PLUGIN_RUN_MPU stand for it.
 */
int plugin_run_exec_ok(uint32_t lo, uint32_t hi, const char **why);

/**
 * The source_ok hook: the whole of what will be read lies inside the staging
 * region the caller was handed (@p token is plugin_run_load()'s own record of
 * it, on its stack).
 */
int plugin_run_source_ok(const void *container, uint32_t len, uintptr_t token,
                         const char **why);

/** The board's port: its cache maintenance, and the two hooks above. */
#define PLUGIN_RUN_PORT(sync_caches_fn)                                     \
	{                                                                   \
		.sync_caches = (sync_caches_fn),                            \
		.exec_ok     = plugin_run_exec_ok,                          \
		.source_ok   = plugin_run_source_ok,                        \
	}

#ifdef __cplusplus
}
#endif

#endif /* PLUGIN_RUN_CORE_H */
