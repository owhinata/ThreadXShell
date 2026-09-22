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
 * @param sp_at_load where the stack pointer was when this called the shared
 *                   loader, or NULL -- see PLUGIN_RUN_ENTRY_FRAME
 *
 * @return PLUGIN_RUN_OK, or a reason.  PLUGIN_RUN_NO_PLUGIN when the container
 *         carries only a model -- a legal container, and the caller carries on.
 *
 * Logs its own refusals: svc/plugin_exec.c returns diagnostics rather than
 * printing them, precisely so that the board decides where they go.
 */
enum plugin_run_result plugin_run_load(const struct plugin_view *v,
                                       const void *container, uint32_t lease,
                                       const struct plugin_base_api *base,
                                       uintptr_t *sp_at_load);

/**
 * How much further down the stack entry() is entered than *sp_at_load (issue
 * #119): the shared loader's own frame at its call through the entry slot.
 *
 * [!] A NUMBER READ OFF THE FINAL ELF, NOT A PROPERTY THE CODE CAN STATE.  The
 * loader is svc/plugin_exec.c, which may own no storage, so the probe cannot go
 * inside it; it goes in front of it, and this is what lies between.  In the
 * image it was derived from, plugin_exec_load() opens with
 * `stmdb sp!, {r4-r11, lr}` (36 B) and `sub sp, #20` and does not move sp again
 * before `blx` to the entry: 56 B.  Its fifth and sixth arguments are in the
 * CALLER's frame and so are already in the sample.  Anything that changes that
 * function or the flags it is built with changes this number -- re-derive it
 * (`objdump -d shell.elf`, <plugin_exec_load>) rather than trust it.
 */
#define PLUGIN_RUN_ENTRY_FRAME 56

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
