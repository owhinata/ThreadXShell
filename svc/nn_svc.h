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
#include "nn_stream_life.h" /* the shared stream lifecycle (issue #99)   */
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
 * passes through as a negative number, explained in the result's `detail`.
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
#define NN_SVC_ERR_STALE    (-77)  /**< the sample straddled a transition; retry */
#define NN_SVC_ERR_GEN      (-78)  /**< that stream generation is not current    */

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
	/** Never acquired, the board rolled it back itself, or it belongs to
	 *  something with its own lifetime.  The caller must NOT release: wio's
	 *  load takes its software claim, fails to take the hardware one, and
	 *  unwinds the first internally -- releasing again here would free a claim
	 *  that by then belongs to somebody else.
	 *
	 *  [!] A HEALTHY RUNNING STREAM REPORTS THIS TOO, and it is not a
	 *  contradiction.  nn_svc_stream_start() leaves the claim held BY THE
	 *  STREAM until its stop, so there is something held and the caller still
	 *  has no authority over it -- which is exactly what this value says.  It
	 *  is not "nothing is held"; that would be reading it as an ownership
	 *  snapshot, which the note above says it is not. */
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

/* [!] nn_stream_life.h mirrors these because it cannot include this file
 * without a cycle.  A drift is a build failure here rather than a table that
 * decides about values nobody means. */
_Static_assert((int)NN_CLAIM_NONE      == NN_STREAM_CLAIM_NONE,      "claim moved");
_Static_assert((int)NN_CLAIM_CALLER    == NN_STREAM_CLAIM_CALLER,    "claim moved");
_Static_assert((int)NN_CLAIM_RETRYABLE == NN_STREAM_CLAIM_RETRYABLE, "claim moved");
_Static_assert((int)NN_CLAIM_TERMINAL  == NN_STREAM_CLAIM_TERMINAL,  "claim moved");

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

/** Longest board-written explanation carried back with a result. */
#define NN_SVC_DETAIL_MAX 159

/**
 * The two answers every operation gives.  Never collapse them into one.
 *
 * [!] @ref detail IS A COPY, for the same reason the info strings are.  A board
 * adapter cannot print -- it holds no shell instance -- so it writes why a thing
 * failed in its own words and the shared command prints that.  Returning a
 * pointer into the adapter instead would hand the caller something the next
 * command on another console overwrites while it is being printed, which is the
 * bug this contract already had once at the tensor level.  Empty means "no more
 * than the status code says".
 */
struct nn_op_result {
	int     status;  /**< NN_SVC_OK, or a negative code               */
	uint8_t claim;   /**< enum nn_claim -- the caller's authority     */
	char    detail[NN_SVC_DETAIL_MAX + 1];
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

/*
 * Identity and cost of what is loaded.
 *
 * [!] THE STRINGS ARE COPIES, NOT BORROWED POINTERS, AND THAT IS THE WHOLE
 * POINT.  A board fills this under whatever claim it uses and releases before
 * returning -- `nn info` must keep answering while a stream runs, which is
 * exactly when it is worth asking.  A borrowed pointer would then be printed
 * after the claim was gone, and on one board it points straight at the buffer a
 * concurrent `nn model unload` clears.  Copying under the claim is what makes
 * "non-blocking info" and "a name that is still true when printed" both hold.
 *
 * An empty string means "nothing to say", so a caller never has to test for
 * NULL and can never print a pointer it does not own.
 */
/**
 * Whether one section of @ref nn_svc_info actually got filled.
 *
 * [!] THREE VALUES, BECAUSE TWO CANNOT HOLD THREE MEANINGS.  A zero in
 * `arena_used` already means "this board does not report it", so the same zero
 * cannot also mean "a stream owns the claim and I could not look".  Those are
 * different answers -- one is permanent and one clears when the stream stops --
 * and an operator acts on them differently.
 *
 * PER SECTION, because this is precisely where the boards legitimately differ.
 * Two of them copy their identity with no claim at all and so answer everything
 * while a stream runs; the third must take its claim to walk interpreter state,
 * and a stream holds that claim for its whole life.  One flag for the struct
 * would force the strictest board's answer onto the other two.
 */
enum nn_avail {
	NN_AVAIL_OK = 0,      /**< filled and true                                */
	NN_AVAIL_NA,          /**< this board has nothing to report here          */
	NN_AVAIL_WITHHELD,    /**< a stream owns the claim; ask again after it stops */
};

#define NN_SVC_BACKEND_MAX 31
/* [!] THE VERSION IS NOT A NAME AND DOES NOT FIT IN A NAME'S FIELD.  Sharing
 * the backend's 31 characters cut one board's "tflite-micro + CMSIS-NN, no
 * on-board model verify" off at "no on-", which reads as a truncated sentence
 * rather than a shortened one -- and the half it removed was the caveat. */
#define NN_SVC_VERSION_MAX 63
/* [!] LONG ENOUGH FOR THE LONGEST NAME ANY BOARD CAN PRODUCE.  wio's asset
 * store allows 63 characters, and a field that truncates would report two
 * distinct legal models under one prefix -- a name that is wrong rather than
 * merely short. */
#define NN_SVC_MODEL_MAX   63
#define NN_SVC_SOURCE_MAX  79

struct nn_svc_info {
	char     backend[NN_SVC_BACKEND_MAX + 1];  /**< runtime, or ""        */
	char     version[NN_SVC_VERSION_MAX + 1];  /**< its build, or ""      */
	char     model[NN_SVC_MODEL_MAX + 1];      /**< active model, or ""   */
	char     source[NN_SVC_SOURCE_MAX + 1];    /**< where from, or ""     */
	uint32_t arena_bytes;   /**< arena the board reserves                 */
	uint32_t arena_used;    /**< of that, what the model needs (0 = n/a)  */
	uint8_t  model_active;  /**< 0 when nothing is loaded                 */

	/*
	 * What of the above is actually an answer -- see @ref nn_avail.  A board
	 * that fills everything sets all three to NN_AVAIL_OK and nothing changes
	 * for it.  The command prints the reason for anything else rather than
	 * quietly leaving the line out: a shorter report reads like a model with
	 * no tensors, which is a different and wrong fact.
	 */
	uint8_t  avail_identity;  /**< model / source / model_active            */
	uint8_t  avail_runtime;   /**< arena_used, and any live hardware state  */
	uint8_t  avail_tensors;   /**< nn_svc_input / _output / _output_count   */
};

/** Copy @p src into a fixed field, always NUL-terminated, never truncating into
 *  something that could read as a different name.  NULL @p src gives "". */
static inline void nn_svc_str(char *dst, size_t cap, const char *src)
{
	size_t i = 0;

	if (dst == NULL || cap == 0u)
		return;
	if (src != NULL)
		for (; i + 1u < cap && src[i] != '\0'; i++)
			dst[i] = src[i];
	dst[i] = '\0';
}

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
 * Where a board's extra report lines go (issue #101).
 *
 * The same shape as @ref nn_svc_cancel_fn and for the same reason: the thing
 * that can print is a shell instance, which must not cross into a board's port.
 * A plain function and an opaque context say nothing about a shell.
 *
 * [!] LENGTH-BEARING AND NOT VARARGS.  The caller formats its own text.  A
 * printf-shaped callback here would put a formatter in the ABI, and svc/fmt.c
 * implements no %f -- so the first board that wanted a fraction would pull a
 * float formatter into three firmwares at once.  This is also the shape a
 * loaded plugin's report() takes in Step 1b, so there is one convention rather
 * than two.
 *
 * [!] AND FAILURE PROPAGATES.  A sink that has said no has said no; a caller
 * that kept writing would turn one refused line into a run of them and the
 * operator would see only the last.
 *
 * @return the bytes taken, or negative on failure.
 */
typedef int (*nn_svc_write_fn)(void *ctx, const char *s, size_t len);

/**
 * @brief  Emit whatever this board wants to add to `nn info`.
 *
 * Called after the shared lines are printed.  A board with nothing to say does
 * nothing -- silence is a legal answer, unlike the withheld/unavailable
 * distinction the shared fields carry, because these lines are the board's own
 * subject and their absence cannot be mistaken for a fact.
 *
 * [!] THE PORT MUST NOT STORE @p write.  It is valid for the duration of the
 * call and no longer; keeping it would let a later path print from a thread
 * that owns no console.
 *
 * [!] AND IT MUST NOT BE CALLED WITH INTERRUPTS MASKED OR A GATE HELD.  A board
 * that needs state from under a claim SNAPSHOTS it, releases, and then writes:
 * printing is slow, and holding an inference gate across a console write would
 * stall a producer thread for the length of a UART line.
 *
 * @param write  never NULL when called
 * @param ctx    opaque, passed straight back
 */
void nn_svc_info_extra(nn_svc_write_fn write, void *ctx);

/**
 * @brief  Let the board's active decoder describe its own last result.
 *
 * Called by `nn run` and `nn dets` when @ref nn_det_snapshot::external says the
 * boxes are not in the caller's array.  A board with no such decoder does
 * nothing and returns 0 -- and that is not a stub for symmetry: the shared
 * command only calls this when a board has already said the result is
 * elsewhere, so silence here would be a board contradicting itself.
 *
 * Same rules as @ref nn_svc_info_extra: @p write is valid for the call only,
 * must not be stored, and must not be invoked with interrupts masked or an
 * inference gate held.
 *
 * @return 0, or negative when the writer refused.
 */
int nn_svc_report(nn_svc_write_fn write, void *ctx);

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
	/**
	 * [!] THE CLOCK THE CONVERSION USED, or 0 when the board counts real time
	 * directly.  Not decoration: on the board whose core clock is INHERITED
	 * from a bootloader, the cycle counter is the raw measurement and
	 * microseconds are derived from this number -- so a misread clock moves
	 * every figure above while the hardware is fine.  Printing what was
	 * assumed is what makes the reading checkable.
	 */
	uint32_t clock_mhz;
};

/** Invoke @p iters times and report the spread. */
void nn_svc_bench_run(uint32_t iters, struct nn_bench_stats *out,
                      nn_svc_cancel_fn cancel, void *ctx,
                      struct nn_op_result *res);

/* ---- live inference (issue #99) ------------------------------------------
 *
 * One grammar on every board: `nn stream start/stop/stats`.  Until issue #99
 * two of them had a non-blocking worker and the third had a blocking panel
 * `preview`, each implemented by its own command file; the wait is now written
 * once, here above the boards.
 *
 * [!] A STREAM HAS AN IDENTITY, AND THAT IS THE WHOLE REASON FOR `gen`.  A
 * bounded `--frames` run waits on one console while another console -- or a
 * background job on a board that has only one console -- can stop that stream
 * and start a new one.  A waiter that then issued a plain "stop" would tear
 * down a stream it never started, releasing a camera, an NPU or a bus guard
 * that now belongs to somebody else.  So a start hands back a generation, and
 * the two operations that a waiter performs afterwards carry it.
 */

/** How a stream is started.  Tagged for the same reason a model source is: the
 *  bare word would mean different things on different boards. */
struct nn_stream_spec {
	/** Feed a test pattern instead of the sensor.  Refused with
	 *  NN_SVC_ERR_SPEC where the board has no such thing.  A frame count is
	 *  deliberately NOT here: the bounded run is the shared command's own, and
	 *  a field in this struct is a field a board could start using. */
	uint8_t test;
};

/**
 * What a stream has done so far.
 *
 * [!] `frames` IS WHAT THE INFERENCE PATH WAS OFFERED, and the other counts are
 * subsets of it: `skipped` are the offered frames it did not take, `infers` are
 * the ones it finished.  So `frames - skipped >= infers` reads as a sentence.
 *
 * The boards' own counters do NOT agree on this and each converts.  Left alone,
 * two of them reported only the frames they ingested while still reporting every
 * frame they turned away, which printed as "55 in, 269 skipped" -- skipped out
 * of what?  A shared command whose one number means two things on two boards is
 * the thing this contract exists to prevent.
 */
struct nn_stream_stats {
	uint8_t  running;      /**< the stream is still producing               */
	/**
	 * [!] "NEVER DECODED" IS NOT "DECODED NOTHING".  Without this a console
	 * prints "0 faces" for a stream that has not finished its first frame,
	 * which reads as a working detector finding nobody.  One board's own
	 * contract already required the two apart and this keeps that true.
	 */
	uint8_t  last_valid;
	uint32_t frames;
	uint32_t skipped;      /**< delivered but not inferred (busy, stopping) */
	uint32_t infers;
	uint32_t errors;
	/** Of `errors`, split as issue #97 split them: a model whose tensors are
	 *  not the shape the decoder wants means "load a different model", while a
	 *  decoder that refused means "this firmware is wired wrong".  One total
	 *  cannot say which, and the producer has no console to say it on. */
	uint32_t model_errors;
	uint32_t decoder_errors;
	uint32_t last_us;      /**< the most recent inference                   */
	uint32_t elapsed_ms;   /**< since this generation started               */
	int32_t  last_ndet;    /**< valid only when @ref last_valid             */
};

/** Which set of board lines is being asked for. */
enum nn_stream_lines_ctx {
	NN_STREAM_LINES_STARTED = 0,  /**< printed once, after a successful start */
	NN_STREAM_LINES_STATS,        /**< printed under `nn stream stats`        */
};

/** Capacity the shared command provides for one board line, terminator
 *  included, and the hard cap on how many it will ask for.  The cap is not
 *  decoration: it is what stops an adapter bug from printing for ever. */
#define NN_STREAM_LINE_MAX   96
#define NN_STREAM_LINES_MAX  12

/**
 * Start inferring, and return immediately.
 *
 * The board runs its OWN ordered sequence -- one of them must refuse a model
 * that cannot annotate before a stream exists, because a preview that starts
 * and then fails on every frame is a live picture with no explanation -- and
 * unwinds it itself on failure.
 *
 * On success the STREAM owns the board's transient claim until its stop, so the
 * caller's authority is NN_CLAIM_NONE (see the note on that value).  A
 * one-line note for the operator goes in the result's `detail`: one board tells
 * "started" from "re-armed after a lost stream" there, and those are different
 * events.
 *
 * @param gen  set to this stream's generation on success; untouched otherwise
 */
void nn_svc_stream_start(const struct nn_stream_spec *spec,
                         struct nn_op_result *res, uint32_t *gen);

/**
 * How far it has got.  Takes no claim, so it answers WHILE the stream runs --
 * which is when it is worth asking.
 *
 * [!] SAMPLED IN TWO PHASES, AND IT HAS TO BE.  The numbers live in subsystems
 * that take their own locks, so an adapter cannot gather them with interrupts
 * disabled: that is a deadlock, not a slow path.  It samples them outside its
 * critical section, then re-enters and accepts them only if nothing moved.
 *
 * [!] AND WHAT IT CHECKS IS A TRANSITION COUNTER, NOT THE GENERATION AND STATE.
 * Those two are not enough here, for a reason this contract creates: a
 * RETRYABLE stop keeps its generation and puts the lifecycle back where a later
 * stop can claim it, so a sample can straddle an entire failed teardown and
 * find both fields exactly as it left them.  A counter bumped on every
 * transition is what makes this a real seqlock rather than an ABA.
 *
 * @return NN_SVC_OK; NN_SVC_ERR_STALE if the sample moved under it (not an
 *         error -- sample again); NN_SVC_ERR_GEN if @p gen is not the current
 *         stream, which is a DIFFERENT answer from "not running" because it
 *         means somebody else's stream is
 */
int nn_svc_stream_poll(uint32_t gen, struct nn_stream_stats *out);

/**
 * Stop, tear down, and say who may clean up.
 *
 * [!] ONE CALL, NOT A REQUEST AND THEN A STOP.  One board must ask its overlay
 * to stop before it stops the producer, or an ordinary Ctrl+C waits out an
 * inference that had not begun.  Exposing that as two entry points would put an
 * ordering the shared caller can get wrong in place of one it cannot.
 *
 * @param gen  NN_STREAM_GEN_ANY for an operator's `nn stream stop`; a waiter
 *             passes the generation it started, so it can never stop a
 *             successor.  Tested inside the same critical section that claims
 *             the stop transition -- outside it, it would be a stale read.
 *
 * A stop that comes back RETRYABLE must leave the lifecycle stoppable again,
 * keeping its generation.  Parking it mid-transition would turn one moment of
 * lock contention into a stream nothing can ever tear down, which is the very
 * outcome the retryable answer exists to avoid.
 */
void nn_svc_stream_stop(uint32_t gen, struct nn_op_result *res);

/**
 * One line of whatever this board wants to say that the neutral struct has no
 * field for -- an ownership invariant, an ingest deadline, an ordering warning.
 *
 * Indexed rather than returned in a block so the shared command needs exactly
 * one small buffer and still owns no storage.  The rules are all load-bearing:
 *
 *   - the adapter writes ONLY @p buf, never a buffer of its own, so two
 *     consoles can enumerate at the same time without overwriting each other;
 *   - it always NUL-terminates;
 *   - the lines are INDEPENDENTLY SAMPLED and are not one atomic snapshot, so
 *     nothing may be computed across two of them;
 *   - the caller stops at NN_STREAM_LINES_MAX regardless.
 *
 * @return 1 when a line was written, 0 when there are no more, negative on a
 *         failure the caller reports rather than silently ends on
 */
int nn_svc_stream_lines(enum nn_stream_lines_ctx ctx, unsigned index,
                        char *buf, size_t cap);

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
