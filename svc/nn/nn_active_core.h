/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    nn_active_core.h
 * @brief   The one place that decides whether a plugin decodes, shared by
 *          grove-vision-ai-v2 and wio-lite-ai (issue #126).
 *
 * Each board's port nn_active.c keeps its public nn_active_*() API and turns
 * its own tensors into svc/tensor.h descriptors; the decision -- is there a
 * plugin, which slot, what the answer is when there is none -- and every
 * indirect call into a plugin's decoder slots is made here, once.
 *
 * WHAT A BOARD PASSES (struct nn_active_board), and nothing else:
 *   - whether a plugin is live and where its slots are (the loader is the
 *     board's: plugin_run_active() / plugin_run_slot());
 *   - where to record the stack depth a slot is entered at;
 *   - its answer to "can these shapes be read?" when NO plugin is loaded.
 *     The two boards give different answers on purpose and are not to be made
 *     to agree: grove-vision-ai-v2 refuses (its stream admission asks this and
 *     nothing else), wio-lite-ai accepts (its `nn run` on a bare model shares
 *     the question, and a stream is refused one question lower, by can_draw).
 *
 * [!] THIS FILE DOES NOT KNOW THAT A RESULT GATE EXISTS.  It takes no lock and
 * its contract does not say "the caller holds X".  Who may call a plugin and
 * when -- wio-lite-ai's result lease, grove-vision-ai-v2's one guard and the
 * pipeline pin -- is each board's, at each board's call sites.  And it has no
 * path into a plugin of its own: every call below is made only because a board
 * called the function that makes it.
 *
 * [!] THE DEPTH IS SAMPLED HERE, IMMEDIATELY BEFORE EACH INDIRECT CALL (issues
 * #119, #126).  The stack pointer is read in the function that makes the call,
 * after everything it builds -- a caller's descriptor array is in an outer
 * frame, so it is inside the number -- and handed to the board's note().  The
 * note is a call of its own and its frame is gone before the plugin's begins.
 * Taken only on the branch that really calls: an answer that entered nothing
 * sampled nothing.  entry() is not called from here (svc/plugin_exec.c calls
 * it); each board samples it in its exec_ok hook.
 *
 * No mutable storage.
 */
#ifndef NN_ACTIVE_CORE_H
#define NN_ACTIVE_CORE_H

#include <stddef.h>
#include <stdint.h>

#include "blazeface.h"    /* BF_ERR_* -- the shared decode vocabulary */
#include "nn_svc.h"       /* nn_svc_write_fn, NN_SVC_THRESH_NONE */
#include "plugin_abi.h"
#include "tensor.h"

#ifdef __cplusplus
extern "C" {
#endif

/** What a board hands this file.  A constant in the board's .rodata. */
struct nn_active_board {
	/** Is a plugin loaded and live? */
	int  (*active)(void);
	/** The callable address of @p slot, or NULL when it has none. */
	void *(*slot)(unsigned slot);
	/** Record that @p slot is about to be entered with the stack pointer at
	 *  @p sp (0 on a host build). */
	void (*note)(unsigned slot, uintptr_t sp);
	/** shapes_ok()'s answer when no plugin is loaded: 0 or 1. */
	int  shapes_without_plugin;
};

/**
 * The stack pointer, read where this is written.  A macro so the read happens
 * in the caller's own frame -- a helper would sample its own.
 */
#if defined(__arm__)
#define NN_ACTIVE_CORE_SP()                                             \
	({ uintptr_t nn_active_core_sp_;                                \
	   __asm__ volatile ("mov %0, sp" : "=r"(nn_active_core_sp_));  \
	   nn_active_core_sp_; })
#else
/* A host build has no thread stack to be in; 0 is outside every one. */
#define NN_ACTIVE_CORE_SP() ((uintptr_t)0)
#endif

/** Is a plugin in force?  A live plugin with a decode slot. */
int nn_active_core_is_plugin(const struct nn_active_board *b);

/** Can the active decoder read these?  With no plugin, the board's answer.
 *  A NULL @p d is refused. */
int nn_active_core_shapes_ok(const struct nn_active_board *b,
                             const struct tensor_desc *d, unsigned n);

/**
 * Decode with the plugin.  Its count, or a negative BF_ERR_*: BF_ERR_ARG for a
 * NULL @p d, BF_ERR_UNINIT with no plugin -- deliberately not BF_ERR_MODEL,
 * which means "not a detector" and routes to the shared class report.
 */
int nn_active_core_decode(const struct nn_active_board *b,
                          const struct tensor_desc *d, unsigned n);

/** Let the plugin paint.  Nothing with no plugin, no draw slot or no painter. */
void nn_active_core_draw(const struct nn_active_board *b,
                         const struct plugin_painter *paint);

/** Will it paint?  0 with no plugin. */
int nn_active_core_can_draw(const struct nn_active_board *b);

/** Will it describe its result?  0 with no plugin. */
int nn_active_core_can_report(const struct nn_active_board *b);

/** Let it describe its result: what its report returned, or 0 with no plugin,
 *  no report slot or no writer. */
int nn_active_core_report(const struct nn_active_board *b,
                          nn_svc_write_fn write, void *ctx);

/** The plugin's threshold in milli, or NN_SVC_THRESH_NONE when nothing holds
 *  one (no plugin, no param_get slot, or the plugin declines). */
unsigned nn_active_core_get_thresh_milli(const struct nn_active_board *b);

/**
 * Set it.
 *
 * [!] THREE ANSWERS, NOT TWO (issue #104).  "The value was refused" and "there
 * is nothing here to hold one" send an operator to different places -- a number
 * to change, or a container to load.
 */
#define NN_ACTIVE_THRESH_OK           0
#define NN_ACTIVE_THRESH_REFUSED    (-1)
#define NN_ACTIVE_THRESH_NO_DECODER (-2)

int nn_active_core_set_thresh_milli(const struct nn_active_board *b,
                                    unsigned milli);

#ifdef __cplusplus
}
#endif

#endif /* NN_ACTIVE_CORE_H */
