/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    plugin_run.h
 * @brief   This board's binding of the shared plugin loader (issues #103, #110).
 *
 * The loader itself is svc/plugin_exec.c, shared with wio-lite-ai.  What is
 * genuinely this board's, and lives here, is:
 *
 *   - the state, in permanently allocated static memory;
 *   - the reservation, from this board's linker script;
 *   - cache maintenance for a Cortex-M55 with split I- and D-caches;
 *   - the Armv8-M MPU read-back and its verdict (port/plugin/plugin_mpu.c);
 *   - the precondition on the image's source -- here, the flash lease.
 *
 * [!] AND THE CALLER MUST ALREADY HOLD THE FLASH LEASE.  The container is read
 * from the XIP window, and the window can be taken down: port/nor/nor_flash.c's
 * enable_XIP() is the only thing that reconfigures it, a writer's reservation is
 * granted only when no reader lease is live (port/nor/nor_state.c), and
 * npu_hw_init() holds NOR_LEASE_NPU for as long as a model is open.  A plugin is
 * only ever loaded while a model is open, so the window is already pinned --
 * this file checks that rather than building a second mechanism, because the
 * one that exists is the one the rest of the port already obeys.
 *
 * The names below are unchanged from before the split so that nn_active.c,
 * fault.c and nn_svc_grove.c are unaffected by where the machine lives.
 */
#ifndef PLUGIN_RUN_H
#define PLUGIN_RUN_H

#include <stdint.h>

#include "plugin_abi.h"
#include "plugin_exec.h"
#include "plugin_load.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief  Copy, verify and start the plugin @p v describes.
 *
 * @param v          the validated view, from plugin_parse()
 * @param container  where the container lies (the XIP window address)
 * @param lease      the caller's live NOR lease token
 * @param base       the vtable handed to the plugin; must outlive the plugin
 *
 * @return PLUGIN_RUN_OK, or a reason.  PLUGIN_RUN_NO_PLUGIN when the container
 *         carries only a model -- a legal container, and the caller carries on.
 *
 * Logs its own refusals: svc/plugin_exec.c returns diagnostics rather than
 * printing them, precisely so that the board decides where they go.
 *
 * [!] AND IT RECORDS HOW DEEP THE STACK WAS WHERE entry() WAS CALLED (issue
 * #119), on a load that succeeded and on no other.  The call is inside the
 * shared loader, which may own no storage, so the sample is taken in this
 * board's exec_ok hook -- which the loader calls from the same frame, with the
 * stack pointer it will branch to entry() with -- and the hook's own small frame
 * makes it an upper bound, never an under-count (see plugin_run.c).
 */
enum plugin_run_result plugin_run_load(const struct plugin_view *v,
                                       const void *container, uint32_t lease,
                                       const struct plugin_base_api *base);

/** Forget the active plugin.  Idempotent. */
void plugin_run_unload(void);

/** Is a plugin loaded and started? */
int plugin_run_active(void);

/** The address of one entry point of the active plugin, or NULL. */
void *plugin_run_slot(unsigned slot);

/**
 * @brief  Name the plugin an address belongs to, for the fault reporter.
 *
 * [!] SAFE TO CALL FROM AN EXCEPTION -- see plugin_exec.h.  This board's
 * binding satisfies the accessor rule it states: the environment is a file
 * scope `static const` over a file scope `static` state object, so reaching it
 * from a fault handler allocates nothing and initialises nothing.
 */
const char *plugin_run_attribute(uint32_t pc, uint32_t *off);

#ifdef __cplusplus
}
#endif

#endif /* PLUGIN_RUN_H */
