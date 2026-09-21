/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    plugin_run.h
 * @brief   This board's binding of the shared plugin loader (issue #110 =
 *          #78 Step 3b).
 *
 * The loader itself is svc/plugin_exec.c, shared with grove-vision-ai-v2.  What
 * is genuinely this board's, and lives here, is:
 *
 *   - the state, in permanently allocated static memory;
 *   - the reservation, from this board's linker script -- the top 32 KB of
 *     AXI-SRAM, which is a stated exception to "AXI-SRAM is for bus masters"
 *     because a Cortex-M7 cannot fetch instructions from DTCM;
 *   - cache maintenance: the M7's I- and D-caches are separate and an
 *     instruction fetch does not snoop the D-cache;
 *   - the Armv7-M MPU read-back and its verdict (port/plugin/plugin_mpu_v7m.c),
 *     which is NOT the other board's judgement -- see that file;
 *   - the precondition on the image's source.
 *
 * [!] AND THERE IS NO FLASH LEASE HERE.  Grove reads a container out of a
 * memory-mapped flash window that another thread can take down, so its loader
 * checks a lease.  This board has no such window on purpose (port/nor drives
 * the device through indirect transactions only), and the container is a copy
 * in the backend's staging buffer.  That is a weaker obligation, not an absent
 * one, so it is still a check: the bytes must lie inside the region the backend
 * handed out, because a board with no hook is indistinguishable from a board
 * that forgot to write one.
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
 * @param container  where the container lies: inside the backend's staging
 *                   buffer, which the caller obtained from
 *                   nn_model_load_region() while holding the NN session
 * @param base       the vtable handed to the plugin; must outlive the plugin
 *
 * @return PLUGIN_RUN_OK, or a reason.  PLUGIN_RUN_NO_PLUGIN when the container
 *         carries only a model -- a legal container, and the caller carries on.
 *
 * Logs its own refusals: svc/plugin_exec.c returns diagnostics rather than
 * printing them, precisely so that the board decides where they go.
 */
enum plugin_run_result plugin_run_load(const struct plugin_view *v,
                                       const void *container,
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

/** Where the reservation is, for `nn info` and the board README's numbers. */
uint32_t plugin_run_res_base(void);
uint32_t plugin_run_res_len(void);

#ifdef __cplusplus
}
#endif

#endif /* PLUGIN_RUN_H */
