/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    nn_probe.h
 * @brief   How deep the stack is where a plugin callback is entered, slot by
 *          slot and thread by thread (issue #119).
 *
 * What a plugin may be lent on a thread is what that thread has left at the
 * instant the plugin is entered, less the exception reserve and a margin
 * (port/npu/nn_plugin_stack.h).  This records the first term, where it happens:
 *
 *   - the six callbacks nn_active.c calls through are sampled in the function
 *     that makes the indirect call, immediately before it (NN_PROBE_SP() reads
 *     the stack pointer there, explicitly -- not the address of a local);
 *   - entry() is called from inside the shared loader (svc/plugin_exec.c),
 *     which owns no storage and so cannot hold a probe.  The loader calls the
 *     board's exec_ok hook from the very frame it then calls entry() from,
 *     with the same stack pointer, and the hook samples there: the hook's own
 *     frame is on top, so the number is an UPPER BOUND on the entry depth and
 *     needs no constant read off an ELF.  The sample waits in a
 *     struct nn_probe_pending until plugin_run_load() knows whether entry()
 *     was CALLED -- the load succeeded, or entry() ran and refused -- and is
 *     recorded only then (see nn_probe_pending_settle()).
 *
 * [!] A SAMPLE NOBODY CAN ATTRIBUTE IS NOT A MEASUREMENT.  No current thread,
 * an exception handler, a thread this board cannot name, or a stack pointer
 * outside the thread's own stack: each is counted as INVALID and turns into no
 * depth and no "left" at all.  Recording it would mean inventing which stack it
 * was taken on.
 *
 * [!] THE CONTEXT IS NAMED BY (base priority, stack size).  The four threads a
 * plugin runs on are created by three different owners -- the camera, the panel
 * sink and the shell core -- and the port may not reach up into the shell to ask
 * which kind of shell thread it is on.  Both numbers are published by their
 * owners already, the base priority is the one priority inheritance leaves
 * alone, and nn_probe_rtos.c asserts the four pairs apart.  A thread matching
 * none of them is INVALID, never guessed.
 *
 * Everything in this header except NN_PROBE_SP() and the two firmware entry
 * points at the bottom is pure, and has a host test (test/test_nn_probe.c).
 */
#ifndef NN_PROBE_H
#define NN_PROBE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Where a callback ran.  The order is the bit order of nn_plugin_stack.h's
 *  GROVE_PLUGIN_ON_* masks. */
enum nn_probe_ctx {
	NN_PROBE_PRODUCER = 0,   /**< the camera producer (`nn stream`)        */
	NN_PROBE_PANEL    = 1,   /**< the panel sink's thread (draw)           */
	NN_PROBE_CONSOLE  = 2,   /**< a shell instance                         */
	NN_PROBE_BG       = 3,   /**< a background job (`... &`)               */
	NN_PROBE_CTX_COUNT = 4,
	NN_PROBE_UNKNOWN  = NN_PROBE_CTX_COUNT,
};

/** How a thread of each context is recognised. */
struct nn_probe_thread_class {
	uint32_t prio;    /**< tx_thread_user_priority: what it was created
	                       with, moved only by tx_thread_priority_change()
	                       -- which nothing here calls -- and never by
	                       priority inheritance                          */
	uint32_t stack;   /**< its stack size, as given to tx_thread_create */
};

/** One slot on one context.  hits == 0 means "never observed", and then no
 *  other field means anything. */
struct nn_probe_cell {
	uint32_t depth_hw;   /**< deepest entry seen, bytes from the stack top  */
	uint32_t left_min;   /**< least left below that entry, bytes            */
	uint32_t stack;      /**< the stack size that least-left was out of     */
	uint32_t hits;       /**< observations (saturates)                      */
};

/** One slot, every context, plus the samples that could not be attributed. */
struct nn_probe_row {
	struct nn_probe_cell cell[NN_PROBE_CTX_COUNT];
	uint32_t invalid;
};

/** Results of nn_probe_measure(). */
#define NN_PROBE_OK          0
#define NN_PROBE_OUT_OF_RANGE 1   /**< the stack pointer is not in the stack */
#define NN_PROBE_BAD_STACK   2    /**< a zero or wrapping stack description  */

/** Which context a thread is, or NN_PROBE_UNKNOWN.  Both numbers must match. */
enum nn_probe_ctx nn_probe_classify(
	const struct nn_probe_thread_class tab[NN_PROBE_CTX_COUNT],
	uint32_t prio, uint32_t stack);

/**
 * The depth at a plugin's entry and what is left below it.
 *
 * @param sp     the stack pointer where it was sampled
 * @param lo     the lowest byte of the thread's stack
 * @param size   the thread's stack size; the top is lo + size
 * @param extra  bytes the stack grows between the sample and the branch
 * @return NN_PROBE_OK and both outputs set, or a reason and neither touched.
 *         A depth past the bottom is still a measurement: left is then 0.
 */
int nn_probe_measure(uintptr_t sp, uintptr_t lo, uint32_t size, uint32_t extra,
                     uint32_t *depth, uint32_t *left);

/** Fold one valid observation into @p r.  A context out of range is counted
 *  as invalid instead. */
void nn_probe_record(struct nn_probe_row *r, unsigned ctx, uint32_t depth,
                     uint32_t left, uint32_t stack);

/** Count one observation that could not be attributed. */
void nn_probe_reject(struct nn_probe_row *r);

/**
 * One line of the stack report, for `nn stream stats`.
 *
 * Every context the slot runs on (@p runs, GROVE_PLUGIN_ON_* bits) is shown,
 * as `depth/stack` when observed and `--` when not; one it was observed on but
 * is not supposed to run on is shown too, flagged `!`, because that would mean
 * the table in nn_plugin_stack.h is wrong.  Then `left N`, the least left over
 * what was observed, or "not measured" when nothing was; then `inv N` if any
 * sample could not be attributed (`inv >99999` past that); then @p note if not
 * NULL.  The note is the one field the widest lines may lose.
 *
 * @return the length written (always NUL-terminated when @p cap > 0), which is
 *         never 0 when @p cap > 1 -- the caller stops at the first empty line,
 *         so "nothing measured" must still be a line.
 */
int nn_probe_line(char *buf, size_t cap, const char *label,
                  const struct nn_probe_row *r, unsigned runs,
                  const char *note);

/** The largest invalid count a line spells out in full. */
#define NN_PROBE_INV_SHOWN_MAX 99999u

/** A slot's name in the report, and what its number includes that a plain
 *  sample would not (NULL when nothing): one table, so the report and the test
 *  of its widest line read the same words. */
const char *nn_probe_slot_label(unsigned slot);
const char *nn_probe_slot_note(unsigned slot);

/* ---- a sample that waits to learn whether entry() was called ------------ */

/**
 * entry()'s sample, between the hook that takes it and the load that decides
 * whether it happened (plugin_run.c).
 *
 * [!] A SAMPLE COUNTS ONLY IF IT IS THIS LOAD'S, TAKEN ONCE, AND entry() WAS
 * THEN CALLED.  The hook runs after the image is copied and before the
 * branch, and the hook's own verdict can still stop the load there -- a sample
 * of a branch that never happened is not a depth.  But entry() REFUSING is
 * not that: entry() ran, at exactly the depth sampled, and dropping it would
 * let an earlier, shallower success stand in the report unmarked (issue
 * #119's review: the console's depth differs by 32 B between a line ended with
 * CR and one ended with LF).  A hook called from another thread, or twice in
 * one load, is not "immediately before entry()" and is not a depth anybody can
 * attribute.  Plain data: the owner keeps it, and the one thread that loads
 * is the only one that touches it.
 */
struct nn_probe_pending {
	uintptr_t   sp;      /**< the stack pointer the hook read             */
	const void *who;     /**< the thread the load runs on                 */
	uint32_t    takes;   /**< hook calls by that thread during the load   */
	uint8_t     armed;   /**< a load is in flight                         */
};

/** What to do with the sample once the load has answered. */
enum nn_probe_settle {
	NN_PROBE_SETTLE_NONE = 0,  /**< entry() was not called, or no load was
	                                in flight                             */
	NN_PROBE_SETTLE_RECORD,    /**< one sample, this load's: record it      */
	NN_PROBE_SETTLE_REJECT,    /**< entry() was called but the samples do
	                                not describe its branch: count invalid */
};

/** A load is starting on thread @p who; forget any earlier sample. */
void nn_probe_pending_arm(struct nn_probe_pending *p, const void *who);

/** The hook: keep @p sp if it was taken on the loading thread @p who.  A call
 *  outside a load is harmless: arm() starts over and settle() ignores it. */
void nn_probe_pending_take(struct nn_probe_pending *p, const void *who,
                           uintptr_t sp);

/**
 * The load has answered; @p entered is nonzero when entry() was called
 * (plugin_run_entered()).  Disarms, and says whether the sample is to be
 * recorded, counted invalid, or dropped; on NN_PROBE_SETTLE_RECORD, @p sp is
 * set.
 */
enum nn_probe_settle nn_probe_pending_settle(struct nn_probe_pending *p,
                                             int entered, uintptr_t *sp);

/* ---- firmware only (nn_probe_rtos.c) -------------------------------------- */

/**
 * The stack pointer, read where this is written.  A macro so the read happens
 * in the caller's own frame -- a helper would sample its own.
 */
#if defined(__arm__)
#define NN_PROBE_SP()                                                   \
	({ uintptr_t nn_probe_sp_;                                      \
	   __asm__ volatile ("mov %0, sp" : "=r"(nn_probe_sp_));        \
	   nn_probe_sp_; })
#else
/* A host build has no thread stack to be in; 0 is outside every one. */
#define NN_PROBE_SP() ((uintptr_t)0)
#endif

/** Record one sample for @p slot, taken on the current thread at @p sp, with
 *  @p extra bytes still to be pushed before the plugin is entered. */
void nn_probe_note(unsigned slot, uintptr_t sp, uint32_t extra);

/** Count one invalid sample for @p slot -- one that cannot be attributed. */
void nn_probe_discard(unsigned slot);

/** One slot's row, copied in one critical section. */
void nn_probe_snapshot(unsigned slot, struct nn_probe_row *out);

#ifdef __cplusplus
}
#endif

#endif /* NN_PROBE_H */
