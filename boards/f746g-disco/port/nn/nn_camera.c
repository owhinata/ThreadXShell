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
 *   - A SYNCHRONOUS copy push sink (like nx_mjpeg.c eth_sink): consume() runs in
 *     the camera producer thread, resizes + converts the RGB565 frame into an
 *     int8 staging buffer, and camera_frame_put()s the pin as its LAST STATEMENT.
 *     The nearest-neighbour resize reads only WxH sampled source pixels (cheaper
 *     than copying the whole frame), keeping producer-thread load low.
 *     [!] This file used to claim the in-flight count is "always 0".  That is
 *     true of the consume BODY and false at detach, which is the moment an owner
 *     asks: camera_unsubscribe() detaches while the base keeps running, so a
 *     publish can already be in flight across the unlink.  nn_camera_stop() now
 *     drains that pin instead of assuming it away (issue #72).
 *   - NN-input ownership (the BLOCKING codex fix): the sink NEVER writes a buffer
 *     the worker is using.  Two staging buffers with a FREE/FILLING/READY/RUNNING
 *     state machine under a short TX_MUTEX; the worker copies a READY stage into
 *     the model input (the only writer of nn_input()->data) and runs inference.
 *     No free stage -> drop (FRAME_POLICY_DROP).
 *   - Worker priority 18 (below BG-17/CLI-16/GUIX-14/net-12/camera-10): fully
 *     best-effort, so inference never starves the DCMI ring, UI, net, or the CLI
 *     that must deliver `ai stream stop`.  Inference is monolithic (no mid-run
 *     yield), so a lower-than-CLI priority is what guarantees stop reaches us.
 */
#include <string.h>          /* memcpy */

#include "tx_api.h"

#include "nn.h"
#include "nn_camera.h"
#include "nn_decoder.h"   /* the shared BlazeFace decoder, via this board's adapter */
#include "nn_det_record.h"
#include "camera.h"          /* camera_subscribe / camera_unsubscribe / camera_frame_put */
#include "cam_own.h"         /* the owner lifecycle (issue #72)                        */
#include "frame_pipeline.h"  /* struct frame_sink / frame_desc / FRAME_POLICY_* */

#include "stm32f7xx_hal.h"   /* HAL_GetTick, HAL_RCC_GetHCLKFreq */

#define LOG_TAG "nncam"
#include "log.h"

/* Max model input: BlazeFace-128 is 128x128x3.  Two staging buffers in the NN
 * arena (.sdram.ai, bank3).  Sized for the worst case -- a FLOAT32 128x128x3
 * input (BlazeFace) = 196608 B/buffer; int8 models use a quarter.  Larger models
 * would bump these bounds. */
#define NNCAM_IN_MAX_W    128u
#define NNCAM_IN_MAX_H    128u
#define NNCAM_IN_MAX_C    3u
#define NNCAM_STAGE_BYTES (NNCAM_IN_MAX_W * NNCAM_IN_MAX_H * NNCAM_IN_MAX_C * 4u)
#define NNCAM_STAGE_N     2

/* Float32 input normalization.  BlazeFace's model card says [-1,1], but its ST
 * config says rescale 1/255 -> [0,1] -- and [0,1] is what actually detects faces
 * on hardware (maxscore 288 vs 11), so ST retrained with [0,1].  Default [0,1];
 * `ai norm 1` flips to [-1,1] at runtime for other models. */
#ifndef NNCAM_NORM_SIGNED
#define NNCAM_NORM_SIGNED 0
#endif

#define NNCAM_WORKER_PRIORITY 18          /* full best-effort (below BG-17)        */
/* Sized from the measured high-water-mark (`thread` peak = 1024 B under the
 * stedgeai backend, owhinata/stm32f746g-disco#93); 4096 keeps ~4x margin.  NOTE: re-measure
 * if switching to the tflm/reloc backend -- CMSIS-NN kernels + the interpreter nest
 * deeper. */
#define NNCAM_WORKER_STACK    4096u
#define NNCAM_POLL_TICKS      100u        /* sem wait -> stop latency               */
/* Teardown budgets (ticks, WALL CLOCK; 1 tick = 1 ms).  The sink drain is short
 * because a healthy consume() is one resize away from its put; the worker settle
 * keeps the 3 s this always allowed, since it may have to outlast a whole
 * inference.  Both are wall clock rather than a count of sleeps: counting
 * iterations burns the budget instantly once the sleeps stop sleeping, and would
 * then report a healthy worker as stuck (issue #65). */
#define NNCAM_DRAIN_TICKS     100u        /* sink pin -> release                    */
#define NNCAM_SETTLE_TICKS    3000u       /* worker parks (may span an inference)   */

/* Staging buffers live in the NN arena (bank3, .sdram.ai).  Raw bytes: hold either
 * int8 or float32 preprocessed input depending on the model's input dtype. */
static uint8_t nncam_stage[NNCAM_STAGE_N][NNCAM_STAGE_BYTES]
	__attribute__((aligned(32), section(".sdram.ai")));

enum { ST_FREE = 0, ST_FILLING, ST_READY, ST_RUNNING };
static uint8_t nncam_state[NNCAM_STAGE_N];

static TX_THREAD    nncam_thread;
static UCHAR        nncam_stack[NNCAM_WORKER_STACK];
static TX_MUTEX     nncam_lock;           /* guards nncam_state[]                   */
static TX_SEMAPHORE nncam_sem;            /* consume posts a READY stage             */

static struct frame_sink nncam_sink;
static struct nn_model  *nncam_model;

/* enabled intent (ai stream start=1, stop=0) */
static volatile int nncam_run;
/* worker is in the run loop (set by worker) */
static volatile int nncam_active;
/* base detached (close cb): paused, not stopped */
static volatile int nncam_producer_dead;
/* the AI subscriber holds the nn session */
static volatile int nncam_holds_session;
/* worker/objects created once */
static int          nncam_created;

/* The `ai stream` lifecycle (issue #72), serialised by cam_own.h.  It guards the
 * SINK: which entry points may subscribe it, and the interval in which a stop
 * has detached it and is watching its pins fall to zero.  The three volatile
 * flags above stay, and describe something else -- the worker and the nn session,
 * which deliberately OUTLIVE a stop that returned -2 (the worker releases the
 * session itself on its way out).  Both are consulted at a start; neither
 * subsumes the other. */
static volatile enum cam_own_state nncam_own;
/* enum camera_res hint (display only, owhinata/stm32f746g-disco#100) */
static uint8_t      nncam_res;

/* Session generation, bumped on every (re)attach + on a base detach.  An in-flight
 * ingest that spans a session boundary reverts instead of injecting a stale frame. */
static volatile uint32_t nncam_epoch;

/* Release the nn session iff the AI subscriber still holds it (exactly once per
 * `ai stream` lifetime; called only from nn_camera_stop()).  A base detach (close)
 * NEVER releases -- the session owner is the AI enabled intent (contract
 * owhinata/stm32f746g-disco#100.4). */
static void nncam_release_session(void)
{
	int held;

	tx_mutex_get(&nncam_lock, TX_WAIT_FOREVER);
	held = nncam_holds_session;
	nncam_holds_session = 0;
	tx_mutex_put(&nncam_lock);
	if (held)
		nn_session_release();
}

/* Model input geometry, latched at start from the input tensor. */
static uint16_t nncam_in_w, nncam_in_h, nncam_in_c;
static uint32_t nncam_in_bytes;
static uint8_t  nncam_in_dtype;   /* enum nn_dtype of the model input */

/* Latest detections (BlazeFace), guarded by nncam_lock; read via nn_camera_dets_get(). */
/*
 * The published decode: boxes AND the diagnostics that describe them, plus the
 * session generation that keeps a decode from a session that has ended out of a
 * live one.  The rules live in svc/nn_det_record.c so they can be tested -- the
 * ordering they guard against cannot be produced deterministically on hardware --
 * and the storage and the lock (nncam_lock) are ours.
 */
static struct nn_det_record nncam_rec;

/* Float32 input normalization, runtime-tunable (ai norm) to settle the [-1,1] vs
 * [0,1] ambiguity on hardware without reflashing. */
static int nncam_norm_signed = NNCAM_NORM_SIGNED;

static struct {
	uint32_t frames;
	uint32_t drops;
	uint32_t infers;
	uint32_t errors;
	uint32_t last_us;
	uint32_t detections;
	uint32_t start_tick;
} nnstat;

/* ---- RGB565 -> model input: nearest-neighbour resize + convert ------------ */

/* Convert one camera frame @p f (RGB565) into @p dst (HxWxC), formatted for the
 * model input dtype.  Reads only the sampled source pixels.
 *   - FLOAT32: RGB in [-1,1] (NNCAM_NORM_SIGNED) or [0,1] -- BlazeFace.
 *   - INT8   : uint8 - 128 (symmetric ~[-1,1], scale 1/128) -- MNIST/null stub. */
static void nncam_preprocess(const struct frame_desc *f, void *dst)
{
	const uint8_t *base = (const uint8_t *)f->data;
	uint32_t stride = f->stride ? f->stride : (uint32_t)f->width * 2u;
	uint16_t ow = nncam_in_w, oh = nncam_in_h, oc = nncam_in_c;
	uint16_t sw = f->width, sh = f->height;
	int is_f32 = (nncam_in_dtype == NN_DTYPE_FLOAT32);

	for (uint16_t oy = 0; oy < oh; oy++) {
		uint16_t sy = (uint16_t)((uint32_t)oy * sh / oh);
		const uint8_t *row = base + (uint32_t)sy * stride;
		for (uint16_t ox = 0; ox < ow; ox++) {
			uint16_t sx = (uint16_t)((uint32_t)ox * sw / ow);
			uint16_t px = (uint16_t)(row[sx * 2] | ((uint16_t)row[sx * 2 + 1] << 8));
			uint8_t rgb[3];
			uint32_t o = ((uint32_t)oy * ow + ox) * oc;

			rgb[0] = (uint8_t)(((px >> 11) & 0x1Fu) * 255u / 31u);
			rgb[1] = (uint8_t)(((px >> 5) & 0x3Fu) * 255u / 63u);
			rgb[2] = (uint8_t)((px & 0x1Fu) * 255u / 31u);

			if (is_f32) {
				float *o32 = (float *)dst + o;
				for (uint16_t c = 0; c < oc && c < 3; c++)
					o32[c] = nncam_norm_signed
					       ? (float)rgb[c] / 127.5f - 1.0f   /* [-1,1] */
					       : (float)rgb[c] / 255.0f;         /* [0,1]  */
			} else {
				int8_t *o8 = (int8_t *)dst + o;
				for (uint16_t c = 0; c < oc && c < 3; c++)
					o8[c] = (int8_t)((int)rgb[c] - 128);
			}
		}
	}
}

/* ---- frame-pipeline sink (synchronous copy) ------------------------------- */

/* Reset per-session state on attach (nncam_open, the sole attach path in the
 * subscriber model).  Bumps the epoch so an in-flight ingest from a prior session
 * reverts, and clears producer_dead so a stale base-detach flag does not instantly
 * re-pause the fresh session.  Under nncam_lock and preserving any ST_RUNNING stage:
 * an overrun auto-recovery re-attaches (this reset) while the AI stays enabled, so
 * the prio-18 worker may be mid-copy out of a ST_RUNNING stage -- clobbering it to
 * ST_FREE would let the producer reuse that buffer under the worker (contract
 * owhinata/stm32f746g-disco#100.3, same discipline as nncam_close). */
static void nncam_session_reset(void)
{
	tx_mutex_get(&nncam_lock, TX_WAIT_FOREVER);
	nncam_producer_dead = 0;
	/* Drop stale detections from a prior session AND move the generation, so an
	 * inference still running under the old one cannot publish into this one. */
	nn_det_record_reset(&nncam_rec);
	for (int i = 0; i < NNCAM_STAGE_N; i++)
		if (nncam_state[i] != ST_RUNNING)
			nncam_state[i] = ST_FREE;   /* leave a stage the worker is copying out of */
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

	/* Record the actual base geometry for the stats display (the input adapts to
	   whatever resolution the base publishes -- the model input is fixed 128x128). */
	nncam_res = (w <= 160u) ? (uint8_t)CAM_RES_QQVGA
	          : (w <= 320u) ? (uint8_t)CAM_RES_QVGA
	                        : (uint8_t)CAM_RES_VGA;
	nncam_session_reset();                  /* fresh session on (re)attach          */
	return 0;
}

/* Ingest one delivered frame: claim a FREE stage, preprocess @p f into it, and
 * (unless the session was torn down / rotated during preprocess) publish it READY
 * and wake the worker.  Reads f->data.  Does NOT release the pipeline pin -- the
 * consume() caller owns pin release. */
static void nncam_ingest(const struct frame_desc *f)
{
	uint32_t epoch0;
	int i = -1;

	/* Claim a FREE staging buffer (short critical section), latching the epoch. */
	tx_mutex_get(&nncam_lock, TX_WAIT_FOREVER);
	epoch0 = nncam_epoch;
	for (int k = 0; k < NNCAM_STAGE_N; k++) {
		if (nncam_state[k] == ST_FREE) { nncam_state[k] = ST_FILLING; i = k; break; }
	}
	tx_mutex_put(&nncam_lock);

	if (i < 0) {                            /* no free buffer -> drop this frame    */
		nnstat.drops++;
		return;
	}

	/* Resize + convert while the slot is still pinned by the caller (reads f->data). */
	nncam_preprocess(f, nncam_stage[i]);

	tx_mutex_get(&nncam_lock, TX_WAIT_FOREVER);
	if (nncam_epoch != epoch0 || !nncam_run) {
		/* The session was aborted / rotated during preprocess: do NOT inject this
		 * (now stale) frame into a new session.  Revert the stage to FREE. */
		nncam_state[i] = ST_FREE;
		tx_mutex_put(&nncam_lock);
		return;
	}
	nncam_state[i] = ST_READY;
	tx_mutex_put(&nncam_lock);

	nnstat.frames++;
	(void)tx_semaphore_put(&nncam_sem);     /* wake the worker                      */
}

/* [!] PUT LAST.  camera_frame_put() must stay the LAST STATEMENT here, as in
   every consume() on this board (issue #72): once the pin reaches zero this
   callback touches nothing the owner owns, which is what lets nn_camera_stop()
   settle for ONE count.  Work moved back below the put would be invisible to
   that count -- the failure this rule exists to prevent. */
static int nncam_consume(void *ctx, const struct frame_desc *f)
{
	(void)ctx;

	if (nncam_run)
		nncam_ingest(f);                    /* preprocess into staging (pin held)   */
	camera_frame_put(&nncam_sink, f);       /* LAST -- see the rule above           */
	return 0;
}

/* Base detached this subscriber (base capture stopped / DCMI overrun / cascade).
 * A PAUSE, not a stop (contract owhinata/stm32f746g-disco#100.2/.4): keep the AI enabled
 * (nncam_run) and the nn session held -- the session owner is the `ai stream` intent,
 * released only by nn_camera_stop().  Bump the epoch so an in-flight ingest reverts, drop
 * any pending (non-RUNNING) staged frame so a stale frame is not inferred after a
 * later re-attach, and wake the worker to re-evaluate.  Non-blocking and no camera
 * API re-entry, so it is safe under the camera lock. */
static void nncam_close(void *ctx)
{
	(void)ctx;
	tx_mutex_get(&nncam_lock, TX_WAIT_FOREVER);
	nncam_producer_dead = 1;
	nncam_epoch++;                          /* an in-flight FILLING ingest reverts   */
	for (int i = 0; i < NNCAM_STAGE_N; i++)
		if (nncam_state[i] == ST_READY)
			nncam_state[i] = ST_FREE;       /* drop pending; ST_RUNNING/FILLING left */
	/* Clear the boxes: no live frames while paused.  Moving the generation with
	 * them is what stops the ST_RUNNING stage this function deliberately leaves
	 * alone from publishing after the pause. */
	nn_det_record_reset(&nncam_rec);
	tx_mutex_put(&nncam_lock);
	(void)tx_semaphore_put(&nncam_sem);     /* wake worker (idles until re-attach)  */
}

/* ---- inference worker ----------------------------------------------------- */

/* Run one inference from a READY stage, if any.  Returns 1 if an inference ran. */
static int nncam_step(void)
{
	uint32_t hclk = HAL_RCC_GetHCLKFreq();
	uint32_t mhz = hclk / 1000000u ? hclk / 1000000u : 1u;
	int j = -1;
	int rc;
	uint32_t gen;

	tx_mutex_get(&nncam_lock, TX_WAIT_FOREVER);
	for (int k = 0; k < NNCAM_STAGE_N; k++) {
		if (nncam_state[k] == ST_READY) { nncam_state[k] = ST_RUNNING; j = k; break; }
	}
	/*
	 * [!] THE GENERATION IS SAMPLED WITH THE CLAIM, under the same lock.  Taking
	 * it later would leave a window: nncam_close() deliberately LEAVES an
	 * ST_RUNNING stage alone and bumps the epoch, so this thread can be inside
	 * the inference below when the session ends -- and a value read afterwards
	 * would already be the next session's.
	 */
	gen = nn_det_record_gen(&nncam_rec);
	tx_mutex_put(&nncam_lock);

	if (j < 0)
		return 0;

	/* Copy the READY stage into the model input (worker is the sole writer of
	 * nn_input()->data), then free the stage so the sink can reuse it during the
	 * inference -- the double-buffer benefit. */
	{
		struct nn_tensor *in = nn_input(nncam_model, 0);
		if (in && in->data)
			memcpy(in->data, nncam_stage[j], nncam_in_bytes);
	}
	tx_mutex_get(&nncam_lock, TX_WAIT_FOREVER);
	nncam_state[j] = ST_FREE;
	tx_mutex_put(&nncam_lock);

	rc = nn_run(nncam_model);
	if (rc != 0) {
		nnstat.errors++;
		return 1;
	}
	nnstat.infers++;
	nnstat.last_us = nn_last_cycles(nncam_model) / mhz;

	/* Model-specific decode (BlazeFace).  A safe no-op (returns BF_ERR_MODEL) for
	 * other models, so this stays model-agnostic at the sink level.  Publish the
	 * boxes and the diagnostics that go with them under the lock. */
	{
		struct bf_det tmp[BF_MAX_DET];
		struct bf_result bfr;
		int nd = nn_decoder_run(nncam_model, tmp, BF_MAX_DET, &bfr);

		tx_mutex_get(&nncam_lock, TX_WAIT_FOREVER);
		/*
		 * [!] A NEGATIVE RETURN IS NO LONGER FOLDED INTO ZERO (issue #57/#97).
		 * It used to be, and that was the worst place for it: "0 faces" reads
		 * as a measurement, so a decoder that had never been initialised --
		 * a build fault, permanent, on every frame -- would have shown up as a
		 * perfectly healthy stream finding nobody.  The record keeps -1, and
		 * the status with it.
		 */
		(void)nn_det_record_publish(&nncam_rec, tmp, nd, &bfr, gen);
		tx_mutex_put(&nncam_lock);
		nnstat.detections = (nd > 0) ? (uint32_t)nd : 0u;
	}
	return 1;
}

static void nncam_entry(ULONG arg)
{
	(void)arg;
	for (;;) {
		while (!nncam_run)
			tx_thread_sleep(20);            /* parked until started                */

		/* Acknowledge the start ONLY once we are actually in the run loop.  If a
		 * stop raced ahead (nncam_run already 0), we fall straight back to park
		 * without ever setting nncam_active -- so nn_camera_stop() never waits on
		 * an active flag the worker will not clear (the BLOCKING stuck race). */
		nncam_active = 1;
		LOG_INF("inference running (prio %u)", (unsigned)NNCAM_WORKER_PRIORITY);
		/* Run while enabled.  A base detach (producer_dead) is a PAUSE, not a stop
		 * (owhinata/stm32f746g-disco#100): no frames flow so the worker just idles on the poll
		 * timeout until the base re-attaches (nncam_open re-arms the session).  Only
		 * nn_camera_stop clears nncam_run, so the session release stays with the AI intent
		 * owner. */
		while (nncam_run) {
			if (tx_semaphore_get(&nncam_sem, NNCAM_POLL_TICKS) != TX_SUCCESS)
				continue;                   /* timeout -> re-check run               */
			(void)nncam_step();
		}
		/* [!] THE SESSION GOES BACK BEFORE WE ANNOUNCE THAT WE PARKED, and the
		 * order is the correctness (issue #72).  nncam_active = 0 is what a stop
		 * waits on; the moment it is visible the stop may release the session,
		 * commit the lifecycle to IDLE and let a NEW `ai stream start` acquire a
		 * fresh session -- and if this release were still to come, it would then
		 * hand back the new stream's session, because the holds flag it reads is
		 * the one that start has just set.  Releasing first makes that
		 * impossible: after this line we own nothing a later start could be
		 * given.
		 *
		 * The run loop exits only on nncam_run=0 (an `ai stream stop`) -- a base
		 * detach is a pause that keeps run=1.  Releasing here is what frees the
		 * session even when nn_camera_stop() timed out mid-nn_run() (idempotent
		 * with nn_camera_stop's own release via the holds flag). */
		nncam_release_session();
		nncam_active = 0;                   /* parked; nn_camera_stop() waits on this */
		/* And finish the lifecycle if a stop gave up waiting for us: it committed
		 * SETTLING and returned -2, so this parking is the event that lets a later
		 * `ai stream start` back in (issue #72).  A no-op in every other state --
		 * a stop still inside its own drain commits its own result, and must not
		 * have it overwritten from here. */
		cam_own_settle(&nncam_own);
		LOG_INF("inference stopped (%lu infers)", (unsigned long)nnstat.infers);
	}
}

/* Open the model + latch its input geometry, bounds-checked against the staging
 * buffers.  Returns 0, -3 (model open) or -4 (geometry).  Shared by both start paths. */
static int nncam_open_model(void)
{
	struct nn_tensor *in;

	if (nn_model_open(&nncam_model) != 0)
		return -3;
	/* No model loaded (e.g. the stedgeai_reloc backend before `ai model load`): the
	 * single choke point that rejects `ai run`/`ai stream` cleanly. */
	if (nn_input_count(nncam_model) == 0)
		return -3;
	in = nn_input(nncam_model, 0);
	if (!in || in->ndim < 3)
		return -4;
	nncam_in_h = in->dims[1];
	nncam_in_w = in->dims[2];
	nncam_in_c = (in->ndim >= 4) ? in->dims[3] : 1;
	nncam_in_bytes = in->bytes;
	nncam_in_dtype = in->dtype;
	if (nncam_in_w > NNCAM_IN_MAX_W || nncam_in_h > NNCAM_IN_MAX_H ||
	    nncam_in_c > NNCAM_IN_MAX_C || nncam_in_bytes > NNCAM_STAGE_BYTES)
		return -4;                          /* model input exceeds the staging bound */
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
	nncam_sink.name    = "nncam";
	nncam_sink.policy  = FRAME_POLICY_DROP;
	nncam_sink.open    = nncam_open;
	nncam_sink.consume = nncam_consume;
	nncam_sink.close   = nncam_close;
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

int nn_camera_start(enum camera_res res)
{
	enum cam_own_start act;
	int rc;

	/* input adapts to the base geometry (owhinata/stm32f746g-disco#100) */
	(void)res;
	/* Claim the lifecycle first (issue #72).  Everything below happens inside
	   CAM_OWN_STARTING, so a concurrent `ai stream stop` is refused rather than
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

	rc = nncam_open_model();                /* model + input geometry (bounds-checked) */
	if (rc != 0) {
		cam_own_start_finish(&nncam_own, 0);
		return rc;
	}
	if (nncam_create_objects() != 0) {
		cam_own_start_finish(&nncam_own, 0);
		return -5;
	}

	/* Claim the single inference session first: refused (-6) if `ai bench` or a
	 * stream is using the non-reentrant singleton model.  The session owner is this
	 * `ai stream` enable; it is released only by nn_camera_stop() / the worker's
	 * run-loop exit, NEVER by a base detach (contract owhinata/stm32f746g-disco#100.4). */
	if (nn_session_try_acquire() != 0) {
		cam_own_start_finish(&nncam_own, 0);
		return -6;
	}
	nncam_holds_session = 1;

	/* Enable the subscriber BEFORE subscribing so a frame delivered the instant we
	 * attach is ingested.  camera_subscribe() attaches now iff the base is running
	 * and RGB565 (calls nncam_open -> session reset); otherwise the subscriber stays
	 * enabled + idle and attaches at the next `camera stream start`.  A non-zero rc
	 * is a hard failure (registry full / immediate attach rejected) -> unwind. */
	nncam_run = 1;
	rc = camera_subscribe(&nncam_sink, CAM_FMT_RGB565);
	if (rc != 0) {
		nncam_run = 0;
		nncam_holds_session = 0;
		nn_session_release();
		cam_own_start_finish(&nncam_own, 0);
		return rc;
	}
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
	 * BEFORE this unsubscribe, so no `ai stream start` can re-subscribe the sink
	 * while its pins are being watched -- frame_pipeline_attach() resets exactly
	 * the count the drain below reads.  Everything from here to the commit runs
	 * with the lifecycle held but NO serialiser held: these waits sleep, and one
	 * of the callbacks takes nncam_lock. */
	nncam_run = 0;                          /* disable: worker exits its run loop   */
	(void)tx_semaphore_put(&nncam_sem);     /* wake the worker if waiting           */
	(void)camera_unsubscribe(&nncam_sink);  /* detach (close); base keeps running   */

	/* The sink first.  A publish can have been in flight across the unlink, and
	 * that consume() keeps preprocessing into a staging buffer until it puts its
	 * pin back -- the thing this file used to assume away. */
	step = camera_sink_drain(&nncam_sink, NNCAM_DRAIN_TICKS);

	/* Then the worker.  If it is still mid-nn_run(), keep the session: releasing
	 * now would let another activity acquire the model while the worker still
	 * reads it.  The worker releases the session itself when it leaves the run
	 * loop, so the -2 below does not lose the release. */
	parked = nncam_settle_worker();
	if (parked)
		nncam_release_session();

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

void nn_camera_stats_get(struct nn_camera_stats *out)
{
	uint32_t now, elapsed;

	if (!out)
		return;
	out->running    = (nncam_run != 0);
	out->res        = nncam_res;
	out->frames     = nnstat.frames;
	out->drops      = nnstat.drops;
	out->infers     = nnstat.infers;
	out->errors     = nnstat.errors;
	out->last_us    = nnstat.last_us;
	out->detections = nnstat.detections;

	now = HAL_GetTick();
	elapsed = now - nnstat.start_tick;
	out->fps_x100 = elapsed ? (uint32_t)((uint64_t)nnstat.infers * 100000u / elapsed) : 0u;
}

int nn_camera_dets_get(struct bf_det *out, int max)
{
	struct nn_camera_decode snap;

	if (!out || max <= 0)
		return 0;
	if (!nn_camera_decode_get(&snap, out, max))
		return 0;
	/* A count of boxes actually copied.  A negative ndet means "not a BlazeFace
	 * model" and there are no boxes to hand back; callers that need to tell that
	 * apart from an honest zero use nn_camera_decode_get(). */
	if (snap.ndet <= 0)
		return 0;
	return snap.ndet < max ? snap.ndet : max;
}

int nn_camera_decode_get(struct nn_camera_decode *out, struct bf_det *dets,
                         int max)
{
	struct nn_det_snapshot snap;

	if (!nncam_created || !out)
		return 0;                           /* nncam_lock not created before 1st start */
	tx_mutex_get(&nncam_lock, TX_WAIT_FOREVER);
	/*
	 * [!] ONE LOCK FOR BOTH.  The boxes and the diagnostics that describe them
	 * are published together and have to be read together: taking them in two
	 * calls lets a frame land in between, and `ai stream stats` -- which does not
	 * decode anything itself -- would print this frame's boxes beside another
	 * frame's peak score with nothing to show they disagree (issue #97).
	 */
	nn_det_record_snapshot(&nncam_rec, &snap, dets, max);
	tx_mutex_put(&nncam_lock);
	out->valid = snap.valid;
	out->ndet  = snap.ndet;
	out->res   = snap.res;
	return 1;
}

void nn_camera_set_norm(int signed_range) { nncam_norm_signed = signed_range ? 1 : 0; }
int  nn_camera_get_norm(void)              { return nncam_norm_signed; }
