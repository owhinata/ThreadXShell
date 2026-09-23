/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Wio Lite AI ThreadX Shell Project
 */
/**
 * @file    nn_camera.h
 * @brief   Camera -> BlazeFace inference glue (owhinata/wio-lite-ai#9 phase 3).
 *
 * Drives a worker thread that repeatedly asks the band stream for one frame,
 * downsamples it straight into the model's input tensor, runs inference and
 * publishes decoded face boxes.
 *
 * WHY THIS IS IN app/ AND NOT port/nn/.  The donor firmware keeps the equivalent
 * glue under port/nn/, and it can: it has a camera subscriber registry, so its glue
 * only ever calls DOWN.  Here the glue claims a band stream, publishes boxes for the
 * display and is driven by a shell command -- that is board integration, and this
 * repository already has the shape for it in app/cam_preview.c, which binds
 * port/camera to port/ltdc from app/.  Nothing under port/ includes an app/ header
 * (checked, not assumed), and keeping it that way is what leaves port/nn exactly
 * what its own header claims to be: a model-agnostic, hardware-free inference API.
 *
 * NO STAGING BUFFER, AND THAT IS THE INTERESTING PART.  The donor stages frames
 * through two 192 KB buffers behind a four-state machine with epoch counters.  Here
 * the ratio does that job instead: inference is ~373 ms and a frame period is ~74 ms,
 * so a fresh frame is always available the moment one is wanted.  What is needed is
 * not buffering but a way to say "fill me one":
 *
 *     worker:   want_frame = 1 -> wait(sem) -> nn_run() -> decode -> publish -> repeat
 *     band cb:  band 0 && want_frame -> latch filling
 *               filling -> downsample this band's rows into nn_input()->data
 *               band 3  -> filling = 0; want_frame = 0; post(sem)
 *
 * While want_frame is 0 the producer does not touch the input tensor at all, and the
 * worker owns it exclusively for the whole inference.  That removes the staging
 * buffers, the state machine, the epoch counters and a 196,608 B memcpy per
 * inference -- but ONLY because of the properties spelled out on nn_camera_stop()
 * and nn_camera_start() below.  The handoff IS the correctness argument here.
 */
#ifndef NN_CAMERA_H
#define NN_CAMERA_H

#include <stdint.h>

#include "blazeface.h"      /* struct bf_det / bf_result / BF_MAX_DET (svc/) */
#include "nn_det_record.h"  /* enum nn_det_kind                              */
#include "nn_svc.h"         /* struct nn_report_capture                      */

#define NNCAM_OK           0
#define NNCAM_ERR_RUNNING (-1)  /**< a stream is already running                 */
#define NNCAM_ERR_NOTRUN  (-2)  /**< nothing to stop                             */
#define NNCAM_ERR_MODEL   (-3)  /**< no model loaded, or it has no input tensor   */
#define NNCAM_ERR_SESSION (-4)  /**< the NN session is held (bench / model load)  */
#define NNCAM_ERR_PSRAM   (-5)  /**< PSRAM down, or OCTOSPI1 held by a retuner    */
#define NNCAM_ERR_BAND    (-6)  /**< the camera would not start a band stream     */
#define NNCAM_ERR_GEOM    (-7)  /**< the model input does not tile onto the bands */
#define NNCAM_ERR_INIT    (-8)  /**< thread / semaphore / mutex creation failed   */
#define NNCAM_ERR_TEARING (-9)  /**< stop: still tearing down -- see below        */
#define NNCAM_ERR_REARM  (-10)  /**< re-arm after a lost stream failed; stop first */
#define NNCAM_ERR_QUANT  (-11)  /**< int8 input without a per-tensor quant scale   */
#define NNCAM_ERR_SHAPES (-12)  /**< the loaded decoder cannot read these outputs  */
#define NNCAM_ERR_NODRAW (-13)  /**< a preview was asked for; the decoder draws none */
#define NNCAM_ERR_DECBUSY (-14) /**< the decoder could not be held still to ask it */

/**
 * The worker thread's (`nn_work`) stack, in DTCM.  Published here since issue #108
 * because the plugin policy in port/nn/nn_svc_wio.c asserts its provisional
 * allowance for a decode callback strictly below it -- an allowance equal to the
 * whole stack is a check that cannot fire (issue #103).  See nn_camera.c for how
 * the number was chosen.
 */
#define NNCAM_STACK_BYTES  3072u

/**
 * The three places a plugin callback stands (issue #108 placed two, #110 added
 * the third and the calls beside all of them).
 *
 * DECODE is on `nn_work`, immediately before the decode in the worker step.
 * DRAW is on the preview thread, at the call that lets the plugin paint -- and
 * at that call rather than in its caller, so the number is the depth the
 * callback inherits and does not depend on what the compiler inlined.
 *
 * [!] SHELL IS THE ONE 3a DID NOT MEASURE, and it is where four of the seven
 * slots are actually called: entry (from `nn model load`), shapes_ok (from the
 * admission both `nn run` and `nn stream start` pass through), report and the
 * parameters.  board.cmake declared the WORKER's allowance for them, which is
 * a bound on the wrong thread's stack -- and the capture buffer `nn run` now
 * carries in its own frame comes out of this one.  Recorded at the deepest of
 * those sites; `nn info` prints it.
 */
enum nn_camera_site {
	NNCAM_SITE_DECODE = 0,
	NNCAM_SITE_DRAW   = 1,
	NNCAM_SITE_SHELL  = 2,
};

/**
 * Record how much of the CALLING thread's stack is already spent here.
 *
 * Grove's probe shape (issue #103): the address of a local, checked to lie inside
 * the identified ThreadX thread's stack, kept as a high-water per site.  Called
 * BEFORE the call it describes, so the number is the depth a callee inherits.
 *
 * [!] THE PROBE PERTURBS THE FRAME IT SITS IN.  It is out of line (noinline), so
 * the value includes the probe's own few bytes -- an over-report, which is the
 * safe direction -- and does not move with inlining.  The same probe, in the same
 * places, stays through Step 3b: removing or reshaping it means measuring again,
 * not reusing 3a's number.
 */
void nn_camera_note_depth(enum nn_camera_site site);

struct nn_camera_stats {
	uint8_t  running;
	uint8_t  holds_guards;   /**< the session + OCTOSPI1 guard are still held  */
	uint8_t  stream_lost;    /**< the band stream died under us (latched)      */
	uint8_t  norm_signed;    /**< input range: 1 = [-1,1], 0 = [0,1]           */
	uint8_t  overlay;
	uint32_t infers;
	uint32_t frames;         /**< complete frames ingested into the tensor     */
	uint32_t skipped;        /**< complete frames that passed while busy       */
	uint32_t errors;
	/**< bands that wrote the tensor mid-inference (owhinata/wio-lite-ai#54) */
	uint32_t raced;
	/**< frame posts discarded by the pre-arm drain (owhinata/wio-lite-ai#54) */
	uint32_t stale_posts;
	/**< DWT cycles of the last band's downsample */
	uint32_t ingest_last_cyc;
	/**< worst band since start -- vs the ~18.5 ms deadline */
	uint32_t ingest_max_cyc;
	uint32_t infer_last_cyc;
	uint32_t elapsed_ms;
	int      ndet;
	/**< bytes already spent at each plugin call site, high-water since start
	 *   (issue #108); 0 until the site has run */
	uint32_t depth_decode;
	uint32_t depth_draw;
	uint32_t depth_shell;
};

/**
 * Claim the band stream and start inferring.
 *
 * Holds the NN session AND psram_acquire_shared() for the WHOLE lifetime of the
 * stream, not per inference.  Both are deliberate:
 *
 *  - The session being held is what makes `nn model load` refuse while streaming,
 *    which matters more than it looks: a reload rebuilds the interpreter and
 *    re-plans the arena, so nn_input()->data MOVES.  (The band callback re-reads
 *    that pointer every frame anyway, rather than caching it across a session.)
 *  - [!] The OCTOSPI1 guard closes a hole camera_streaming() does not cover.
 *    psram_acquire() consults only the camera and the LTDC, so the instant a DCMI
 *    overrun tears the band stream down while the worker is still inside nn_run()
 *    reading the arena, a `psram clk` retune would become legal underneath it.
 *    Per-inference holding would refuse the same commands AND leave a gap between
 *    inferences for exactly that.
 *
 * The documented consequence: START THE PREVIEW BEFORE `nn stream start`.  The guard
 * coexists with an already-armed LTDC scan-out and band DMA, but it refuses a NEW
 * `lcd on` / `lcd reset` / `camera capture` / `camera preview on` while held -- the
 * same behaviour `nn bench` has, for the same reason.
 */
/**
 * @param require_draw  the caller is about to light a PANEL, so a decoder that
 *                      draws nothing is a preview that runs and never
 *                      annotates -- indistinguishable from a broken one.
 *                      `nn run` passes 0: a report-only plugin serves it
 *                      perfectly well.
 *
 * [!] ASKED HERE AND NOWHERE ELSE (issue #110).  It was a pre-check in the
 * stream's service entry, which answered before the NN session was held: a
 * load landing in between changed the decoder after the question, and a
 * timed-out lease there was read as "yes".  Under the session and the lease
 * this is the decoder that will actually run.
 */
int nn_camera_start(int colorbar, int require_draw);

/**
 * Stop inferring, drain, and release the guards.
 *
 * [!] RETURNS NNCAM_ERR_TEARING WITHOUT RELEASING ANYTHING if either side is still
 * in flight: a band callback that never returned (a producer killed by a DCMI
 * overrun) or a worker still inside nn_run().  Releasing early would hand the model
 * and its arena to `nn bench` while something can still write the input tensor.
 * Holding a session nobody can use is recoverable; that is not.  Calling stop again
 * re-checks and completes the release, which is why it is idempotent.
 */
int nn_camera_stop(void);

int nn_camera_running(void);
void nn_camera_stats_get(struct nn_camera_stats *out);

/**
 * One decode's boxes and the diagnostics that belong to them.
 *
 * [!] READ TOGETHER OR NOT AT ALL (issue #97).  The worker publishes both under
 * one lock; a caller that fetched the boxes and then asked the decoder for its
 * "last" numbers was pairing this frame's boxes with whatever had been decoded
 * by the time it got round to printing -- while a stream runs, a different frame.
 */
struct nn_camera_decode {
	/** 0 = nothing published since the model went in.  [!] NOT "this
	 *  session": the result outlives a stop (issue #118) -- @ref current and
	 *  @ref accepted answer that. */
	int              valid;
	int              ndet;   /**< items; @ref kind says what they are         */
	struct bf_result res;    /**< status, peak, pass/kept, threshold APPLIED  */
	/**
	 * [!] CARRIED, NOT RESTATED (issue #110).  This projection used to drop
	 * the record's kind and the service layer asserted NN_DET_CALLER_BOXES
	 * afterwards -- true while that was the only thing a record could hold,
	 * and exactly the sort of statement that outlives the fact behind it.
	 */
	uint8_t          kind;   /**< one of @ref nn_det_kind                     */
	/* Carried from the record in the same snapshot (issue #118) -- see
	 * svc/nn_det_record.h for each. */
	uint8_t          reportable;  /**< the plugin can still describe it      */
	uint8_t          current;     /**< published in the session in force     */
	uint32_t         accepted;    /**< publishes the record has ever taken   */
	uint32_t         epoch;       /**< moves when a model change clears it   */
};

/**
 * The model or its decoder changed: clear the last result (issue #118).
 *
 * Called by the load and the unload whenever what is open is not what was open
 * before -- whatever the operation's status -- and before the new identity is
 * published.  [!] WITH THE RESULT LEASE HELD where the build has one: the
 * order is lease, then the record lock, the same as every other holder.
 */
void nn_camera_record_invalidate(void);

/**
 * Take a coherent snapshot of the last published decode.
 *
 * @param rep   optional.  When given AND the last decode belongs to a loaded
 *              plugin, the plugin is asked to describe it and the bytes land
 *              here.
 * @param ext   optional; the model-dependent part the result was published
 *              with (issue #121), copied under the same lock hold.
 *
 * [!] IT HANDS BACK NO BOXES (issue #116).  It used to take an array to fill;
 * since the resident decoder went there is no publisher on this board that
 * writes one -- a plugin keeps its result and a bare model has none -- so the
 * kind in the snapshot is what a caller routes on, and the boxes were never
 * this function's to give.
 *
 * [!] THE CAPTURE AND THE SNAPSHOT ARE ONE TRANSACTION, which is why they are
 * one call.  The plugin's result is private and it is rewritten by the next
 * decode, so the only moment its account of it is guaranteed to describe THIS
 * snapshot is while the result lease is held -- and taking the lease at one
 * call site and the record at another would leave a decode able to land
 * between them.  The order is lease, then the record lock; nothing takes them
 * the other way round.
 *
 * Asking for a capture therefore costs a bounded wait behind a decode.  The
 * panel does not ask (it passes NULL) precisely so that it never waits.
 * @return non-zero if a snapshot was taken (zero before the first stream start,
 *         when the lock does not exist yet).  A snapshot with `valid == 0` means
 *         nothing has been published since the model went in -- which is NOT the
 *         same as a decode that found nothing, and must not be printed as one.
 *
 * Cumulative counters stay in nn_camera_stats_get(): they are updated outside
 * this lock, and one of the callers polls them every 10 ms without wanting the
 * boxes at all.
 */
int nn_camera_decode_get(struct nn_camera_decode *out,
                         struct nn_report_capture *rep,
                         struct nn_result_extra *ext);

/** Input normalization: 1 = [-1,1], 0 = [0,1] (default).  Applies to float32 and
 *  quantized inputs alike -- a quantized input is the normalized value put through
 *  the tensor's own scale/zero_point. */
void nn_camera_set_norm(int signed_range);
int  nn_camera_get_norm(void);

/** Draw the boxes on the LCD preview (app/cam_preview.c does the drawing). */
void nn_camera_set_overlay(int on);
int  nn_camera_get_overlay(void);

#endif /* NN_CAMERA_H */
