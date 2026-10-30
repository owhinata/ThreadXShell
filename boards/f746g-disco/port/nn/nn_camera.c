/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    nn_camera.c
 * @brief   Live camera -> NN inference glue (owhinata/stm32f746g-disco#81, Epic
 * owhinata/stm32f746g-disco#80). See nn_camera.h.
 *
 * Design (codex-reviewed, owhinata/stm32f746g-disco#81):
 *   - A SYNCHRONOUS step of the camera sink (issue #142; its own push sink
 *     before, like nx_mjpeg.c eth_sink): consume() runs in the camera producer
 *     thread and resizes + converts the RGB565 frame STRAIGHT INTO the model's
 *     input tensor when the worker wants one; the sink puts the pin after its
 *     last step (port/camera/cam_sink.c).
 *     The nearest-neighbour resize reads only WxH sampled source pixels (cheaper
 *     than copying the whole frame), keeping producer-thread load low.
 *     [!] This file used to claim the in-flight count is "always 0".  That is
 *     true of the consume BODY and false at detach, which is the moment an owner
 *     asks: the stop switches the ingest off while the base keeps running, so a
 *     frame can already be in flight across it.  nn_camera_stop() now drains it
 *     instead of assuming it away (issue #72; since issue #142 the ingest is the
 *     nn step of the one camera sink, and what it drains is the step's count).
 *   - NN-input ownership (the BLOCKING codex fix): the sink NEVER writes the
 *     input while the worker is using it.  Since issue #130 that is the shared
 *     hand-over word (svc/nn/nn_handoff.h, stepped only by svc/nn/nn_core_frame.c),
 *     the one grove-vision-ai-v2 and wio-lite-ai use: the producer writes the
 *     input tensor only while the word is FILLING, hands it over, and the worker
 *     owns it from TAKE until the publish is done.  A frame that arrives while
 *     the worker is busy is SKIPPED -- not staged for later.  This replaced two
 *     SDRAM staging buffers (393,216 B together) and the worker's copy out of
 *     them; the input tensor is in the TFLM arena (bank3, CPU-only), so nothing
 *     but the CPU reaches the memory either way.
 *   - Worker priority 18 (below BG-17/CLI-16/GUIX-14/net-12/camera-10): fully
 *     best-effort, so inference never starves the DCMI ring, UI, net, or the CLI
 *     that must deliver `nn stream stop`.  Inference is monolithic (no mid-run
 *     yield), so a lower-than-CLI priority is what guarantees stop reaches us.
 *
 * Split by thread (issue #140): this file is the console side (start / stop /
 * stats / record reads), the caller of attach / detach (the sink's open() and
 * close()), and the owner of the shared state and the frame path's ops table;
 * nn_producer.c is the camera producer thread (consume(), the prep) and
 * nn_worker.c the inference worker.  What they share is nn_camera_priv.h.
 */
#include <stdint.h>

#include "tx_api.h"

#include "nn.h"
#include "nn_camera.h"
#include "nn_camera_priv.h"
#include "nn_det_record.h"
#include "nn_core_frame.h"   /* the shared frame path and its hand-over word (#130) */
#include "nn_sess_release.h" /* when the session may go back (#130)               */
#include "camera.h"          /* enum camera_res / CAM_RES_*                         */
#include "cam_own.h"         /* the owner lifecycle (issue #72)                        */
#include "cam_sink.h"        /* the nn step of the camera sink (issue #142)            */
#include "cam_holders.h"     /* struct cam_holder: nn's place on the camera (#142)     */
#include "frame_pipeline.h"  /* struct frame_desc */

#include "stm32f7xx_hal.h"   /* HAL_GetTick */

#if defined(CONFIG_NN_BACKEND_TFLM)
#include "nn_active.h"       /* the plugin entries (issue #159)                    */
#include "nn_panel.h"        /* the GUIX draw's figures, armed with the stream     */
#include "plugin_lease.h"    /* the one lock into the plugin                       */
#endif

/* Float32 input normalization.  BlazeFace's model card says [-1,1], but its ST
 * config says rescale 1/255 -> [0,1] -- and [0,1] is what actually detects faces
 * on hardware (maxscore 288 vs 11), so ST retrained with [0,1].  Default [0,1];
 * `nn norm 1` flips to [-1,1] at runtime for other models. */
#ifndef NNCAM_NORM_SIGNED
#define NNCAM_NORM_SIGNED 0
#endif

/* NNCAM_WORKER_PRIORITY is in nn_camera_priv.h (the worker logs it), and
 * NNCAM_WORKER_STACK in nn_camera.h (`nn stream stats` prints it). */
#if defined(CONFIG_NN_BACKEND_TFLM)
/* The decode slot's plugin allowance (board.cmake, issue #159) strictly below
 * the thread it runs on. */
_Static_assert(F746_PLUGIN_STACK_NN_WORK < NNCAM_WORKER_STACK,
               "the decode allowance must be below the nn worker's stack");
#endif
/* Teardown budgets (ticks, WALL CLOCK; 1 tick = 1 ms).  The sink drain is short
 * because a healthy consume() is one resize away from its put; the worker settle
 * keeps the 3 s this always allowed, since it may have to outlast a whole
 * inference.  Both are wall clock rather than a count of sleeps: counting
 * iterations burns the budget instantly once the sleeps stop sleeping, and would
 * then report a healthy worker as stuck (issue #65). */
#define NNCAM_DRAIN_TICKS     100u        /* sink pin -> release                    */
#define NNCAM_SETTLE_TICKS    3000u       /* worker parks (may span an inference)   */

/*
 * WHO MAY TOUCH THE INPUT TENSOR (issue #130): the shared hand-over word, stepped
 * only by svc/nn/nn_core_frame.c.  It replaced two staging buffers and their
 * FREE/FILLING/READY/RUNNING states -- the worker used to copy a READY stage
 * into the input, so the producer never needed the input itself.  Now it writes
 * the input directly, while the word says FILLING and at no other time: the
 * interpreter may reuse that memory for intermediates while a job RUNs.
 */
struct nn_core_frame nncam_frame;
/*
 * The generation the frame in the input was prepared under.  Sampled by the
 * PRODUCER at the start of its prep, under nncam_lock (issue #118), written
 * while the word is FILLING and read by the worker only after its TAKE -- the
 * word orders the two.  [!] On the producer and not at the worker's take: a
 * frame handed over before a base detach would otherwise be taken AFTER the
 * detach's boundary and published into the next attach looking current, which
 * is what the old stages' "drop READY at close" prevented.
 */
uint32_t     nncam_job_gen;

static TX_THREAD    nncam_thread;
static UCHAR        nncam_stack[NNCAM_WORKER_STACK];
TX_MUTEX     nncam_lock;           /* guards the record, the epoch, the session */
TX_SEMAPHORE nncam_sem;            /* wakes the worker: a hand-over, or a stop   */

struct nn_model  *nncam_model;

/* enabled intent (nn stream start=1, stop=0) */
volatile int nncam_run;
/* worker is in the run loop (set by worker) */
volatile int nncam_active;
/* base detached (close cb): paused, not stopped */
static volatile int nncam_producer_dead;
/* the AI subscriber holds the nn session */
static volatile int nncam_holds_session;
/* A step drain (issue #142; a sink drain before it) since this stream's start
 * returned DONE: the producer is confirmed
 * out of the input tensor (issue #130).  Cleared by the start, set only by a stop
 * whose drain succeeded; read and the hold cleared together under nncam_lock. */
static int          nncam_sink_drained;
/* worker/objects created once */
static int          nncam_created;

/* The `nn stream` lifecycle (issue #72), serialised by cam_own.h.  It guards the
 * nn STEP of the camera sink (issue #142; its own sink before): which entry points
 * may enable it, and the interval in which a stop has disabled it and is watching
 * its count fall to zero.  The three volatile
 * flags above stay, and describe something else -- the worker and the nn session,
 * which deliberately OUTLIVE a stop that returned -2 (the worker releases the
 * session itself on its way out).  Both are consulted at a start; neither
 * subsumes the other. */
volatile enum cam_own_state nncam_own;
/* enum camera_res hint (display only, owhinata/stm32f746g-disco#100) */
static uint8_t      nncam_res;
/*
 * [!] FRAMES SINCE THE LOGICAL STREAM START, WHICH IS NOT WHAT nnstat.frames
 * COUNTS (issue #99).  nnstat is cleared by nncam_session_reset(), and that runs
 * on every pipeline ATTACH -- so it restarts when the base capture starts, and
 * again after an overrun re-attach.  A bounded `nn stream start --frames n`
 * cannot be built on a number that goes backwards underneath it, and subtracting
 * a baseline in the shared command does not help for the same reason.  This one
 * is reset only where a stream logically begins, and is monotonic across base
 * re-attachments.
 */
uint32_t     nncam_gen_frames;
/* [!] AND ITS SIBLINGS, for the same reason.  Reporting a per-GENERATION frame
 * count beside per-ATTACH inference and drop counts produced stats that do not
 * add up -- "60 frames in, 36 infers, 0 skipped" was the first bench run.  All
 * four move together or none of them mean anything. */
uint32_t     nncam_gen_infers;
uint32_t     nncam_gen_errors;
uint32_t     nncam_gen_drops;
/* Two of these (nncam_gen_errors, and nnstat.errors) have two writers: see
 * nncam_error() in nn_camera_priv.h (issue #138). */
/* Of nncam_gen_errors: decodes the resident decoder refused as a wiring fault
 * (issue #130 step 6c, nn_decode_count.h -- BF_ERR_MODEL is a result here, not
 * one of these).  Written by the worker only. */
uint32_t     nncam_gen_decoder_errors;
/* Of nncam_gen_errors: a LOADED PLUGIN's BF_ERR_MODEL -- its model and its
 * decoder do not match (issue #159; nn_decode_count_plugin_of()).  The worker
 * only. */
uint32_t     nncam_gen_model_errors;
/* The running stream is a `nn run`'s: its decodes are not counted (it reports
 * its own result).  Written by nn_camera_start() before nncam_run is raised. */
int          nncam_oneshot;

/* nn's place among the camera's holders (issue #142): a `nn stream start` or
 * `nn run` holds the camera open -- starting it, and holding the operator's
 * place too, when nothing streams -- and the stop lets go of nn's place only,
 * so the camera keeps streaming for the operator.  What the start's join added
 * is kept for the one undo a failed start needs. */
static const struct cam_holder nncam_holder = { "nn" };

/* Session generation, bumped on every (re)attach + on a base detach.  An in-flight
 * prep that spans a session boundary is abandoned instead of handing over a stale frame. */
volatile uint32_t nncam_epoch;

/* Release the nn session iff the AI subscriber still holds it AND the sink drain
 * has confirmed the producer out of the input tensor (issue #130 -- the producer
 * writes the arena the session guards, see nn_sess_release.h).  Exactly once per
 * `nn stream` lifetime: the decision and the clearing of the hold are one hold of
 * nncam_lock, so of the stop and the worker only one gives it back.  Called by
 * the worker as it leaves its run loop and by nn_camera_stop() after it waited
 * for the worker.  A base detach (close) NEVER releases -- the session owner is
 * the AI enabled intent (contract owhinata/stm32f746g-disco#100.4). */
void nncam_release_session(enum nn_sess_who who)
{
	int release;

	tx_mutex_get(&nncam_lock, TX_WAIT_FOREVER);
	release = nn_sess_may_release(nncam_sink_drained, nncam_holds_session, who);
	if (release)
		nncam_holds_session = 0;
	tx_mutex_put(&nncam_lock);
	if (release)
		nn_session_release();
}

/*
 * How deep each thread's stack already was where a plugin was CALLED (issue
 * #159; wio-lite-ai's form since #108 / #126).  High-waters, written by
 * nn_camera_note_depth_at() on the thread that calls -- each site has one
 * writer thread -- and read by `nn stream stats`.  decode / draw are reset when
 * a stream starts; shell is not (its deepest site is `nn model load`).  Only
 * ever written with a real measurement, so 0 means "not reached yet".
 */
static uint32_t nncam_depth_decode, nncam_depth_draw, nncam_depth_shell;

/*
 * The geometry of the frames the nn step is attached to, (w << 16) | h, one
 * word so a reader on another thread never pairs one attach's width with
 * another's height.  Written by nncam_open() (under the camera lock); read by
 * a plugin's to_frame() on the worker.  0 before the first attach.
 */
static volatile uint32_t nncam_geom;

/*
 * A stream starts or stops: move the generation under the lock, so an in-flight
 * decode from the previous session lands nowhere.  [!] THE RESULT STAYS (issue
 * #118).
 *
 * [!] UNDER THE PLUGIN LEASE WHEN ONE CAN BE HAD (issue #159, wio-lite-ai's
 * #118).  The worker asks "is my generation still current?", decodes and
 * publishes, all under the lease -- so a boundary taken under it too lands
 * either before the question (no decode runs, and the plugin's private result
 * still describes the record) or after the publish.  Never in between, where a
 * decode would rewrite the plugin's result for a publish then dropped.  The
 * wait is the lease's one bound; if it expires the boundary is taken anyway --
 * a stop does not wait on a plugin -- and the record's reportable rule covers
 * the decode that may then run.  Lease before the record lock, as everywhere.
 * (The attach and the detach move the generation under the camera lock, where
 * no wait is allowed; they keep the plain boundary.)
 */
static void nncam_record_boundary(void)
{
#if defined(CONFIG_NN_BACKEND_TFLM)
	int leased = plugin_lease_take();
#endif

	tx_mutex_get(&nncam_lock, TX_WAIT_FOREVER);
	nn_det_record_boundary(&nncam_rec);
	tx_mutex_put(&nncam_lock);
#if defined(CONFIG_NN_BACKEND_TFLM)
	if (leased)
		plugin_lease_give();
#endif
}

/* See nn_camera.h. */
void nn_camera_note_depth_at(enum nn_camera_site site, uintptr_t sp)
{
	TX_THREAD *t = tx_thread_identify();
	uintptr_t  lo, hi;
	uint32_t   used;
	uint32_t  *hw = (site == NNCAM_SITE_DRAW)  ? &nncam_depth_draw :
	                (site == NNCAM_SITE_SHELL) ? &nncam_depth_shell :
	                                             &nncam_depth_decode;

	if (t == NULL)
		return;                     /* not on a thread; nothing to say */
	lo = (uintptr_t)t->tx_thread_stack_start;
	hi = lo + (uintptr_t)t->tx_thread_stack_size;
	if (sp < lo || sp > hi)
		return;                     /* not this thread's stack after all */
	used = (uint32_t)(hi - sp);
	if (used > *hw)
		*hw = used;
}

void nn_camera_frame_geom(uint16_t *w, uint16_t *h)
{
	uint32_t g = nncam_geom;

	if (w != NULL)
		*w = (uint16_t)(g >> 16);
	if (h != NULL)
		*h = (uint16_t)(g & 0xFFFFu);
}

/* Model input geometry, latched at start from the input tensor. */
uint16_t nncam_in_w, nncam_in_h, nncam_in_c;
uint32_t nncam_in_bytes;   /* what the preprocess writes (<= the tensor's) */
uint8_t  nncam_in_dtype;   /* enum nn_dtype of the model input */


/* Latest detections (BlazeFace), guarded by nncam_lock; read via nn_camera_dets_get(). */
/*
 * The published decode: boxes AND the diagnostics that describe them, plus the
 * session generation that keeps a decode from a session that has ended out of a
 * live one.  The rules live in svc/nn/nn_det_record.c so they can be tested -- the
 * ordering they guard against cannot be produced deterministically on hardware --
 * and the storage and the lock (nncam_lock) are ours.
 */
struct nn_det_record nncam_rec;

/* Float32 input normalization, runtime-tunable (`nn norm`) to settle the [-1,1] vs
 * [0,1] ambiguity on hardware without reflashing. */
int nncam_norm_signed = NNCAM_NORM_SIGNED;

/* The per-attach counters; the type and who writes each are in nn_camera_priv.h. */
struct nncam_stat nnstat;

/* ---- frame-pipeline sink (synchronous copy) ------------------------------- */

/* Reset per-session state when the step is armed (nncam_open: the base attaching
 * the camera sink with the step enabled, or the start enabling the step while the
 * base is attached -- issue #142).  Bumps the epoch so an in-flight ingest from a prior session
 * is abandoned, and clears producer_dead so a stale base-detach flag does not instantly
 * re-pause the fresh session.  [!] The hand-over word is NOT touched: an overrun
 * auto-recovery re-attaches (this reset) while the AI stays enabled, so the
 * prio-18 worker may be inside a job, and a frame handed over before the detach is
 * the worker's to take -- its publish is refused by the generation it was prepared
 * under (contract owhinata/stm32f746g-disco#100.3, same discipline as nncam_close). */
static void nncam_session_reset(void)
{
	tx_mutex_get(&nncam_lock, TX_WAIT_FOREVER);
	nncam_producer_dead = 0;
	/* Move the generation, so an inference still running under the old session
	 * cannot publish into this one.  [!] The last result STAYS (issue #118):
	 * `nn dets` reads it until a model change clears it, and what a session
	 * produced is counted from a base taken after this boundary. */
	nn_det_record_boundary(&nncam_rec);
	nncam_epoch++;
	tx_mutex_put(&nncam_lock);
	memset(&nnstat, 0, sizeof nnstat);
	nnstat.start_tick = HAL_GetTick();
	while (tx_semaphore_get(&nncam_sem, TX_NO_WAIT) == TX_SUCCESS)
		;
}

static int nncam_open(void *ctx, enum frame_format fmt, uint16_t w, uint16_t h)
{
	(void)ctx;

	if (fmt != FRAME_FMT_RGB565)
		return -1;                          /* base raster is RGB565 only          */
	/* [!] Not on a stream nn lost (issue #142).  A confirmed end (the overrun
	   recovery gave up) emptied the camera's holders; if the operator restarts
	   the camera, nn does not quietly resume -- it resumes when nn itself is
	   restarted (camera_stream_join() clears the mark).  Called under the camera
	   lock on an attach, which is what makes the unlocked read settled. */
	if (camera_stream_lost(&nncam_holder))
		return -1;

	/* Record the actual base geometry for the stats display (the input adapts to
	   whatever resolution the base publishes -- the model input is fixed 128x128). */
	nncam_res = (w <= 160u) ? (uint8_t)CAM_RES_QQVGA
	          : (w <= 320u) ? (uint8_t)CAM_RES_QVGA
	                        : (uint8_t)CAM_RES_VGA;
	nncam_geom = ((uint32_t)w << 16) | (uint32_t)h;
	nncam_session_reset();                  /* fresh session on (re)attach          */
	return 0;
}

/* ---- the frame path's hooks (svc/nn/nn_core_frame.h, issue #130) ------------- */

/* nncam_cs_enter / nncam_cs_exit are in nn_camera_priv.h (both threads take
 * them); nncam_prep / nncam_infer_start in nn_producer.c; nncam_publish /
 * nncam_account in nn_worker.c. */

/* Is a plugin in force?  The question the frame path asks before the lease
 * (issue #159); with none, the resident decoder publishes (nncam_publish). */
static int nncam_is_plugin(void *ctx)
{
	(void)ctx;
#if defined(CONFIG_NN_BACKEND_TFLM)
	return nn_active_is_plugin();
#else
	return 0;                           /* no plugin mechanism in this backend */
#endif
}

/* The plugin's admission / decode / publish and the lease are tflm's only (the
 * other backends cannot load a container), and stay NULL elsewhere, as in
 * wio-lite-ai's null build: the frame path never reaches them while is_plugin()
 * says no. */
const struct nn_core_frame_ops nncam_frame_ops = {
	.cs_enter    = nncam_cs_enter,
	.cs_exit     = nncam_cs_exit,
	.present     = NULL,   /* the GUI preview is its own subscriber */
	.prep        = nncam_prep,
	.infer_start = nncam_infer_start,
	.is_plugin   = nncam_is_plugin,
	.publish_raw = nncam_publish,
	.outputs     = NULL,   /* nn_active_decode() reads them itself */
	.account     = nncam_account,
#if defined(CONFIG_NN_BACKEND_TFLM)
	.admits            = nncam_admits_hook,
	.decode            = nncam_decode,
	.publish           = nncam_publish_hook,
	.lease_try         = plugin_lease_try,
	.lease_held        = plugin_lease_held,
	.lease_give        = plugin_lease_give,
	.lease_note_unheld = plugin_lease_note_unheld,
#endif
};

/* The nn step was disarmed: the base detached the camera sink (base capture
 * stopped / DCMI overrun / cascade), or the stop disabled the step (issue #142;
 * the stop's unsubscribe used to call this the same way).  On a base detach it is
 * a PAUSE, not a stop (contract owhinata/stm32f746g-disco#100.2/.4): keep the AI enabled
 * (nncam_run) and the nn session held -- the session owner is the `nn stream` intent,
 * released only by nn_camera_stop().  Bump the epoch so an in-flight ingest is
 * abandoned, move the generation so a frame already handed over is not published
 * after a later re-attach, and wake the worker to re-evaluate.  Non-blocking and no
 * camera API re-entry, so it is safe under the camera lock. */
static void nncam_close(void *ctx)
{
	(void)ctx;
	tx_mutex_get(&nncam_lock, TX_WAIT_FOREVER);
	nncam_producer_dead = 1;
	nncam_epoch++;                          /* an in-flight prep is abandoned        */
	/* Move the generation: that is what stops a frame handed over or running --
	 * the word is deliberately left alone, a delivery may be in flight across
	 * this detach -- from publishing after the pause, because it was prepared
	 * under the generation this ends (issue #130; it used to drop READY stages
	 * here).  The boxes stay in the record (issue #118); a live picture asks
	 * whether they are CURRENT, and after this boundary they are not. */
	nn_det_record_boundary(&nncam_rec);
	tx_mutex_put(&nncam_lock);
	(void)tx_semaphore_put(&nncam_sem);     /* wake worker (idles until re-attach)  */
}

/* The nn step of the one camera sink (issue #142); src/main.c registers it after
 * the display step.  nncam_consume() is in nn_producer.c.  (nncam_holder, nn's
 * place among the camera's holders, is defined further up.) */
static const struct cam_step_ops nncam_step_ops = {
	.open    = nncam_open,
	.consume = nncam_consume,
	.close   = nncam_close,
};

struct cam_sink_step nncam_step = {
	.name = "nn",
	.ops  = &nncam_step_ops,
	.ctx  = NULL,
};

/* ---- the inference worker is in nn_worker.c -------------------------------- */

/* Open the model + latch its input geometry, bounds-checked against the input
 * tensor the producer writes into.  Returns 0, -3 (model open) or -4 (geometry).
 * Shared by both start paths. */
static int nncam_open_model(void)
{
	struct nn_tensor *in;

	if (nn_model_open(&nncam_model) != 0)
		return -3;
	/* No model loaded (e.g. the stedgeai_reloc backend before `nn model load`): the
	 * single choke point that rejects `nn run`/`nn stream` cleanly. */
	if (nn_input_count(nncam_model) == 0)
		return -3;
	in = nn_input(nncam_model, 0);
	if (!in || in->ndim < 3)
		return -4;
	nncam_in_h = in->dims[1];
	nncam_in_w = in->dims[2];
	nncam_in_c = (in->ndim >= 4) ? in->dims[3] : 1;
	nncam_in_dtype = in->dtype;
	/*
	 * [!] THE BOUND IS THE TENSOR ITSELF (issue #130).  The preprocess writes
	 * H x W x C elements of 4 bytes (FLOAT32) or 1 (anything else) straight into
	 * the input, so that is what the tensor must hold.  It used to be bounded by
	 * the staging buffers (128 x 128 x 3 x 4), which are gone; what is left to
	 * protect is the arena around the input.
	 */
	{
		uint64_t need = (uint64_t)nncam_in_w * nncam_in_h * nncam_in_c *
		                ((nncam_in_dtype == NN_DTYPE_FLOAT32) ? 4u : 1u);

		if (in->data == NULL || need == 0u || need > (uint64_t)in->bytes)
			return -4;                      /* the input cannot hold the frame      */
		nncam_in_bytes = (uint32_t)need;
	}
	return 0;
}

static volatile int nncam_creating;     /* one thread owns the one-time object create */

/* Create the worker thread, mutex and semaphore exactly once, shared by both start
 * paths.  Serializes the one-time creation across concurrent shells with a PRIMASK
 * init latch (no pre-created mutex -- same idiom as nn_model_open): exactly one thread
 * creates the objects, any other waits for nncam_created before returning.  Idempotent
 * once created (lock-free fast path).  Returns 0 or -5. */
static int nncam_create_objects(void)
{
	for (;;) {
		uint32_t primask = __get_PRIMASK();
		int owner = 0;

		__disable_irq();
		if (nncam_created) {
			__set_PRIMASK(primask);
			return 0;
		}
		if (!nncam_creating) { nncam_creating = 1; owner = 1; }
		__set_PRIMASK(primask);

		if (owner)
			break;
		tx_thread_sleep(1);             /* another thread is creating; wait + retry */
	}

	/* This thread owns the create. */
	if (tx_mutex_create(&nncam_lock, "nncam", TX_INHERIT) != TX_SUCCESS) {
		nncam_creating = 0;
		return -5;
	}
	if (tx_semaphore_create(&nncam_sem, "nncam", 0) != TX_SUCCESS) {
		tx_mutex_delete(&nncam_lock);
		nncam_creating = 0;
		return -5;
	}
	if (tx_thread_create(&nncam_thread, "nn-worker", nncam_entry, 0,
	                     nncam_stack, sizeof nncam_stack,
	                     NNCAM_WORKER_PRIORITY, NNCAM_WORKER_PRIORITY,
	                     TX_NO_TIME_SLICE, TX_AUTO_START) != TX_SUCCESS) {
		tx_semaphore_delete(&nncam_sem);
		tx_mutex_delete(&nncam_lock);
		nncam_creating = 0;
		return -5;
	}
	nncam_created = 1;                   /* publish (waiters spin on this) */
	nncam_creating = 0;
	return 0;
}

/* ---- public API ----------------------------------------------------------- */

int nn_camera_start(enum camera_res res, int oneshot)
{
	enum cam_own_start act;
	struct cam_holders_ticket tk;
	int rc;

	/* input adapts to the base geometry (owhinata/stm32f746g-disco#100) */
	(void)res;
	/* Claim the lifecycle first (issue #72).  Everything below happens inside
	   CAM_OWN_STARTING, so a concurrent `nn stream stop` is refused rather than
	   interleaved -- a stop that ran a whole teardown between this claim and the
	   subscribe below would leave both commands reporting success. */
	act = cam_own_start_take(&nncam_own);
	if (act != CAM_OWN_START_GO)
		return -2;                          /* running, or a teardown owns the sink */

	/* The flags are still tested, and they are NOT the same question: the
	   lifecycle guards the sink, while these say whether the worker and the nn
	   session from a previous stream are still winding down.  A stop that
	   returned -2 leaves the lifecycle clear and these set. */
	if (nncam_run || nncam_active || nncam_holds_session) {
		cam_own_start_finish(&nncam_own, 0);
		return -2;                          /* still tearing down                   */
	}

	/* Claim the single inference session first: refused (-6) if `nn bench` or a
	 * stream is using the non-reentrant singleton model.  The session owner is this
	 * `nn stream` enable; it is released only by nn_camera_stop() / the worker's
	 * run-loop exit, NEVER by a base detach (contract owhinata/stm32f746g-disco#100.4).
	 *
	 * [!] BEFORE THE MODEL IS LOOKED AT (issue #131).  The model's shape is
	 * latched below and the worker runs on it; with the session taken after,
	 * an `nn model unload` on the other console could empty the model between
	 * the two and this start would succeed on nothing -- every frame an error,
	 * `nn run` a timeout.  Under the session no load or unload can run, so what
	 * nncam_open_model() reads stays true for the stream.  If this is the
	 * first nn command after boot, its open is the one that adopts the
	 * built-in model -- under the session, and a load or unload racing it
	 * waits on the open latch and then finds the session taken.  Otherwise the
	 * open builds nothing. */
	rc = nn_session_try_acquire();
	if (rc != 0) {
		cam_own_start_finish(&nncam_own, 0);
		/* `sdram test` holding .sdram.ai is its own answer (issue #152):
		 * passed through so the caller can say so as busy. */
		return (rc == NN_ERR_SCRATCH_HELD) ? NN_ERR_SCRATCH_HELD : -6;
	}

	rc = nncam_open_model();                /* model + input geometry (bounds-checked) */
	if (rc != 0) {
		nn_session_release();
		cam_own_start_finish(&nncam_own, 0);
		return rc;
	}
#if defined(CONFIG_NN_BACKEND_TFLM)
	/*
	 * [!] CAN THE PLUGIN READ THIS MODEL, AND WILL IT DRAW (issue #159,
	 * wio-lite-ai's #110 / #116)?  A plugin was shipped WITH the model it reads;
	 * finding out per frame that it cannot is a stream that runs and silently
	 * never annotates.  Asked only with a plugin in force: in Step 4b the
	 * resident decoder is still the no-plugin fallback, and it answers for
	 * itself as it always has (BF_ERR_MODEL is its "not a detector").  The draw
	 * question is a STREAM's -- a `nn run` reports, it does not draw.  Under the
	 * lease like every entry into a plugin, and a timeout is not a yes.
	 */
	if (nn_active_is_plugin()) {
		int shapes, draws;

		if (!plugin_lease_take()) {
			nn_session_release();
			cam_own_start_finish(&nncam_own, 0);
			return NNCAM_ERR_DECBUSY;
		}
		shapes = nn_active_shapes_ok(nncam_model);
		draws  = oneshot ? 1 : nn_active_can_draw();
		plugin_lease_give();
		/* Compared against 1: NN_ACTIVE_NOT_HELD is negative. */
		if (shapes == NN_ACTIVE_NOT_HELD || draws == NN_ACTIVE_NOT_HELD) {
			nn_session_release();
			cam_own_start_finish(&nncam_own, 0);
			return NNCAM_ERR_DECBUSY;
		}
		if (shapes != 1 || draws != 1) {
			nn_session_release();
			cam_own_start_finish(&nncam_own, 0);
			return (shapes != 1) ? NNCAM_ERR_SHAPES : NNCAM_ERR_NODRAW;
		}
	}
#endif
	if (nncam_create_objects() != 0) {
		nn_session_release();
		cam_own_start_finish(&nncam_own, 0);
		return -5;
	}
	nncam_holds_session = 1;

	/* Enable the AI BEFORE enabling the step so a frame delivered the instant it
	 * is armed is ingested.  camera_sink_step_enable() arms the nn step now iff the
	 * base is running RGB565 (calls nncam_open -> session reset); otherwise the
	 * step stays enabled + idle and is armed at the next `camera stream start`.  A
	 * non-zero rc is a hard failure (the sink could not be subscribed / the step is
	 * still busy / its open refused) -> unwind. */
	/*
	 * [!] A BOUNDARY OF ITS OWN, BEFORE THE SUBSCRIBE (issue #118).  The
	 * attach below is one only if the base is running; with it idle the
	 * attach comes later, and the caller samples its accepted-count base
	 * after this call returns.  Moving the generation here makes "everything
	 * accepted after the base is this session's" true on both paths.  The
	 * worker is parked (checked above), so nothing is in flight to drop.
	 */
	nncam_record_boundary();

	/*
	 * [!] THE WORD IS PARKED HERE, BEFORE nncam_run IS RAISED (issue #130).  A
	 * frame the last session handed over and never ran is dropped (TAKE +
	 * DONE_LAST, as wio-lite-ai's worker drops one at its first step), and a word
	 * it left armed or filling is joined.  Both are safe only because nothing
	 * else can touch the word now: the worker is parked (checked above), and the
	 * lifecycle said IDLE, which it reaches only through a stop whose step drain
	 * succeeded -- so the nn step is disarmed and the producer is not in it.
	 */
	(void)nn_core_frame_discard(&nncam_frame, &nncam_frame_ops);
	(void)nn_core_frame_join(&nncam_frame, &nncam_frame_ops);

	/* Nothing of this stream has been drained yet (issue #130). */
	tx_mutex_get(&nncam_lock, TX_WAIT_FOREVER);
	nncam_sink_drained = 0;
	tx_mutex_put(&nncam_lock);

	/* Hold the camera open (issue #142): join a running stream, or start it
	 * -- in the current mode, which must stream RGB565 (D3) -- holding the
	 * operator's place too.  Before the step is enabled, so a base this starts
	 * attaches with the step still off and the enable below opens it. */
	rc = camera_stream_join(&nncam_holder, &tk);
	if (rc != 0) {
		nncam_holds_session = 0;
		nn_session_release();
		cam_own_start_finish(&nncam_own, 0);
		/* [!] TRANSLATED, NOT PASSED THROUGH (review F4): the camera's
		 * CAM_ERR_* overlap this function's own codes (CAM_ERR_HAL is -2,
		 * which the adapter reads as "already running"). */
		return (rc == CAM_ERR_FORMAT) ? -9 : -10;
	}

	nncam_oneshot = oneshot ? 1 : 0;
	nncam_run = 1;
	rc = camera_sink_step_enable(&nncam_step);
	if (rc != 0) {
		nncam_run = 0;
		/* Undo the join -- exactly what it added: the operator's place goes
		 * only if this start took it. */
		(void)camera_stream_leave(&nncam_holder, &tk);
		nncam_holds_session = 0;
		nn_session_release();
		cam_own_start_finish(&nncam_own, 0);
		return rc;
	}
	/* [!] HERE, and nowhere else (issue #99): a refused start must not clear
	   it, and neither must a re-attach part way through a running stream. */
	nncam_gen_frames = 0u;
	nncam_gen_infers = 0u;
	nncam_gen_errors = 0u;
	nncam_gen_drops  = 0u;
	nncam_gen_decoder_errors = 0u;
	nncam_gen_model_errors   = 0u;
	/* The call-site depths a stream measures (issue #159): the decode and
	 * draw sites are this stream's; the shell's is kept from boot. */
	nncam_depth_decode = 0u;
	nncam_depth_draw   = 0u;
#if defined(CONFIG_NN_BACKEND_TFLM)
	/* The GUIX draw's figures and the lease misses are this stream's too --
	 * armed here, not at a stop, so the stats right after a stop still
	 * describe the stream that ended. */
	nn_panel_arm();
#endif
	cam_own_start_finish(&nncam_own, 1);    /* STARTING -> RUNNING                  */
	return 0;
}

/* Wait (bounded, wall clock) for the worker to leave its run loop; non-zero if
 * it parked.  Kept apart from the sink drain because it answers a different
 * question: the sink drain says the producer is out of our staging state, this
 * says the worker is out of the model. */
static int nncam_settle_worker(void)
{
	ULONG start = tx_time_get();

	while (nncam_active && (tx_time_get() - start) < (ULONG)NNCAM_SETTLE_TICKS)
		tx_thread_sleep(10);
	return !nncam_active;
}

int nn_camera_stop(void)
{
	enum cam_own_stop act = cam_own_stop_take(&nncam_own);
	enum cam_drain_step step;
	int parked;

	switch (act) {
	case CAM_OWN_STOP_IDLE:
		return -1;                          /* nothing of ours is up                */
	case CAM_OWN_STOP_HELD:
		return -8;                          /* a start / another stop owns it       */
	case CAM_OWN_STOP_DRAIN:                /* the running case                     */
	case CAM_OWN_STOP_RETRY:                /* finish what an earlier stop could not */
		break;
	}

	/* [!] The ORDER is the fix (issue #72): CAM_OWN_DRAINING was entered above,
	 * BEFORE this disable, so no `nn stream start` can re-enable the step while
	 * its count is being watched -- a re-armed step's zero would prove nothing.  Everything from here to the commit runs
	 * with the lifecycle held but NO serialiser held: these waits sleep, and one
	 * of the callbacks takes nncam_lock. */
	nncam_run = 0;                          /* disable: worker exits its run loop   */
	/* [!] The stop is a boundary whether or not the sink is attached (issue
	 * #118): the close below moves the generation only if there is a base to
	 * detach from, and an inference in flight must land nowhere either way. */
	nncam_record_boundary();
	(void)tx_semaphore_put(&nncam_sem);     /* wake the worker if waiting           */
	camera_sink_step_disable(&nncam_step);  /* disarm (close); base keeps running   */
	/* nn lets go of the camera (issue #142).  The operator's place stays, so
	 * the camera keeps streaming: stopping it is `camera stream stop`'s. */
	(void)camera_stream_leave(&nncam_holder, NULL);

	/* The step first.  The producer can have been inside it across the disarm,
	 * and keeps preprocessing into the input tensor until it leaves -- the thing
	 * this file used to assume away.  Zero after the disarm is the proof that it
	 * is out and cannot come back (port/camera/cam_step.h, issue #142). */
	step = camera_sink_step_drain(&nncam_step, NNCAM_DRAIN_TICKS);
	/* The producer is confirmed out once the drain succeeded: a frame it was
	 * part way through preparing will never be finished (JOIN, issue #130).  A
	 * frame handed over or running is the worker's, and is left to it -- the
	 * worker parks the word on its way out, and the next start drops anything
	 * it left. */
	if (step == CAM_DRAIN_DONE) {
		(void)nn_core_frame_join(&nncam_frame, &nncam_frame_ops);
		/* [!] AND ONLY NOW MAY THE SESSION GO BACK (issue #130): the producer
		 * writes the input tensor, inside the arena the session guards.  Set on
		 * the first stop and on a retry alike; never on a drain that ran out. */
		tx_mutex_get(&nncam_lock, TX_WAIT_FOREVER);
		nncam_sink_drained = 1;
		tx_mutex_put(&nncam_lock);
	}

	/* Then the worker.  If it is still mid-nn_run(), keep the session: releasing
	 * now would let another activity acquire the model while the worker still
	 * reads it.  The worker releases the session itself when it leaves the run
	 * loop, so the -2 below does not lose the release.  And a drain that ran out
	 * keeps it too (-7): the retrying stop that confirms the drain releases. */
	parked = nncam_settle_worker();
	nncam_release_session(parked ? NN_SESS_STOP_PARKED : NN_SESS_STOP_BUSY);

	cam_own_drain_finish(&nncam_own, step, parked);
	/* [!] The worker may have parked between that last poll and this commit, and
	 * found the lifecycle still DRAINING -- so it left it alone, correctly, and
	 * nobody would ever clear the SETTLING just written.  Close that window here;
	 * in the other interleaving the worker's own settle runs after this commit
	 * and does it instead. */
	if (!parked && !nncam_active)
		cam_own_settle(&nncam_own);

	if (step != CAM_DRAIN_DONE)
		return -7;                          /* sink still pinned: reuse refused     */
	return parked ? 0 : -2;                 /* -2: worker still winding down        */
}

bool nn_camera_running(void)
{
	return nncam_run != 0;
}

bool nn_camera_overlay(void)
{
	return nncam_run != 0 && !nncam_oneshot;
}

void nn_camera_stats_get(struct nn_camera_stats *out)
{
	uint32_t now, elapsed;

	if (!out)
		return;
	out->running    = (nncam_run != 0);
	out->res        = nncam_res;
	out->gen_frames = nncam_gen_frames;
	out->gen_infers = nncam_gen_infers;
	out->gen_errors = nncam_gen_errors;
	out->gen_drops  = nncam_gen_drops;
	out->gen_decoder_errors = nncam_gen_decoder_errors;
	out->gen_model_errors   = nncam_gen_model_errors;
	out->frames     = nnstat.frames;
	out->drops      = nnstat.drops;
	out->infers     = nnstat.infers;
	out->errors     = nnstat.errors;
	out->last_us    = nnstat.last_us;
	out->detections = nnstat.detections;
	out->depth_decode = nncam_depth_decode;
	out->depth_draw   = nncam_depth_draw;
	out->depth_shell  = nncam_depth_shell;

	now = HAL_GetTick();
	elapsed = now - nnstat.start_tick;
	out->fps_x100 = elapsed ? (uint32_t)((uint64_t)nnstat.infers * 100000u / elapsed) : 0u;
}

int nn_camera_dets_get(struct bf_det *out, int max)
{
	struct nn_camera_decode snap;

	if (!out || max <= 0)
		return 0;
	if (!nn_camera_decode_get(&snap, out, max, NULL, NULL))
		return 0;
	/* [!] ONLY THE SESSION IN FORCE (issue #118).  The record keeps its last
	 * boxes across a stop now, and this is what the GUI stamps on a LIVE
	 * picture: a new stream would open wearing the previous one's faces. */
	if (!snap.current)
		return 0;
	/* [!] AND ONLY THE RESIDENT DECODER'S (issue #159): a plugin's count has no
	 * boxes in this record -- its result is private, and it draws it itself. */
	if (snap.kind != (uint8_t)NN_DET_CALLER_BOXES)
		return 0;
	/* A count of boxes actually copied.  A negative ndet means "not a BlazeFace
	 * model" and there are no boxes to hand back; callers that need to tell that
	 * apart from an honest zero use nn_camera_decode_get(). */
	if (snap.ndet <= 0)
		return 0;
	return snap.ndet < max ? snap.ndet : max;
}

int nn_camera_decode_get(struct nn_camera_decode *out, struct bf_det *dets,
                         int max, struct nn_report_capture *rep,
                         struct nn_result_extra *ext)
{
	struct nn_det_snapshot snap;
#if defined(CONFIG_NN_BACKEND_TFLM)
	int leased = 0;
#endif

	if (!nncam_created || !out)
		return 0;                           /* nncam_lock not created before 1st start */
#if defined(CONFIG_NN_BACKEND_TFLM)
	/*
	 * [!] THE LEASE FIRST, AND ONLY FOR A CAPTURE (issue #159, wio-lite-ai's
	 * #110).  A plugin's account of its result has to be taken in the same
	 * breath as the snapshot that describes it -- the next decode overwrites
	 * the private state -- so both happen inside one hold.  The order is lease
	 * then record lock, the same way the worker takes them; the GUIX draw asks
	 * for no capture, so it never waits here.
	 */
	if (rep != NULL && nn_active_is_plugin()) {
		leased = plugin_lease_take();
		if (!leased)
			nn_report_set(rep, NN_REPORT_STALE);
	}
#endif
	tx_mutex_get(&nncam_lock, TX_WAIT_FOREVER);
	/*
	 * [!] ONE LOCK FOR BOTH.  The boxes and the diagnostics that describe them
	 * are published together and have to be read together: taking them in two
	 * calls lets a frame land in between, and `nn stream stats` -- which does not
	 * decode anything itself -- would print this frame's boxes beside another
	 * frame's peak score with nothing to show they disagree (issue #97).
	 */
	nn_det_record_snapshot(&nncam_rec, &snap, dets, max);
	if (ext != NULL)
		nn_det_record_extra(&nncam_rec, ext);   /* same hold (issue #121) */
	tx_mutex_put(&nncam_lock);
	out->valid      = snap.valid;
	out->ndet       = snap.ndet;
	out->res        = snap.res;
	out->current    = snap.current;
	out->accepted   = snap.accepted;
	out->epoch      = snap.epoch;
	out->kind       = snap.kind;
	out->reportable = snap.reportable;

#if defined(CONFIG_NN_BACKEND_TFLM)
	if (leased) {
		/* Still under the lease: the snapshot above and this account of it
		 * describe the same decode because nothing has run in between. */
		if (snap.valid && snap.kind == (uint8_t)NN_DET_PLUGIN_REPORT &&
		    !snap.reportable) {
			/* A later decode ran and its publish was dropped (issue #118):
			 * the plugin's result is that frame's now. */
			nn_report_set(rep, NN_REPORT_SUPERSEDED);
		} else if (snap.valid && snap.kind == (uint8_t)NN_DET_PLUGIN_REPORT) {
			int rc;

			nn_report_begin(rep);
			/* Compared against 1: NN_ACTIVE_NOT_HELD is negative, and a
			 * refusal for want of the lease is REFUSED, never UNSUPPORTED. */
			rc = nn_active_can_report();
			if (rc == NN_ACTIVE_NOT_HELD)
				nn_report_set(rep, NN_REPORT_REFUSED);
			else if (rc != 1)
				nn_report_set(rep, NN_REPORT_UNSUPPORTED);
			else
				nn_report_end(rep, nn_active_report(nn_report_write, rep));
		} else {
			/* A plugin is loaded but this record is not its work. */
			nn_report_set(rep, NN_REPORT_NONE);
		}
		plugin_lease_give();
	}
#endif
	return 1;
}

void nn_camera_record_invalidate(void)
{
	/* Before the first start the lock does not exist, and nothing has ever
	 * published into the record. */
	if (!nncam_created)
		return;
	tx_mutex_get(&nncam_lock, TX_WAIT_FOREVER);
	nn_det_record_invalidate(&nncam_rec);
	tx_mutex_put(&nncam_lock);
}

void nn_camera_set_norm(int signed_range) { nncam_norm_signed = signed_range ? 1 : 0; }
int  nn_camera_get_norm(void)              { return nncam_norm_signed; }
