/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    plugin_run.h
 * @brief   Load a validated plugin image and call its entry point (issue #103).
 *
 * Step 1a (issue #101) delivered a container and validated its manifest without
 * ever branching into it.  This is the step that branches.
 *
 * [!] ENTRY IS CALLED LAST, AFTER EVERYTHING ELSE HAS PASSED.  An earlier
 * design had the plugin RETURN its descriptor from entry(), which meant running
 * unverified code to find out whether the code was worth running.  The manifest
 * is data now, and by the time control reaches the image the loader has already
 * established that the bytes match their digest, that they fit the reservation,
 * that the reservation is executable, and that the caches agree about what is
 * in it.
 *
 * [!] AND THE CALLER MUST ALREADY HOLD THE FLASH LEASE.  The container is read
 * from the XIP window, and the window can be taken down: port/nor/nor_flash.c's
 * enable_XIP() is the only thing that reconfigures it, a writer's reservation is
 * granted only when no reader lease is live (port/nor/nor_state.c), and
 * npu_hw_init() holds NOR_LEASE_NPU for as long as a model is open.  A plugin is
 * only ever loaded while a model is open, so the window is already pinned --
 * this file checks that rather than building a second mechanism, because the
 * one that exists is the one the rest of the port already obeys.
 */
#ifndef PLUGIN_RUN_H
#define PLUGIN_RUN_H

#include <stdint.h>

#include "plugin_abi.h"
#include "plugin_load.h"

#ifdef __cplusplus
extern "C" {
#endif

enum plugin_run_result {
	PLUGIN_RUN_OK = 0,
	PLUGIN_RUN_ARG,
	PLUGIN_RUN_NO_PLUGIN,   /**< the container carries none; not an error  */
	PLUGIN_RUN_NO_LEASE,    /**< the XIP window is not pinned by us        */
	PLUGIN_RUN_TOO_BIG,     /**< mem_size exceeds the reservation          */
	PLUGIN_RUN_MPU,         /**< the reservation is not executable         */
	PLUGIN_RUN_ENTRY,       /**< the plugin's own entry point refused      */
};

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
 * On any failure nothing is left published: a plugin that refused its own entry
 * is not active, and the fault reporter will not name it.
 */
enum plugin_run_result plugin_run_load(const struct plugin_view *v,
                                       const void *container, uint32_t lease,
                                       const struct plugin_base_api *base);

/**
 * @brief  Forget the active plugin.
 *
 * [!] THE ACTIVE POINTER IS CLEARED BEFORE ANYTHING IS REUSED.  The fault
 * reporter reads it from an exception, so a slot that is being refilled while
 * it is still published would be read half-written by a fault that arrived at
 * the wrong moment.  Idempotent: unloading nothing is not an error.
 */
void plugin_run_unload(void);

/** Is a plugin loaded and started? */
int plugin_run_active(void);

/**
 * @brief  The address of one entry point of the active plugin, or NULL.
 *
 * [!] THE ONE PLACE AN OFFSET BECOMES SOMETHING CALLABLE.  svc/plugin_load.c
 * deliberately hands back integers and no function pointers, so that "the
 * validation step does not execute anything" is a property of its types rather
 * than a discipline someone keeps.  Here is where the arithmetic happens, and
 * it happens for the loader's own entry call as well -- the earlier shape had
 * this file compute the entry address and the decoder shim compute the other
 * six, which is two places claiming to be one.
 *
 * @return NULL for an absent slot, an out-of-range index, or any time no plugin
 *         has completed its entry point.  A caller casts the result to the
 *         prototype enum plugin_slot names for that slot; the Thumb bit is kept,
 *         because the manifest carried it and plugin_load.c insisted on it.
 */
void *plugin_run_slot(unsigned slot);

/**
 * @brief  Name the plugin an address belongs to, for the fault reporter.
 *
 * @param pc   a program counter
 * @param off  receives the offset from the image base, when it matches
 * @return the plugin's name, or NULL when @p pc is not inside the active image
 *
 * [!] SAFE TO CALL FROM AN EXCEPTION.  It reads one atomically published
 * pointer and then only immutable, loader-owned memory; it never dereferences
 * anything inside the plugin.  The claim it supports is "the pc is inside the
 * active plugin", which is all an imprecise fault can honestly say.
 */
const char *plugin_run_attribute(uint32_t pc, uint32_t *off);

/** Short description of a result (never NULL). */
const char *plugin_run_strerror(enum plugin_run_result r);

#ifdef __cplusplus
}
#endif

#endif /* PLUGIN_RUN_H */
