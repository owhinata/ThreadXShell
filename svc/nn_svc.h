/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    nn_svc.h
 * @brief   The board-neutral contract one shared `nn` command speaks (issue #50).
 *
 * Three boards run inference behind three different runtimes: f746g-disco and
 * wio-lite-ai share a model/tensor API in their own port/nn/, and
 * grove-vision-ai-v2 has port/npu/, whose open also brings up the NPU and takes a
 * lease on the external NOR window.  Each grew its own command, and the three
 * drifted in name, grammar and output.  This is the seam that lets ONE command
 * in shell/cmds/ serve all three: the declarations here are implemented by a
 * board adapter in that board's port, the same way svc/blazeface.h is one
 * decoder above three tensor types.
 *
 * WHAT THIS FILE IS NOT.  It is not a model API -- the boards already have those
 * and keep them.  It is the smaller thing a *command* needs: enough to print
 * what is loaded, to point a load at something, to run one, and to say who owns
 * what afterwards.  A board's own richer API stays below it and is not reachable
 * from shell/.
 *
 * [!] TWO LIFETIMES, NOT ONE.  A TRANSIENT CLAIM is the exclusion a board holds
 * while one operation runs; the PERSISTENT MODEL LIFECYCLE is the interpreter,
 * and on Grove the NPU and the NOR lease, that outlive it.  They are not the same
 * and an operation that conflates them will leak one or free the other early:
 * Grove drops its gate the instant an open succeeds and keeps the NPU and lease
 * until close, and wio does the same around its reload.  So every operation here
 * releases its transient claim before returning -- unless it says otherwise
 * through @ref nn_claim -- while the model lifecycle is moved only by
 * nn_svc_model_load() and nn_svc_model_unload().
 *
 * THIS FILE OWNS NO STORAGE, and neither does the command above it.  The board
 * adapter owns whatever has to be remembered, so that each board keeps its own
 * memory map: see cmake/check_no_mutable_storage.py, which audits the shared
 * command per board for exactly that.
 */
#ifndef NN_SVC_H
#define NN_SVC_H

#include <stddef.h>
#include <stdint.h>

#include "blazeface.h"       /* struct bf_det, struct bf_result */
#include "nn_det_record.h"   /* struct nn_det_snapshot (issue #97) */
#include "tensor.h"          /* struct tensor_desc (issue #97) */

#ifdef __cplusplus
extern "C" {
#endif

/*
 * [!] THIS HEADER INCLUDES NOTHING FROM A BOARD, and that is a property of svc/
 * worth keeping rather than an accident: every other shared file here is
 * self-contained, which is why the host tests can link them with no board on the
 * include path at all.
 *
 * The capability macros that decide WHICH of the operations below a build has
 * therefore do not live here.  They come from the board's own nn_svc_config.h --
 * the same seam that already gives shared code the board's log.h and timebase.h
 * -- and they are read by the two layers that may see a board header: the shared
 * command in shell/cmds/, which registers a subcommand only under its macro, and
 * the board's own adapter.  A declaration costs nothing, so everything is
 * declared here unconditionally and a board simply does not implement, and the
 * command does not call, what it does not have.
 */

/* ---- what an operation reports ------------------------------------------- */

/**
 * Status of one operation.  Zero is success; a board's own richer failure code
 * passes through as a negative number it can name through nn_svc_strerror().
 *
 * These sit far from zero so they cannot be confused with a backend's, the same
 * convention the boards' own nn.h already uses.
 */
#define NN_SVC_OK              0
#define NN_SVC_ERR_ARG      (-70)  /**< a null pointer or an out-of-range count  */
#define NN_SVC_ERR_NOSUP    (-71)  /**< this board does not implement it at all  */
#define NN_SVC_ERR_SPEC     (-72)  /**< the spec tag is not one this board takes */
#define NN_SVC_ERR_STATE    (-73)  /**< no model is active, or one already is    */
#define NN_SVC_ERR_BUSY     (-74)  /**< a transient claim is held elsewhere      */
#define NN_SVC_ERR_CANCEL   (-75)  /**< the operator interrupted it              */
#define NN_SVC_ERR_HW       (-76)  /**< the hardware refused or did not settle   */

/**
 * What the CALLER may do about the board's transient claim afterwards.
 *
 * [!] THIS IS CLEANUP AUTHORITY, NOT A SNAPSHOT OF WHO HOLDS WHAT.  It cannot be
 * the latter: on wio the stream worker and the console both race to be the last
 * one out, under one critical section, and exactly one of them performs the
 * release -- so by the time a caller reads a result, the claim it describes may
 * already have been dropped by the other thread.  What stays true regardless is
 * whether the CALLER is the one allowed to release it, and that is what this
 * says.
 *
 * [!] AND IT IS A SEPARATE FIELD FROM THE STATUS, because a board can finish its
 * teardown cleanly and still have something to report -- Grove does exactly that
 * when a stop and detach both confirm but the session recorded a fault.  Folding
 * the two into one enum throws away one of the two answers, and it is never
 * knowable afterwards which one was thrown away.
 */
enum nn_claim {
	/** Never acquired, or the board rolled it back itself.  The caller must
	 *  NOT release: wio's load takes its software claim, fails to take the
	 *  hardware one, and unwinds the first internally -- releasing again here
	 *  would free a claim that by then belongs to somebody else. */
	NN_CLAIM_NONE = 0,
	/** The caller owns it and releases it exactly once. */
	NN_CLAIM_CALLER,
	/** The caller has no release authority.  Cleanup was incomplete when it
	 *  was observed; the owning worker or lifecycle may settle it
	 *  asynchronously, and a documented retry is permitted.  wio and f746
	 *  both return this from a stop that timed out, and their release is
	 *  idempotent precisely so the retry works. */
	NN_CLAIM_RETRYABLE,
	/** No release authority, and quiescence cannot be established -- only a
	 *  reset clears it.  Grove returns this when a producer stop or a panel
	 *  drain is unconfirmed, because releasing the NPU there would let a
	 *  later `nn model unload` dismantle an interpreter a running thread is
	 *  still inside.  A board that CANNOT TELL whether an asynchronously used
	 *  resource is quiescent must fail closed to this rather than guess. */
	NN_CLAIM_TERMINAL,
};

/**
 * Where the model lifecycle ended up.  Reported by nn_svc_model_load()
 * INDEPENDENTLY of its status.
 *
 * [!] A FAILED LOAD DOES NOT PROMISE THE PREVIOUS MODEL SURVIVED.  Both STM32
 * backends rebuild the previous model after rejecting a new one, and both
 * document that if even that rebuild fails the model is left closed -- f746 says
 * so in its own reload contract, and wio's dispatcher adopts whatever handle came
 * back, clearing `open` when that is NULL.  An operator told "rejected, previous
 * unchanged" would then go on believing a model is loaded when none is, so the
 * two cases are different answers and never share one.
 */
enum nn_model_state {
	NN_MODEL_EMPTY = 0,   /**< nothing active -- including after a failed roll
	                       *   back, which is NOT the same as a plain refusal */
	NN_MODEL_NEW,         /**< the model just asked for is active             */
	NN_MODEL_PREVIOUS,    /**< the request was refused; the old one is active */
};

/** The two answers every operation gives.  Never collapse them into one. */
struct nn_op_result {
	int     status;  /**< NN_SVC_OK, or a negative code               */
	uint8_t claim;   /**< enum nn_claim -- the caller's authority     */
};

/* ---- pointing a load at something ---------------------------------------- */

/**
 * How a model is named.
 *
 * [!] TAGGED, NOT A BARE STRING.  The same word means different things on
 * different boards -- a blob name on Grove, a filesystem path on f746, and
 * nothing at all on wio, which has no lookup by name -- so a shared parser given
 * one string would have to guess per board, which is board knowledge in shell/
 * by another route.  Spelling the tag makes the grammar decidable where it is
 * parsed, and lets a board refuse a namespace it does not have instead of
 * pretending to implement it.
 *
 * [!] THE RAW FORM CARRIES A LENGTH AND THAT IS NOT CEREMONY.  It is the bound
 * the FlatBuffer verifier is given before anything walks the payload; "whatever
 * is left in the window" is not a bound, and this repo forbids it.
 */
enum nn_spec_tag {
	NN_SPEC_NONE = 0,
	NN_SPEC_NAME,     /**< `--name <name>`  -- an asset store entry       */
	NN_SPEC_SLOT,     /**< `--slot <index>` -- an asset store index       */
	NN_SPEC_PATH,     /**< `--path <path>`  -- a filesystem path          */
	NN_SPEC_BUILTIN,  /**< `builtin`        -- the model built into the image */
	NN_SPEC_ADDR,     /**< `--addr <addr> <len>` -- raw, always with a length */
};

/** Longest model name the shared parser will carry.  A board with a shorter
 *  limit of its own refuses the overlong ones it cannot hold; this is only the
 *  bound on what shell/ is willing to copy. */
#define NN_SPEC_NAME_MAX 31

/** Where a load is pointed.  Filled by the shared parser before ANY acquisition
 *  or hardware access, so that an unsupported tag is refused while nothing is
 *  held (status NN_SVC_ERR_SPEC, claim NN_CLAIM_NONE). */
struct nn_spec {
	uint8_t  tag;                          /**< enum nn_spec_tag          */
	char     name[NN_SPEC_NAME_MAX + 1];   /**< NN_SPEC_NAME              */
	uint32_t slot;                         /**< NN_SPEC_SLOT              */
	const char *path;                      /**< NN_SPEC_PATH -- argv, not copied */
	uint32_t addr;                         /**< NN_SPEC_ADDR              */
	uint32_t len;                          /**< NN_SPEC_ADDR -- required  */
};

/* ---- what `nn info` prints ----------------------------------------------- */

/** Identity and cost of what is loaded.  Strings are board-owned and stay valid
 *  while the model does; NULL where a board has nothing to say. */
struct nn_svc_info {
	const char *backend;      /**< runtime that would run a model         */
	const char *model;        /**< active model's name, or NULL if none   */
	const char *source;       /**< what the load was pointed at, or NULL  */
	uint32_t    arena_bytes;  /**< arena the board reserves               */
	uint32_t    arena_used;   /**< of that, what the model needs (0=n/a)  */
	uint8_t     model_active; /**< 0 when nothing is loaded               */
};

/* ---- operations ---------------------------------------------------------- */

/**
 * Has the operator asked to stop?
 *
 * An operation that waits needs to notice Ctrl+C, and the thing that knows is a
 * shell instance -- which must not cross into a board's port.  So the command
 * passes this instead: a plain function and an opaque context, neither of which
 * says anything about a shell.  A board calls it while it waits and gives up
 * with NN_SVC_ERR_CANCEL when it returns non-zero.
 *
 * [!] IT MAY BE NULL, and a board must cope: a caller with nothing to cancel on
 * (a test, or a path that cannot be interrupted) passes none, and "no cancel
 * function" means "never cancelled", never a crash.
 */
typedef int (*nn_svc_cancel_fn)(void *ctx);

/**
 * Read a whole file into a buffer.
 *
 * A board whose model source is a filesystem path needs one of these, and the
 * filesystem helper that has it takes a shell instance and prints its own
 * errors -- so it cannot be called from a port.  The same shape as the cancel
 * hook solves it: the board's own SHELL-layer file implements the read, the
 * shared command passes it down, and the port sees only a function pointer.
 * Nothing in a port ever names anything above it.
 *
 * @return 0 with @p len set, non-zero on failure (a message has been printed)
 */
typedef int (*nn_svc_read_fn)(void *ctx, const char *path, void *buf,
                              uint32_t cap, uint32_t *len);

/** Convenience for a board: true when @p fn says stop.  NULL never stops. */
static inline int nn_svc_cancelled(nn_svc_cancel_fn fn, void *ctx)
{
	return (fn != NULL) && (fn(ctx) != 0);
}

/** Backend, model and arena, for `nn info`.  Never fails on a board that has an
 *  adapter at all; a board with no model loaded still answers. */
void nn_svc_info(struct nn_svc_info *out);

/** A board's own name for a status it returned, or NULL to let the shared
 *  command print the generic one. */
const char *nn_svc_strerror(int status);

/**
 * Point the model lifecycle at @p spec.
 *
 * Refuses an unsupported tag before acquiring anything.  On the supported tags
 * the board runs its OWN ordered sequence -- Grove brings the NPU and lease up
 * before it resolves a name, because the lookup reads through the window that
 * bring-up opens -- and unwinds it itself on failure.
 *
 * @param state  where the lifecycle ended up, independent of the status
 */
void nn_svc_model_load(const struct nn_spec *spec, nn_svc_read_fn read,
                       void *ctx, struct nn_op_result *res,
                       enum nn_model_state *state);

/** Release the model-specific persistent resources, in model -> NPU -> lease
 *  order.  Idempotent: unloading nothing succeeds. */
void nn_svc_model_unload(struct nn_op_result *res);

/**
 * Pin the active model so the DATA pointers in its descriptors stay valid.
 *
 * [!] A DESCRIPTOR IS SAFE TO READ UNGUARDED; ITS BUFFER IS NOT.  Shape, dtype,
 * scale and length are copied out and cannot be invalidated underneath a reader.
 * The `data` pointer is different: it addresses the arena, and an unload on
 * another console can close the interpreter, hand back the NOR lease, or rebuild
 * the model singleton while a reader is part way through it.  Both boards that
 * had an equivalent command already knew this -- one guarded its whole tensor
 * report, the other guarded the read loop and deliberately left `info`
 * unguarded because `info` never dereferences the buffer.
 *
 * So: anything that walks tensor CONTENTS holds this across the walk.  Anything
 * that only prints the descriptor does not need it, and taking it there would
 * make `nn info` refuse while a stream runs -- which is exactly the diagnostic
 * an operator wants at that moment.
 *
 * Not recursive: no operation here takes it twice, and none of the board
 * operations may be called while it is held.
 *
 * @return NN_SVC_OK, or a status if the model is gone or the claim is held
 */
int  nn_svc_tensors_pin(void);
void nn_svc_tensors_unpin(void);

/** Tensors, as the shared command sees them.  Valid only while a model is
 *  active.  The descriptor is a copy and may be read unguarded; dereferencing
 *  its @ref tensor_desc::data requires nn_svc_tensors_pin().
 *  @return the count, or a negative status. */
int nn_svc_output_count(void);
int nn_svc_output(unsigned index, struct tensor_desc *out);
int nn_svc_input(struct tensor_desc *out);

/**
 * Capture one frame, preprocess it, infer, and decode.
 *
 * One board hook on purpose: on Grove the sensor bus owner is decided under the
 * camera API mutex, so a shared caller must not split this into "is it
 * streaming?" and then a capture -- the answer would be stale before it is used.
 *
 * @param snap  the decode, boxes and diagnostics together (issue #97)
 * @param dets  optional; up to @p max boxes, normalised to the MODEL INPUT
 */
void nn_svc_run_once(struct nn_det_snapshot *snap, struct bf_det *dets, int max,
                     nn_svc_cancel_fn cancel, void *ctx,
                     struct nn_op_result *res);

/**
 * Decode the model's CURRENT outputs, without capturing anything.
 *
 * What `nn dets` is for: the tensors are whatever the last inference left, so
 * this says what the decoder makes of them.  The decoder's state and its
 * candidate scratch belong to the board (issue #97), which is why this is an
 * operation here and not something the shared command does for itself.
 *
 * The snapshot carries the boxes AND the diagnostics together, on purpose: read
 * separately, a console pairs one frame's boxes with a different frame's
 * numbers.
 */
void nn_svc_decode_current(struct nn_det_snapshot *snap, struct bf_det *dets,
                           int max, struct nn_op_result *res);

/**
 * Fill every input with a deterministic pattern, so runs are comparable.
 *
 * [!] NOT AN OPTIONAL TIDY-UP.  wio's arena is NOLOAD, so an input holds
 * whatever survived the last reset until something fills it, and two runs would
 * not be measuring the same work.  Which pattern is the board's business; that
 * there IS one is not.
 */
void nn_svc_bench_prepare(struct nn_op_result *res);

/**
 * What a benchmark measured.
 *
 * [!] MIN, AVERAGE AND MAX, NOT A TOTAL.  An average alone hides the thing a
 * benchmark is usually run to find: on a board where inference shares a core
 * with a console and a camera, the spread between the fastest and the slowest
 * run is the measurement.  One of the boards already reported all three and
 * dropping to a total would have been a quiet loss of resolution.
 *
 * Microseconds is the neutral unit: the boards count DWT cycles, ThreadX ticks
 * and a free-running vendor timer respectively, and only each of them knows how
 * to get from its own to real time.
 */
struct nn_bench_stats {
	uint32_t runs;      /**< completed runs -- fewer than asked if cancelled */
	uint64_t total_us;
	uint32_t min_us;
	uint32_t max_us;
	uint32_t avg_us;
};

/** Invoke @p iters times and report the spread. */
void nn_svc_bench_run(uint32_t iters, struct nn_bench_stats *out,
                      nn_svc_cancel_fn cancel, void *ctx,
                      struct nn_op_result *res);

/** Float input normalisation: 0 = [0,1], 1 = [-1,1].  Meaningless where the
 *  input is integer, which is why it is a capability and not a stub. */
void nn_svc_norm_set(int signed_range);
int  nn_svc_norm_get(void);

/** Draw the boxes on the board's live preview.  A capability because one board
 *  can turn it off independently while another draws whenever its preview runs
 *  -- "always on" is not the same answer as "there is a switch". */
void nn_svc_overlay_set(int on);
int  nn_svc_overlay_get(void);

/* ---- boxes --------------------------------------------------------------- */

/**
 * Map a box normalised to the MODEL INPUT onto the frame, as fractions of the
 * frame.
 *
 * [!] THIS IS NOT A MULTIPLICATION, AND ASSUMING IT IS PRINTS THE WRONG BOX.
 * wio resamples the whole frame into the input, so there the two spaces coincide
 * and scaling by 100 happens to be right.  Grove feeds the centre square of the
 * frame, so a box at the left edge of the input is nowhere near the left edge of
 * the frame.  The board owns the mapping because the board owns the crop -- and
 * it must be THE SAME mapping its overlay draws with, or the console and the
 * panel will disagree about where a face is (issue #48).
 *
 * @return NN_SVC_OK with @p out filled, or a negative status if the box is not
 *         finite or maps outside the frame entirely -- in both cases there is
 *         nothing to print.
 */
int nn_svc_box_to_frame(const struct bf_det *in, struct bf_det *out);

/* ---- detection threshold ------------------------------------------------- */

/** Score threshold in milli-probability, held by the board's decoder state. */
unsigned nn_svc_thresh_get(void);
int      nn_svc_thresh_set(unsigned milli);

#ifdef __cplusplus
}
#endif

#endif /* NN_SVC_H */
