/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    nn_core_model.h
 * @brief   The order of a `nn model load` and a `nn model unload`, written once
 *          for every board (issue #131).
 *
 * WHY THIS EXISTS.  svc/nn_swap.c decides where a load ended and what that
 * obliges, but each board still walked its own order around that table: when
 * the claim is taken, when the plugin lease is, when the backend is touched,
 * when the plugin is swapped, which obligations run and in what order, and
 * when the claim goes back.  The rules that matter -- the plugin is swapped
 * only after the backend took the new model, a bare model unloads the plugin,
 * a plugin is touched only under the lease, a model-less failure takes the
 * hardware down, BUSY is only ever answered before anything changed -- were
 * held by three copies being kept alike.  This is the one copy.
 *
 * THE LOAD, in order; the first step that fails ends it:
 *
 *   1. the spec's tag is one this board takes          (else SPEC)
 *   2. the board's own check of the spec's arguments   (check_spec)
 *   3. the board's preconditions                       (admit)
 *   4. the claim                                       (else BUSY)
 *   5. had_open, read UNDER the claim
 *   6. the board's preparation                         (prepare; e.g. the NPU)
 *   7. the bytes, resolved and split                   (fetch)     -> REFUSED
 *   8. the plugin lease, bounded                       (else BUSY) -> REFUSED
 *   9. the transition counter goes ODD
 *  10. the backend's swap, which says itself what it left
 *  11. only if the backend took the new model: the plugin -- started, or
 *      unloaded for a bare model
 *  12. svc/nn_swap.c's table, and its obligations, in unload's order:
 *      plugin unload (only under the lease) -> backend release + hw_down ->
 *      the last result -> forget / commit -> the geometry
 *  13. the counter goes EVEN, the lease goes back, the claim goes back.
 *
 * Steps 1-4 change nothing and leave the state as it was; a failure at 6
 * leaves the table unconsulted (the hook took back what it took).  REFUSED
 * with a model open obliges nothing; without one it takes down what step 6
 * brought up.
 *
 * THE UNLOAD: preconditions -> claim -> lease (refused: nothing changes) ->
 * counter ODD -> plugin unload -> the last result -> the geometry -> lease back
 * -> backend release -> hw_down -> forget -> counter EVEN -> claim back.
 *
 * [!] A BUSY REFUSAL NEVER REPLACES OR LOSES THE OPEN MODEL (it is answered
 * before the swap; staging into an inactive buffer or an empty-state cleanup
 * may already have run).  This file's
 * own BUSY refusals are the claim and the lease; a board's admit, prepare or
 * fetch hook may answer BUSY too (wio-lite-ai's blob store and OCTOSPI1 guard)
 * -- all of them before step 10 -- and a BUSY returned by the swap or the
 * plugin start is reported as NN_SVC_ERR_HW.  The
 * shared command relies on this (issue #131 stage 8): a load refused BUSY has
 * changed nothing, so it has no model state to report.
 *
 * [!] THE TRANSITION COUNTER is odd from the first change to what is open to
 * the last, and only then -- a load refused before the lease never moves it.
 * A reader that cannot take the claim reads it on both sides of its copy and
 * asks nn_core_model_copy_stands().  A board that answers `nn info` from the
 * claim's holder instead (grove-vision-ai-v2) keeps the counter and never
 * reads it.
 *
 * [!] IT OWNS NO STORAGE.  The state is the board's @ref nn_core_model, the
 * facts its const @ref nn_core_model_board; the derived audit
 * (cmake/shared_storage_gate.cmake) compiles this like every other svc/ TU.
 * It includes no board header and no tx_api.h.  EVERY HOOK IS CALLED WITH
 * INTERRUPTS ENABLED: a hook that needs a critical section closes its own
 * (grove-vision-ai-v2's claim tests the threshold-call count inside its own).
 *
 * [!] THE CONSOLE ENTERS A PLUGIN THROUGH THIS FILE.  plugin_start runs the new
 * plugin's entry() on the console's stack, below this file's frame, so this
 * frame is part of the depth a board declares for that slot.  Keep it small:
 * no large locals, and fetch -- which may hold a slot table -- has returned
 * before plugin_start is called.
 */
#ifndef NN_CORE_MODEL_H
#define NN_CORE_MODEL_H

#include <stdint.h>

#include "nn_svc.h"   /* struct nn_spec, struct nn_op_result, nn_model_state */

#ifdef __cplusplus
extern "C" {
#endif

/** One board's model lifecycle state.  Zero-initialised static storage in the
 *  adapter; this file never declares one. */
struct nn_core_model {
	/** Odd while a load or an unload is changing what is open. */
	uint32_t seq;
};

/** One load or unload.  The board's adapter owns it, on its own stack. */
struct nn_core_model_job {
	const struct nn_spec *spec;   /**< the load's; NULL for an unload        */
	struct nn_op_result  *res;    /**< the board words its refusals here     */
	void                 *board;  /**< the board's resolution: what fetch
	                               *   found, what commit takes             */
	int had_open;   /**< set by this file, under the claim               */
	int bare;       /**< set by fetch: no plugin travels with the model, so
	                 *   a successful swap unloads the one that is there  */
};

/** What every backend answers.  A backend that cannot swap (a static model) is
 *  not handed to this file: its adapter answers a load as it does today. */
struct nn_core_model_backend {
	/**
	 * Replace what is open with the job's model, as ONE transaction: close,
	 * open the new one, and if that fails reopen the previous one from the
	 * same bytes.  @p model_after is the backend's OWN answer to "is a model
	 * open now" -- never asked again afterwards.
	 * @return NN_SVC_OK if it took the new model, else the status to report
	 *         (with the detail written)
	 */
	int  (*swap)(struct nn_core_model_job *j, int *model_after);
	/** Close whatever is open.  An unload, and a load that ends EMPTY. */
	void (*release)(void);
	/** Whether a model is open.  NO SIDE EFFECTS: it builds nothing.  Asked
	 *  only for a refusal before the claim and for had_open. */
	int  (*has_model)(void);
	/** The board's words for a backend code; for `nn info`.  This file never
	 *  calls this or the two below: NULL where the board asks elsewhere. */
	const char *(*strerror)(int rc);
	/** The arena reserved for the model, and what the open model uses of
	 *  it; for `nn info`.  0 when the backend reserves nothing. */
	uint32_t (*reserved)(void);
	uint32_t (*used)(void);
};

/** The bit for @p tag in @ref nn_core_model_board::tags. */
#define NN_CORE_MODEL_TAG(tag) (1u << (unsigned)(tag))

/**
 * What one board is.  Const, in the board's .rodata.  A hook that may be NULL
 * says so; NULL means the board has nothing to do at that step.
 */
struct nn_core_model_board {
	/** The board's critical section, for the counter only. */
	unsigned (*cs_enter)(void);
	void     (*cs_exit)(unsigned posture);
	const struct nn_core_model_backend *backend;
	/** NN_CORE_MODEL_TAG() of every spec tag this board loads from. */
	uint32_t tags;
	/** Word a SPEC refusal (the tag is not in @ref tags).  NULL: none. */
	void (*spec_refused)(struct nn_core_model_job *j);
	/** The spec's arguments, before anything is acquired.  NULL: none.
	 *  @return NN_SVC_OK or the status to report */
	int  (*check_spec)(struct nn_core_model_job *j);
	/** The board's preconditions, before the claim (a stream, a resource
	 *  guard).  NULL: none.  @return NN_SVC_OK or the status to report */
	int  (*admit)(struct nn_core_model_job *j);
	/** The claim.  A board that refuses a replacement for its own reasons
	 *  decides them here, in the same critical section as the claim.
	 *  @return non-zero if taken
	 *
	 *  It takes no job, so a refusal here has no words of its own.  A board
	 *  whose claim has more than one answer to give (wio-lite-ai,
	 *  f746g-disco) takes it as admit's LAST step instead -- admit holds
	 *  nothing when it fails -- and answers this with a constant 1, so
	 *  nothing runs between the two. */
	int  (*claim_take)(void);
	void (*claim_give)(void);
	/** Bring up what the load needs, knowing j->had_open.  On failure it gives
	 *  back what it took itself; the table is not consulted.  NULL: none.
	 *  @return NN_SVC_OK or the status to report */
	int  (*prepare)(struct nn_core_model_job *j);
	/** Resolve the model's bytes and split a container; sets j->bare.
	 *  @return NN_SVC_OK, or the status of a REFUSED ending */
	int  (*fetch)(struct nn_core_model_job *j);
	/** The plugin lease, bounded.  @return non-zero if held (else the detail
	 *  is set).
	 *
	 *  [!] NULL ON A BOARD WITH NO PLUGIN, AND THEN NO PLUGIN HOOK IS EVER
	 *  CALLED: plugin_start and plugin_unload are skipped and geom_clear is
	 *  told the plugin's copy may not be touched.  The load proceeds as if
	 *  the lease were held (the counter still brackets the change).  One
	 *  rule: a plugin is touched only when lease_take is not NULL and held. */
	int  (*lease_take)(struct nn_core_model_job *j);
	void (*lease_give)(void);
	/** Start the job's plugin (it carries one: !j->bare), under the lease and
	 *  only once the backend took the model.  NULL: no plugin.
	 *  @return NN_SVC_OK, or the status of an UNDECODED ending */
	int  (*plugin_start)(struct nn_core_model_job *j);
	/** Unpublish the plugin; only ever called under the lease.  NULL: none. */
	void (*plugin_unload)(void);
	/** Bring down what prepare brought up.  NULL: none. */
	void (*hw_down)(void);
	/** The last result goes.  NULL: none. */
	void (*invalidate)(void);
	/** The adapter's identity of the open model is cleared. */
	void (*forget)(void);
	/** The job's model becomes the adapter's identity. */
	void (*commit)(struct nn_core_model_job *j);
	/** The geometry of the last capture goes; @p leased says whether the
	 *  plugin's copy may be touched.  NULL: none. */
	void (*geom_clear)(int leased);
};

/**
 * `nn model load`.  The board fills j->spec, j->res (its detail cleared) and
 * j->board; this fills j->res's status and disposition and @p state.
 */
void nn_core_model_load(struct nn_core_model *m,
                        const struct nn_core_model_board *b,
                        struct nn_core_model_job *j,
                        enum nn_model_state *state);

/** `nn model unload`.  Idempotent: unloading nothing succeeds. */
void nn_core_model_unload(struct nn_core_model *m,
                          const struct nn_core_model_board *b,
                          struct nn_core_model_job *j);

/** The transition counter, read under the board's critical section. */
uint32_t nn_core_model_seq(const struct nn_core_model *m,
                           const struct nn_core_model_board *b);

/**
 * Whether a copy taken WITHOUT the claim describes one model.  Pure.
 *
 * @param seq0, seq1   the counter read before and after the copy
 * @param claim_free   the copy was taken under the claim (no load can run)
 *
 * The copy stands when the claim was taken, or when no load or unload was
 * between its steps at either end (seq0 even) nor ran whole in between
 * (seq0 == seq1).  Otherwise the answer is BUSY, not "no model".
 */
int nn_core_model_copy_stands(uint32_t seq0, uint32_t seq1, int claim_free);

#ifdef __cplusplus
}
#endif

#endif /* NN_CORE_MODEL_H */
