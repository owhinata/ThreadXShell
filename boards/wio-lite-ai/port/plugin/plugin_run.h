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
 *
 * [!] AND THE CALLER STATES THAT REGION; THIS FILE DOES NOT ASK FOR IT.  The
 * first version re-queried nn_model_load_region() from inside the check, and
 * the backend is DOUBLE-SLOTTED: load_region() hands out the INACTIVE slot, so
 * the moment nn_model_reload() adopted the staged model that slot became the
 * active one and the query started answering with the other.  The check then
 * refused every container on hardware -- fail-closed, and for a reason that had
 * nothing to do with the container.  Asking a question the operation in between
 * has already changed the answer to is the same mistake nn_model_open() taught
 * this file's neighbour (issue #108's review); the fact travels from the caller
 * that was handed it.
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
 * @param container  where the container lies: inside the staging region below
 * @param stage      the staging region the caller was handed by
 *                   nn_model_load_region(), and @p cap its size.  Passed in,
 *                   not looked up -- see above
 * @param cap        the size of that region
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
                                       const void *stage, uint32_t cap,
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
