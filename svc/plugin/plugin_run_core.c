/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    plugin_run_core.c
 * @brief   The loader's lease checks, its log lines and the staging-region
 *          check.  See plugin_run_core.h.
 *
 * Moved out of wio-lite-ai's port/plugin/plugin_run.c (issue #159); what stays
 * there -- and in f746g-disco's -- is the MPU read-back and the cache
 * maintenance.
 *
 * No mutable storage.
 */
#include "plugin_run_core.h"

#include "fmt.h"
#include "nn_active_core.h"     /* NN_ACTIVE_CORE_SP() */
#include "plugin_lease_api.h"   /* the one lock into the plugin (#130) */

#include <stdarg.h>
#include <stddef.h>

/*
 * The exec_ok hook's lease refusal, by identity.  plugin_exec.c hands the
 * hook's reason back unchanged, so plugin_run_load() recognises this very
 * pointer and names the refusal NOT_HELD -- the board used to keep a flag for
 * it, which is mutable storage this file may not own and a second place for
 * "why" to live.
 */
static const char pl_unheld_why[] = "the plugin lease is not held";

/*
 * One loader line to the board's log.  Not inlined: the line buffer belongs
 * to the report AFTER the load, never to the frame entry() is called beneath.
 */
static __attribute__((noinline)) void pl_log(int error, const char *f, ...)
{
	char line[112];
	va_list ap;

	va_start(ap, f);
	(void)fmt_vsnformat(line, sizeof line, f, ap);
	va_end(ap);
	plugin_run_log(error, line);
}

/*
 * The exec_ok hook, and where entry()'s stack depth is taken (issue #126).
 *
 * [!] THE LOADER CALLS THIS FROM THE FRAME IT CALLS entry() FROM.  entry() is
 * called inside plugin_exec.c, which may own no storage, so no probe can go
 * beside that branch -- but this hook is called in the same function, after
 * the copy and the cache maintenance and before the branch, with the stack
 * pointer the branch will use.  Read here, the depth is the entry depth plus
 * THIS function's frame: an upper bound, never an under-count.
 *
 * Recorded even when the verdict below then stops the load: the boards keep a
 * high-water, not a count, and a load stopped here was at exactly the depth an
 * entry() would have been.
 */
int plugin_run_exec_ok(uint32_t lo, uint32_t hi, const char **why)
{
	plugin_run_note_entry(NN_ACTIVE_CORE_SP());
	/* [!] THE LAST CHECK BEFORE entry() (issue #130).  plugin_run_load()
	 * refuses an unheld caller before anything is touched; this is the same
	 * question asked again at the branch, so a path into the loader that
	 * skipped the first cannot reach entry() either. */
	if (!plugin_lease_held()) {
		plugin_lease_note_unheld();
		if (why != NULL)
			*why = pl_unheld_why;
		return -1;
	}
	return plugin_run_mpu_ok(lo, hi, why);
}

/*
 * The staging region the CALLER was handed, travelling as the port token.
 *
 * [!] A STACK OBJECT OF plugin_run_load()'s, not a file-scope one.  It is valid
 * for exactly the call that passes it, which is the whole lifetime the check
 * needs -- and a static would be a second place for "which region" to live.
 */
struct pl_source {
	const uint8_t *lo;
	uint32_t       cap;
};

/*
 * The image is read out of the backend's staging buffer, and the whole of what
 * will be read has to be inside it.
 *
 * [!] A WEAKER OBLIGATION THAN GROVE'S, AND STILL A CHECK.  There is no window
 * here to be taken down -- the container is a copy in RAM -- so what can go
 * wrong is not a lifetime but a provenance: a caller handing over a pointer
 * into some other buffer.  It deliberately does NOT claim the caller's session
 * is still held: that would answer "somebody holds it", a fact about another
 * thread's lifetime and not about this caller's right to these bytes.
 */
int plugin_run_source_ok(const void *container, uint32_t len, uintptr_t token,
                         const char **why)
{
	const struct pl_source *src = (const struct pl_source *)token;
	const uint8_t *c = (const uint8_t *)container;

	if (src == NULL || src->lo == NULL || src->cap == 0u) {
		if (why != NULL)
			*why = "the caller named no staging region";
		return -1;
	}
	if (c < src->lo || len > src->cap || c + len > src->lo + src->cap) {
		if (why != NULL)
			*why = "the image is not inside the staging region the caller "
			       "was handed";
		return -1;
	}
	return 0;
}

/* The load's outcome, to the board's log -- after the load, see pl_log(). */
static __attribute__((noinline)) void pl_report(enum plugin_run_result r,
                                                const struct plugin_view *v,
                                                const char *why)
{
	switch (r) {
	case PLUGIN_RUN_OK:
		pl_log(0, "'%s' (build %s) loaded: %lu B at 0x%08lx",
		       plugin_run_env.state->slot.name,
		       plugin_run_env.state->slot.build_id,
		       (unsigned long)v->mem_size,
		       (unsigned long)plugin_run_res_base());
		break;
	case PLUGIN_RUN_NO_PLUGIN:
		break;                      /* legal; the caller carries on */
	case PLUGIN_RUN_TOO_BIG:
		pl_log(1, "image wants %lu B at 0x%08lx, reservation is %lu B at "
		       "0x%08lx", (unsigned long)v->mem_size,
		       (unsigned long)v->link_addr,
		       (unsigned long)plugin_run_res_len(),
		       (unsigned long)plugin_run_res_base());
		break;
	case PLUGIN_RUN_ENTRY:
		/* [!] %s, NOT %.*s (issue #112): svc/fmt.c implements neither a
		 * precision nor `*`.  plugin_parse() terminates this field
		 * (plugin_load.h), which is what makes %s the right answer. */
		pl_log(1, "'%s' refused its own entry point or declares none",
		       v->name);
		break;
	default:
		pl_log(1, "%s%s%s", plugin_run_strerror(r), why != NULL ? ": " : "",
		       why != NULL ? why : "");
		break;
	}
}

enum plugin_run_result plugin_run_load(const struct plugin_view *v,
                                       const void *container,
                                       const void *stage, uint32_t cap,
                                       const struct plugin_base_api *base)
{
	struct pl_source src = { (const uint8_t *)stage, cap };
	const char *why = NULL;
	enum plugin_run_result r;

	/*
	 * [!] THE LEASE FIRST, BEFORE THE SHARED LOADER TOUCHES ANYTHING (issue
	 * #130, as grove-vision-ai-v2's since #127).  plugin_exec_load() begins by
	 * unpublishing the previous plugin and then copies the new image over the
	 * reservation it was running from; a caller without the lease could be
	 * doing that under a decode or a draw.  Refused here, the previous plugin
	 * is exactly as it was.
	 */
	if (!plugin_lease_held()) {
		plugin_lease_note_unheld();
		pl_log(1, "load refused: %s", plugin_run_why(PLUGIN_RUN_NOT_HELD));
		return PLUGIN_RUN_NOT_HELD;
	}
	r = plugin_exec_load(&plugin_run_env, v, container, (uintptr_t)&src, base,
	                     &why);
	if (r == PLUGIN_RUN_MPU && why == pl_unheld_why) {
		/* the hook's lease refusal, by name -- not the MPU's */
		pl_log(1, "load refused at entry: %s",
		       plugin_run_why(PLUGIN_RUN_NOT_HELD));
		return PLUGIN_RUN_NOT_HELD;
	}
	pl_report(r, v, why);
	return r;
}

const char *plugin_run_why(enum plugin_run_result r)
{
	if (r == PLUGIN_RUN_NOT_HELD)
		return "the caller does not hold the plugin lease";
	return plugin_run_strerror(r);
}

void plugin_run_unload(void)
{
	/* [!] UNDER THE PLUGIN LEASE, LIKE A LOAD (issue #130; the one entry
	 * Grove's #127 first left unchecked).  Unpublishing clears the slot table
	 * a holder may be calling through.  Refused, the published plugin is left
	 * exactly as it was, and the refusal is counted like every other entry
	 * check's. */
	if (!plugin_lease_held()) {
		plugin_lease_note_unheld();
		pl_log(1, "unload refused: %s", plugin_run_why(PLUGIN_RUN_NOT_HELD));
		return;
	}
	plugin_exec_unload(&plugin_run_env);
}

int plugin_run_active(void)
{
	return plugin_exec_active(&plugin_run_env);
}

void *plugin_run_slot(unsigned slot)
{
	return plugin_exec_slot(&plugin_run_env, slot);
}

/*
 * [!] noinline, AND THAT IS A PLACEMENT DECISION.  Its one caller is the fault
 * handler, which on wio-lite-ai lives in ITCM for interrupt latency, and
 * cmake/check_itcm_residency.py refuses any reference out of ITCM that is not
 * named with a reason.  Out of line, the handler makes exactly one call to a
 * symbol that means something.
 */
__attribute__((noinline))
const char *plugin_run_attribute(uint32_t pc, uint32_t *off)
{
	return plugin_exec_attribute(&plugin_run_env, pc, off);
}

uint32_t plugin_run_res_base(void)
{
	return (uint32_t)(uintptr_t)plugin_run_env.res_lo;
}

uint32_t plugin_run_res_len(void)
{
	return (uint32_t)(plugin_run_env.res_hi - plugin_run_env.res_lo);
}
