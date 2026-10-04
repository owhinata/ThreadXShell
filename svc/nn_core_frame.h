/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    nn_core_frame.h
 * @brief   The frame path of the shared nn core: producer, worker and panel,
 *          written once (issue #130, Epic #122 Phase 3b).
 *
 * The other half of svc/nn_core.h.  That file holds the stream's lifecycle and
 * numbers, called from a console; this one holds what happens to each frame,
 * called from the three threads every board now has:
 *
 *   producer  nn_core_on_frame()        once per PART of a camera frame
 *   worker    nn_core_on_infer_done()   after the board's own invoke
 *   panel     nn_core_draw()            and nn_core_on_present_done()
 *
 * NONE OF THEM WAITS.  The board's worker waits for its inference and for the
 * plugin lease -- bounded, in the board -- before it calls in; the panel only
 * ever TRIES the lease; the producer never touches it.  Where a board has to
 * block (a synchronous NPU driver, a CPU inference, a blanking wait), it does so
 * on a thread of its own, around these calls.
 *
 * THE HAND-OVER is svc/nn_handoff.h's word, kept in @ref nn_core_frame and only
 * ever stepped here, one critical section per transition.  The producer writes
 * the model's input in place while the word is FILLING and at no other time;
 * the worker owns it from TAKE until it says DONE.
 *
 * WHAT STAYS WITH THE BOARD: its threads, how they wait and wake, how its camera
 * starts and stops, the order of its stop and where the record boundary falls,
 * the generation a job is published under (grove-vision-ai-v2 samples it on the
 * producer, wio-lite-ai on the worker when it arms -- issue #118 has why each),
 * every counter and how it counts (the account hook), and the admission
 * questions.  svc/nn_active_core.c is unchanged and still knows nothing of a
 * result gate; the decoder is reached through a hook, so no shared decoder is
 * linked into a board that carries none.
 *
 * [!] NOTHING HERE IS BELOW A PLUGIN CALLBACK.  The decode and the paint are the
 * board's hooks, called from here; a painter, a to_frame(), a log or a report
 * sink never calls into this file.  The stack below a veneer is derived from the
 * shipped image and this file must stay off that path.
 *
 * [!] IT OWNS NO STORAGE AND INCLUDES NO BOARD HEADER AND NO tx_api.h.  State is
 * the board's @ref nn_core_frame, properties and hooks its const
 * @ref nn_core_frame_ops; the critical section and the plugin lease are reached
 * through hooks, so a build with no plugin mechanism (wio-lite-ai's `null`
 * backend) passes none and links nothing it does not have.
 */
#ifndef NN_CORE_FRAME_H
#define NN_CORE_FRAME_H

#include <stdint.h>

#include "nn_det_record.h"     /* struct nn_det_snapshot              */
#include "nn_handoff.h"        /* the word                            */
#include "plugin_lease_miss.h" /* enum plugin_lease_who               */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * One board's frame-path state.  Zero-initialised static storage in the board;
 * zero is IDLE with nothing counted.
 */
struct nn_core_frame {
	/** enum nn_handoff_state.  [!] Only through nn_handoff_step(), inside
	 *  the board's critical section -- never written directly. */
	uint8_t  word;
	/** The panel's scratch between nn_core_draw() and on_present_done(). */
	uint8_t  drawn;
	/** First parts the producer was offered since the last reset. */
	uint32_t frame_no;
	/** The frame the job in the input came from: written at BEGIN, read by
	 *  the worker after TAKE -- the word orders the two. */
	uint32_t job_frame;
	/** The frame the plugin's latest result came from.  Written by the worker
	 *  and read by the panel, both under the plugin lease. */
	uint32_t res_frame;
	/** The panel's: how far behind the painted frame its result was. */
	uint32_t drawn_lag;
	/** How many frames the result drawn is behind the frame it is drawn on
	 *  (issue #129): summed and counted per paint, and the worst.  Written
	 *  in the board's critical section; a board reads them in its own. */
	uint32_t lag_sum;
	uint32_t lag_n;
	uint32_t lag_max;
};

/** What became of one part, for the producer to count (nn_core_on_frame()). */
enum nn_core_on_frame {
	/** Not written and not the last part: nothing to count. */
	NN_CORE_FR_NONE = 0,
	/** This part written; more to come. */
	NN_CORE_FR_WROTE,
	/** [!] THE LAST PART OF A FRAME NOTHING WAS WRITTEN FOR: skipped, ONCE PER
	 *  FRAME.  The worker was busy, or the frame was abandoned part way
	 *  through, or the producer joined it mid-frame. */
	NN_CORE_FR_SKIPPED,
	/** The last part written and handed over; the worker has been woken. */
	NN_CORE_FR_HANDED,
	/** The board's prep refused this part: the frame is abandoned (FILLING
	 *  -> WANT) and the next starts from its first part. */
	NN_CORE_FR_ABANDONED,
	/** [!] The word left FILLING while this producer was writing.  Only a
	 *  JOIN can do that, and it may only run once the producer is out -- so
	 *  this is an invariant broken, counted rather than assumed. */
	NN_CORE_FR_RACED,
};

/** What became of one job, for the board's account hook. */
enum nn_core_done {
	/** No plugin: the inference was published as its raw outputs. */
	NN_CORE_DONE_RAW = 0,
	/** Decoded by the plugin and published (accepted or not -- `took`). */
	NN_CORE_DONE_DECODED,
	/** The plugin path was reached without the lease: nothing was decoded
	 *  and nothing published (counted as unheld, not as an error). */
	NN_CORE_DONE_NOT_HELD,
	/** The generation moved during the inference: not decoded, so the
	 *  plugin's own result still describes the record (issue #118). */
	NN_CORE_DONE_RETIRED,
	/** The outputs could not be handed to the decoder (the board says why). */
	NN_CORE_DONE_NO_OUTPUTS,
};

/** What nn_core_draw() did. */
enum nn_core_draw {
	/** No plugin: nothing tried, nothing locked. */
	NN_CORE_DRAW_NONE = 0,
	/** The lease was busy: the frame goes out bare, counted as a miss by the
	 *  lease; nothing locked. */
	NN_CORE_DRAW_MISSED,
	/** The lease was had, the record said no; the frame lock (if the board
	 *  has one) is HELD. */
	NN_CORE_DRAW_DECLINED,
	/** Painted; the frame lock (if any) is HELD, and on_present_done() is
	 *  owed. */
	NN_CORE_DRAW_PAINTED,
};

/**
 * The board, as the frame path sees it.  Const, in the board's .rodata.
 * @p ctx is whatever the board passed to the call that reaches the hook.
 */
struct nn_core_frame_ops {
	/** The board's critical section: returns the posture to restore. */
	unsigned (*cs_enter)(void);
	void     (*cs_exit)(unsigned posture);

	/* ---- producer ------------------------------------------------------ */
	/** Optional.  Show the part, before anything else is done with it.  NULL
	 *  where presentation is upstream of the frame path (both boards today:
	 *  their preview runs with no stream at all). */
	void (*present)(void *ctx, unsigned part, unsigned nparts);
	/** Write @p part of the frame into the model's input.  Called only while
	 *  the word is FILLING.  0 written, non-zero refused (nothing usable was
	 *  written, and the frame is abandoned). */
	int  (*prep)(void *ctx, unsigned part, unsigned nparts);
	/** Wake the worker.  [!] Never waits. */
	void (*infer_start)(void *ctx);

	/* ---- worker -------------------------------------------------------- */
	/** Is a plugin loaded?  The question asked before the lease. */
	int  (*is_plugin)(void *ctx);
	/** Would a publish at @p gen still be taken?  Asked under the lease
	 *  BEFORE the decode (nn_det_record_admits(), issue #118). */
	int  (*admits)(void *ctx, uint32_t gen);
	/** No plugin: publish the inference as its raw outputs; non-zero taken. */
	int  (*publish_raw)(void *ctx, uint32_t gen);
	/** Optional.  Collect the outputs and set the geometry the decode and the
	 *  plugin's to_frame() read.  0 ready, non-zero not decodable. */
	int  (*outputs)(void *ctx);
	/** Decode with the plugin into @p *n.  0 decoded (any count, negative
	 *  BF_ERR_* included), non-zero the entry refused an unleased call. */
	int  (*decode)(void *ctx, int *n);
	/** Publish the plugin's count under @p gen; non-zero taken. */
	int  (*publish)(void *ctx, int n, uint32_t gen);
	/** The board's counting, after the lease is given back.  Its table, its
	 *  counters: @p n is the decoder's count for DECODED, @p took whether the
	 *  record accepted the publish. */
	void (*account)(void *ctx, enum nn_core_done what, int n, int took);

	/* ---- the plugin lease: all NULL with no plugin mechanism ------------ */
	int  (*lease_try)(enum plugin_lease_who who);
	int  (*lease_held)(void);
	void (*lease_give)(void);
	void (*lease_note_unheld)(void);
};

/**
 * What the PANEL is, apart from the rest: its lock, its picture and its
 * painter belong to the display, which may live in another file than the
 * worker's state.  Const, in the board's .rodata.
 */
struct nn_core_panel {
	/**
	 * Optional.  Take the lock the paint and the presentation need, AFTER the
	 * lease (the order is lease, then frame lock -- wio-lite-ai).  [!] LEFT
	 * HELD: nn_core_draw() returns with it held whenever it won the lease, so
	 * the board can present the frame under it and then release it.  NULL
	 * where the caller already holds what it needs (grove-vision-ai-v2's
	 * panel guard).
	 */
	void (*frame_lock)(void *ctx);
	/** Optional.  The board's own "draw at all" (an overlay switched off). */
	int  (*may_draw)(void *ctx);
	/** A snapshot of the decode record.  0 when there is none to read. */
	int  (*record)(void *ctx, struct nn_det_snapshot *snap);
	/** Bind the painter to the picture and let the plugin paint; the board
	 *  keeps its own spent / refused figures. */
	void (*paint)(void *ctx);
};

/* ---- the producer ---------------------------------------------------------- */

/**
 * One part of a frame, on the producer.  BEGINs a fill only at part 0 while the
 * worker wants one, writes through the board's prep only while FILLING, and at
 * the last part HANDs it over and wakes the worker.  Never waits.
 */
enum nn_core_on_frame nn_core_on_frame(struct nn_core_frame *f,
                                       const struct nn_core_frame_ops *o,
                                       void *ctx, unsigned part,
                                       unsigned nparts);

/** The producer drops the frame it is filling, having stopped writing it (a
 *  part it could not use).  @return non-zero if it was filling one. */
int nn_core_frame_abandon(struct nn_core_frame *f,
                          const struct nn_core_frame_ops *o);

/* ---- the worker ---------------------------------------------------------- */

/** IDLE -> WANT.  @return non-zero if armed. */
int nn_core_frame_want(struct nn_core_frame *f,
                       const struct nn_core_frame_ops *o);

/** HANDED -> RUNNING.  @return non-zero if a job was taken; zero is a stale
 *  wake-up, and the worker sleeps again. */
int nn_core_frame_take(struct nn_core_frame *f,
                       const struct nn_core_frame_ops *o);

/** A job taken and ended before nn_core_on_infer_done() (a stop pending, an
 *  invoke refused, a lease that was not had): RUNNING -> WANT, or -> IDLE when
 *  @p more is zero. */
void nn_core_frame_done(struct nn_core_frame *f,
                        const struct nn_core_frame_ops *o, int more);

/**
 * A frame handed over BEFORE the worker's current arm is not this arm's: take
 * it and park again without running it (HANDED -> IDLE, through TAKE and
 * DONE_LAST in one critical section).  For a worker that re-arms itself and
 * throws away whatever arrived between two arms, as wio-lite-ai's drain does.
 * @return non-zero if one was dropped.
 */
int nn_core_frame_discard(struct nn_core_frame *f,
                          const struct nn_core_frame_ops *o);

/**
 * The worker's half after the inference: decode under the lease, publish under
 * @p gen, count, and step the word.  The board's worker has taken the lease
 * (bounded) if it means to decode; this checks it holds it, and gives it back.
 *
 *   no plugin           -> publish_raw (no lease needed)
 *   plugin, not held    -> nothing decoded, nothing published (unheld)
 *   generation moved    -> not decoded (the plugin keeps describing the record)
 *   outputs refused     -> not decoded
 *   otherwise           -> decode, publish, note the result's frame
 *
 * then the lease is given back (if held), the board's account hook counts, and
 * the word goes RUNNING -> WANT (@p more) or -> IDLE.
 */
void nn_core_on_infer_done(struct nn_core_frame *f,
                           const struct nn_core_frame_ops *o, void *ctx,
                           uint32_t gen, int more);

/* ---- the stop ------------------------------------------------------------- */

/** IDLE / WANT / FILLING -> IDLE, once the board has confirmed its producer is
 *  out.  @return non-zero if it joined; zero while a job is handed over or
 *  running, which the caller waits on (or leaves to the worker). */
int nn_core_frame_join(struct nn_core_frame *f,
                       const struct nn_core_frame_ops *o);

/** Clear the frame numbering (and, with @p stats, the lag figures) for a new
 *  stream.  Leaves the word alone. */
void nn_core_frame_reset(struct nn_core_frame *f,
                         const struct nn_core_frame_ops *o, int stats);

/* ---- the panel ------------------------------------------------------------ */

/**
 * Let the plugin paint this frame, if it may.  TRIES the lease and never waits;
 * then the board's frame lock, its own switch, and the record: [!] HOLDING THE
 * LEASE IS NOT A REASON TO DRAW -- the record must hold a result this session
 * published, and it must be the plugin's.  The lease is given back before this
 * returns; the frame lock is not (see @ref nn_core_panel::frame_lock).
 */
enum nn_core_draw nn_core_draw(struct nn_core_frame *f,
                               const struct nn_core_frame_ops *o,
                               const struct nn_core_panel *p, void *ctx);

/** The painted frame is out: count how far behind it its result was.  Nothing
 *  if the last draw did not paint. */
void nn_core_on_present_done(struct nn_core_frame *f,
                             const struct nn_core_frame_ops *o);

#ifdef __cplusplus
}
#endif

#endif /* NN_CORE_FRAME_H */
