/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    plugin_exec.c
 * @brief   The loader.  See plugin_exec.h.
 */
#include "plugin_exec.h"

#include <stddef.h>
#include <string.h>

/*
 * The publication barrier.
 *
 * [!] A COMPILER BUILTIN, NOT CMSIS.  __DMB() lives behind a board's device
 * header -- WE2_device.h on one board, stm32h7xx.h on another -- and a shared
 * translation unit that included either would stop being shared.  The builtin
 * emits the same data barrier on both cores and compiles on the host, where the
 * ordering tests run.
 */
#define PLUGIN_EXEC_PUBLISH_BARRIER() __atomic_thread_fence(__ATOMIC_SEQ_CST)

/*
 * Turn a slot offset into something callable.  See plugin_exec.h.
 *
 * Split out so that the entry call below and plugin_exec_slot() cannot
 * disagree: they are the same arithmetic, over the same view, with the same
 * absent-slot rule.  Static, because the only thing outside this file that may
 * form one of these addresses is a caller of plugin_exec_slot(), which is gated
 * on the plugin having completed its entry point -- and the entry call
 * obviously cannot be.
 */
static void *plugin_slot_addr(const struct plugin_view *v, uint8_t *base,
                              unsigned slot)
{
	if (v == NULL || base == NULL || slot >= (unsigned)PLUGIN_SLOT_COUNT)
		return NULL;
	if (v->slot[slot] == PLUGIN_SLOT_ABSENT)
		return NULL;
	/* [!] FROM THE POINTER, NOT FROM THE 32-BIT BASE.  Both are the same
	 * address on either target, and the reservation's address is what the
	 * manifest was prelinked against -- but forming a CALLABLE address by
	 * casting a truncated integer back to a pointer is only correct because
	 * the target happens to be 32-bit.  The host tests for this file run in a
	 * 64-bit process, and a rule that holds by coincidence is one they could
	 * not check. */
	return (void *)(base + v->slot[slot]);
}

/* A binding is only usable when the board filled it in completely. */
static int env_ok(const struct plugin_exec_env *env)
{
	return env != NULL && env->state != NULL && env->port != NULL &&
	       env->res_lo != NULL && env->res_hi > env->res_lo;
}

/* ---- load / unload ------------------------------------------------------- */

enum plugin_run_result plugin_exec_load(const struct plugin_exec_env *env,
                                        const struct plugin_view *v,
                                        const void *container, uintptr_t token,
                                        const struct plugin_base_api *base,
                                        const char **why)
{
	struct plugin_exec_state *st;
	const char *reason = NULL;
	plugin_entry_fn entry;
	uint32_t res_base, res_len;

	if (!env_ok(env) || v == NULL || container == NULL || base == NULL)
		return PLUGIN_RUN_ARG;
	st       = env->state;
	res_base = (uint32_t)(uintptr_t)env->res_lo;
	res_len  = (uint32_t)(env->res_hi - env->res_lo);

	/*
	 * Whatever was there is gone from this point on.
	 *
	 * [!] BEFORE THE has_plugin TEST, NOT AFTER IT.  A container that carries
	 * only a model is a legal container and NO_PLUGIN is not an error -- but it
	 * is still a request to load something else, and returning it early left the
	 * PREVIOUS plugin published and callable.  A caller reading NO_PLUGIN as
	 * success would then decode a new model with an old model's decoder.  The
	 * argument check above stays in front: a null pointer is not a request.
	 */
	plugin_exec_unload(env);

	if (!v->has_plugin)
		return PLUGIN_RUN_NO_PLUGIN;

	/* The memory the image is read from must still be guaranteed by the
	 * caller.  See plugin_exec.h for why this is a check and not a new
	 * mechanism. */
	if (env->port->source_ok != NULL &&
	    env->port->source_ok(container, token, &reason) != 0) {
		if (why != NULL)
			*why = reason;
		return PLUGIN_RUN_NO_SOURCE;
	}

	if (v->mem_size > res_len || v->link_addr != res_base)
		return PLUGIN_RUN_TOO_BIG;

	/* Copy, then zero what has no initialiser.  bss and scratch are described
	 * separately by the manifest and both are memory-only, so neither is in the
	 * bytes that arrived. */
	memcpy(env->res_lo, (const uint8_t *)container + v->image_off,
	       v->file_size);
	memset(env->res_lo + v->file_size, 0, v->mem_size - v->file_size);

	/* [!] THE WHOLE RESERVATION, NOT JUST THE IMAGE.  Maintenance is by address
	 * and rounds outward to whole cache lines; maintaining only the image would
	 * let that rounding reach whatever follows it.  Both ends of a reservation
	 * are line-aligned by its linker script. */
	if (env->port->sync_caches != NULL)
		env->port->sync_caches(res_base, res_len);

	/* [!] AFTER the caches, BEFORE the branch.  A board may have code that
	 * reconfigures its MPU behind the loader's back, so the reservation cannot
	 * be assumed still executable just because it was at boot. */
	if (env->port->exec_ok != NULL &&
	    env->port->exec_ok(res_base, res_base + res_len, &reason) != 0) {
		if (why != NULL)
			*why = reason;
		return PLUGIN_RUN_MPU;
	}

	/* Publish before the branch: a fault inside entry() should name the plugin
	 * that caused it. */
	st->view = *v;
	st->slot.base = res_base;
	st->slot.len  = v->mem_size;
	memcpy(st->slot.name, v->name, sizeof st->slot.name);
	memcpy(st->slot.build_id, v->build_id, sizeof st->slot.build_id);
	PLUGIN_EXEC_PUBLISH_BARRIER();
	st->active = &st->slot;          /* single aligned store; see the header */

	entry = (plugin_entry_fn)plugin_slot_addr(&st->view, env->res_lo,
	                                          PLUGIN_SLOT_ENTRY);
	if (entry == NULL) {
		/* plugin_parse() refuses a manifest whose ENTRY is absent, so this is
		 * unreachable through the one caller -- and a branch to 0 is not the
		 * way to find out it stopped being. */
		plugin_exec_unload(env);
		return PLUGIN_RUN_ENTRY;
	}

	if (entry(base) != 0) {
		plugin_exec_unload(env);
		return PLUGIN_RUN_ENTRY;
	}

	st->started = 1;
	return PLUGIN_RUN_OK;
}

void plugin_exec_unload(const struct plugin_exec_env *env)
{
	struct plugin_exec_state *st;

	if (!env_ok(env))
		return;
	st = env->state;

	/* Unpublish FIRST.  Everything below rewrites what the fault reporter
	 * would have been reading. */
	st->active = NULL;
	PLUGIN_EXEC_PUBLISH_BARRIER();

	st->started = 0;
	memset(&st->view, 0, sizeof st->view);
	memset(&st->slot, 0, sizeof st->slot);
}

int plugin_exec_active(const struct plugin_exec_env *env)
{
	return env_ok(env) ? env->state->started : 0;
}

void *plugin_exec_slot(const struct plugin_exec_env *env, unsigned slot)
{
	if (!env_ok(env) || !env->state->started)
		return NULL;
	return plugin_slot_addr(&env->state->view, env->res_lo, slot);
}

const char *plugin_exec_attribute(const struct plugin_exec_env *env,
                                  uint32_t pc, uint32_t *off)
{
	const struct plugin_active *a;

	if (!env_ok(env))
		return NULL;
	a = env->state->active;          /* one load, then immutable */
	if (a == NULL)
		return NULL;
	if (pc < a->base || pc - a->base >= a->len)
		return NULL;
	if (off != NULL)
		*off = pc - a->base;
	return a->name;
}

const char *plugin_run_strerror(enum plugin_run_result r)
{
	switch (r) {
	case PLUGIN_RUN_OK:        return "ok";
	case PLUGIN_RUN_ARG:       return "bad argument";
	case PLUGIN_RUN_NO_PLUGIN: return "the container carries no plugin";
	case PLUGIN_RUN_NO_SOURCE: return "the image's source is not pinned";
	case PLUGIN_RUN_TOO_BIG:   return "it does not fit the reservation";
	case PLUGIN_RUN_MPU:       return "the reservation is not executable";
	case PLUGIN_RUN_ENTRY:     return "the plugin refused its own entry point";
	}
	return "unknown";
}
