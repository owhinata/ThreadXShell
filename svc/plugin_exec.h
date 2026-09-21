/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    plugin_exec.h
 * @brief   Copy a validated plugin image into a board's reservation and call
 *          its entry point (issues #103, #110).
 *
 * THREE LAYERS, AND THIS IS THE MIDDLE ONE.  svc/plugin_load.c validates a
 * container and hands back a POD snapshot of integer offsets -- it yields no
 * callable pointer, so "validation does not execute anything" is a property of
 * its types.  This file is where those offsets become an address and where the
 * branch happens.  A board's own port/plugin/plugin_run.c binds it: it owns the
 * state, the reservation, and the three things that differ per board.
 *
 * WHY IT IS SHARED (issue #110).  grove-vision-ai-v2 had all of this to itself
 * until wio-lite-ai needed the same machine, and f746g-disco is the known third
 * consumer.  The invariants below are written once in AGENTS.md against ONE
 * mechanism; a second copy of the mechanism would be a second copy of them, and
 * the one that drifted would be the one nobody re-read.  Only three things
 * genuinely differ between boards -- cache maintenance, whether the reservation
 * is executable right now, and whether the bytes being read are still there --
 * and those are @ref plugin_exec_port.
 *
 * [!] ENTRY IS CALLED LAST, AFTER EVERYTHING ELSE HAS PASSED.  An earlier design
 * had the plugin RETURN its descriptor from entry(), which meant running
 * unverified code to find out whether the code was worth running.  The manifest
 * is data, and by the time control reaches the image the loader has established
 * that the bytes match their digest, that they fit the reservation, that the
 * reservation is executable, and that the caches agree about what is in it.
 *
 * [!] THIS FILE OWNS NO MUTABLE STORAGE.  The state is the board's, in
 * permanently allocated static memory, and reaches here through
 * @ref plugin_exec_env.  cmake/check_no_mutable_storage.py audits it per board.
 *
 * [!] A GATED PLUGIN IS NOT "SAFE TO EXECUTE".  Nothing here proves memory
 * safety, nor the range in which the plugin uses the pointers it is handed.  A
 * plugin is reviewed, trusted native code with the same standing as board code.
 * See AGENTS.md; do not read any of this as an isolation boundary.
 */
#ifndef PLUGIN_EXEC_H
#define PLUGIN_EXEC_H

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
	PLUGIN_RUN_NO_SOURCE,   /**< the bytes are not pinned by the caller    */
	PLUGIN_RUN_TOO_BIG,     /**< mem_size exceeds the reservation          */
	PLUGIN_RUN_MPU,         /**< the reservation is not executable         */
	PLUGIN_RUN_ENTRY,       /**< the plugin's own entry point refused      */
};

/**
 * What the fault reporter is allowed to see.
 *
 * [!] IMMUTABLE ONCE PUBLISHED, AND THE NAME IS A COPY.  A fault can arrive at
 * any instant, including while a container is being replaced, so the reporter
 * must never follow a pointer into the plugin or into anything the loader is
 * still writing.  The loader fills one of these completely, then publishes a
 * pointer to it with a single store; unpublishing is another single store, and
 * it happens BEFORE the slot is touched again.
 */
struct plugin_active {
	uint32_t base;                       /**< image base address           */
	uint32_t len;                        /**< mem_size                     */
	char     name[PLUGIN_NAME_MAX];
	char     build_id[PLUGIN_BUILD_ID_MAX];
};

/**
 * The loader's state.  The BOARD defines exactly one of these, statically and
 * permanently, and never moves or reallocates it.
 */
struct plugin_exec_state {
	struct plugin_active           slot;
	struct plugin_active *volatile active;   /**< published pointer        */
	struct plugin_view             view;
	int                            started;
};

/**
 * The three things that differ per board.  All are optional except
 * @ref sync_caches on a board with caches; a NULL hook means "nothing to do
 * here", which is a claim the board is making and should say why.
 */
struct plugin_exec_port {
	/**
	 * Make the CPU agree with itself about bytes just written and about to be
	 * executed.  Called with the WHOLE reservation, after the copy and before
	 * anything else looks at it.
	 */
	void (*sync_caches)(uint32_t base, uint32_t len);

	/**
	 * Is [lo, hi) privileged-readable, privileged-writable and executable
	 * right now, as ordinary memory?
	 *
	 * Asked after the caches and before the branch, because a board may have
	 * code that reconfigures its MPU behind the loader's back.  A board where
	 * nothing does still answers by reading the hardware: "nothing reconfigures
	 * it" is a claim about today's source, and this is the check that survives
	 * tomorrow's.
	 *
	 * @return 0 when it is; non-zero with @p why set to a short reason.
	 */
	int (*exec_ok)(uint32_t lo, uint32_t hi, const char **why);

	/**
	 * Is the memory the image is about to be read FROM still guaranteed to be
	 * there, and does the caller own that guarantee?
	 *
	 * Grove reads through a memory-mapped flash window that another thread may
	 * take down, and holds a lease over it; wio reads a staging buffer whose
	 * owner is the load path itself.  The shapes differ, the obligation does
	 * not -- so this is a hook rather than an omission, because a board with no
	 * hook is indistinguishable from a board that forgot.
	 *
	 * @param container  the address the image will be read from
	 * @param token      whatever the board's caller passes as proof
	 * @return 0 when it is; non-zero with @p why set to a short reason.
	 */
	int (*source_ok)(const void *container, uintptr_t token, const char **why);
};

/**
 * A board's binding: its state, its hooks, and its reservation.
 *
 * [!] THE RESERVATION IS PASSED IN, NOT LOOKED UP.  This file could have
 * referenced the linker symbols directly -- both boards happen to spell them
 * the same -- and then a shared translation unit would silently depend on a
 * board's linker script.  It is the board's fact, so the board states it.
 */
struct plugin_exec_env {
	struct plugin_exec_state      *state;
	const struct plugin_exec_port *port;
	/*
	 * The reservation, as two byte pointers rather than an address and a
	 * length: a board's linker script gives it two symbols, and `end - start`
	 * is not something a static initialiser can fold.  Pointers to extern
	 * arrays are link-time constants, so a board states its reservation in an
	 * initialiser and the environment stays in .rodata -- which is what makes
	 * it safe to reach from a fault handler.
	 */
	uint8_t                       *res_lo;
	uint8_t                       *res_hi;
};

/**
 * @brief  Copy, verify and start the plugin @p v describes.
 *
 * @param env        the board's binding; must outlive the plugin
 * @param v          the validated view, from plugin_parse()
 * @param container  where the container lies
 * @param token      the board's proof for @ref plugin_exec_port::source_ok
 * @param base       the vtable handed to the plugin; must outlive the plugin
 * @param why        optional; on a refusal the port's reason, else untouched
 *
 * @return PLUGIN_RUN_OK, or a reason.  PLUGIN_RUN_NO_PLUGIN when the container
 *         carries only a model -- a legal container, and the caller carries on.
 *
 * [!] DIAGNOSTICS ARE RETURNED, NOT LOGGED.  A shared translation unit that
 * included a board's log.h would have quietly made the board's logger part of
 * this contract; the caller is on the board and can say it better anyway.
 *
 * On any failure nothing is left published: a plugin that refused its own entry
 * is not active, and the fault reporter will not name it.
 */
enum plugin_run_result plugin_exec_load(const struct plugin_exec_env *env,
                                        const struct plugin_view *v,
                                        const void *container, uintptr_t token,
                                        const struct plugin_base_api *base,
                                        const char **why);

/**
 * @brief  Forget the active plugin.
 *
 * [!] THE ACTIVE POINTER IS CLEARED BEFORE ANYTHING IS REUSED.  The fault
 * reporter reads it from an exception, so a slot that is being refilled while
 * it is still published would be read half-written by a fault that arrived at
 * the wrong moment.  Idempotent: unloading nothing is not an error.
 */
void plugin_exec_unload(const struct plugin_exec_env *env);

/** Is a plugin loaded and started? */
int plugin_exec_active(const struct plugin_exec_env *env);

/**
 * @brief  The address of one entry point of the active plugin, or NULL.
 *
 * [!] THE ONE PLACE AN OFFSET BECOMES SOMETHING CALLABLE.  svc/plugin_load.c
 * deliberately hands back integers and no function pointers, so that "the
 * validation step does not execute anything" is a property of its types rather
 * than a discipline someone keeps.  Here is where the arithmetic happens, and
 * it happens for the loader's own entry call as well -- an earlier shape had
 * the loader compute the entry address and the decoder shim compute the other
 * six, which is two places claiming to be one.
 *
 * @return NULL for an absent slot, an out-of-range index, or any time no plugin
 *         has completed its entry point.  A caller casts the result to the
 *         prototype enum plugin_slot names for that slot; the Thumb bit is kept,
 *         because the manifest carried it and plugin_load.c insisted on it.
 */
void *plugin_exec_slot(const struct plugin_exec_env *env, unsigned slot);

/**
 * @brief  Name the plugin an address belongs to, for the fault reporter.
 *
 * @param pc   a program counter
 * @param off  receives the offset from the image base, when it matches
 * @return the plugin's name, or NULL when @p pc is not inside the active image
 *
 * [!] SAFE TO CALL FROM AN EXCEPTION.  It loads one atomically published
 * pointer and then reads only immutable, loader-owned memory; it never
 * dereferences anything inside the plugin.  The claim it supports is "the pc is
 * inside the active plugin", which is all an imprecise fault can honestly say.
 * The board's accessor for @p env must be exception-safe from cold boot too:
 * no lazy initialisation, no allocation, no lock, and nothing that can be
 * swapped for another object.
 */
const char *plugin_exec_attribute(const struct plugin_exec_env *env,
                                  uint32_t pc, uint32_t *off);

/** Short description of a result (never NULL). */
const char *plugin_run_strerror(enum plugin_run_result r);

#ifdef __cplusplus
}
#endif

#endif /* PLUGIN_EXEC_H */
