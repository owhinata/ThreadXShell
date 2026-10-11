/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Wio Lite AI ThreadX Shell Project
 */
/**
 * @file    nn_camera_priv.h
 * @brief   The state and hooks shared by the four files of the camera ->
 * inference glue (issue #140).
 *
 * Private to nn_camera.c, nn_producer.c, nn_worker.c and nn_panel.c; nothing
 * else includes it.  The glue is split by the thread a function runs on:
 *
 *   nn_camera.c    console (start / stop / stats / record reads, `nn norm` /
 *                  the derived overlay); DEFINES every object declared below, and the
 *                  frame path's ops table
 *   nn_producer.c  the camera producer thread: the band callback, the prep
 *                  into the input tensor, the wake of the worker
 *   nn_worker.c    the inference worker (`nn_work`, priority 18): nn_run(),
 *                  then decode and publish under the result lease, the counting
 *   nn_panel.c     the preview thread's nn half: the record snapshot, the
 *                  paint's plugin call, the draw counters (the lease is a TRY)
 *
 * Every object here has ONE definition, in nn_camera.c.  Next to each one: who
 * writes it, and who resets it.  The lock order is this board's own: the
 * result lease, then the record lock (nncam_det_lock) or the frame lock --
 * the worker holds the lease across decode + publish, the panel only tries it.
 */
#ifndef NN_CAMERA_PRIV_H
#define NN_CAMERA_PRIV_H

#include <stdint.h>

#include "tx_api.h"

#include "camera.h"          /* CAMERA_BAND_ROWS, CAMERA_FRAME_HEIGHT */
#include "nn.h"
#include "nn_det_record.h"
#include "nn_core_frame.h"   /* the shared frame path and its hand-over word (#130) */

/* ---- objects (defined in nn_camera.c) ------------------------------------- */

/* Created by nn_camera.c; posted by the start, taken by the worker between
 * streams. */
extern TX_SEMAPHORE nncam_start_sem;
/* Posted by the producer (nncam_infer_start(), the hand-over) and the stop (a
 * poke); taken by the worker; drained by the start and by the worker's arm. */
extern TX_SEMAPHORE nncam_frame_sem;
/* Guards nncam_rec.  Created by nn_camera.c; taken by the worker (publish,
 * admits, generation), the console (boundaries, invalidate, decode_get) and,
 * through nn_camera_decode_get(), the panel.  Always AFTER the lease. */
extern TX_MUTEX     nncam_det_lock;
/* Written by the start; read by the producer's prep and the worker. */
extern struct nn_model *nncam_model;
/* Written by the start (1) and the stop (0) only; read by the worker. */
extern volatile int nncam_run;
/* Written by nn_camera_start() only, before nncam_run is raised; read by the
 * worker's counting. */
extern int nncam_oneshot;
/* Written by the worker only; read by the stop. */
extern volatile int nncam_worker_busy;
/* A stop has not yet seen nn's band step drained (nncam_teardown.h).  Written
 * by nn_camera_stop() only; read by the worker and the start. */
extern volatile int nncam_drain_pending;
/* The hand-over word: stepped only through svc/nn_core_frame.c -- by the producer
 * (on_frame, abandon), the worker (discard / want / take / done / join /
 * on_infer_done), the panel (draw, present_done) and the start / stop (join,
 * reset). */
extern struct nn_core_frame nncam_frame;

/*
 * The per-stream counters.  Reset (all of them) by nn_camera_start() only;
 * read by nn_camera_stats_get().
 *   nncam_frames, nncam_skipped, nncam_raced,
 *   nncam_ingest_last, nncam_ingest_max      : the producer only
 *   nncam_infers, nncam_model_errors, nncam_decoder_errors,
 *   nncam_stale_posts, nncam_infer_cyc       : the worker only
 *   nncam_errors : the producer AND the worker -- see nncam_error()
 */
extern uint32_t nncam_infers, nncam_frames, nncam_skipped, nncam_errors;
extern uint32_t nncam_model_errors, nncam_decoder_errors;
extern uint32_t nncam_raced, nncam_stale_posts;
extern uint32_t nncam_ingest_last, nncam_ingest_max, nncam_infer_cyc;

/* Written by `nn norm` (console); read by the producer's downsample. */
extern int nncam_norm_signed;

/* The published decode, guarded by nncam_det_lock: published by the worker,
 * boundaries by the start / stop, invalidated by a load / unload, read by the
 * console and the panel (nn_camera_decode_get()). */
extern struct nn_det_record nncam_rec;

/* Input geometry: written by the start, read by the producer while a stream
 * runs. */
extern unsigned nncam_ow, nncam_oh, nncam_oc;

/* ---- hooks the threads call across the files ------------------------------ */

/* The frame path's hooks (defined in nn_camera.c, in the ops table there). */
extern const struct nn_core_frame_ops nncam_frame_ops;

/* nn_camera.c: give back the NN session + the OCTOSPI1 guard, exactly once --
 * the stop on the console, or the worker on its way off the run loop. */
void nncam_guards_give(void);

/* nn's place among the camera's holders (issue #142); defined in nn_camera.c,
 * read by the band step for its lost mark. */
struct cam_holder;
extern const struct cam_holder nncam_holder;

/* nn_producer.c: nn's step of the band sink (armed by the start), and the
 * frame path's prep / wake. */
void nncam_band(unsigned band, const uint16_t *px, unsigned rows);
int  nncam_prep(void *ctx, unsigned band, unsigned nparts);
void nncam_infer_start(void *ctx);

/* nn_worker.c: the thread entry, and the frame path's publish / account. */
void nncam_entry(ULONG arg);
int  nncam_publish_raw_hook(void *ctx, uint32_t gen);
void nncam_account(void *ctx, enum nn_core_done what, int n, int took);
#if defined(CONFIG_NN_BACKEND_TFLM)
int  nncam_admits_hook(void *ctx, uint32_t gen);
int  nncam_decode(void *ctx, int *n);
int  nncam_publish_hook(void *ctx, int n, uint32_t gen);
#endif

/* ---- helpers used by more than one thread --------------------------------- */

/*
 * [!] nncam_errors HAS TWO WRITERS -- the producer (a band of the wrong height,
 * an abandoned fill) and the worker (an inference, a lease, a refused decode)
 * -- so it moves only through nncam_error(), one masked read-modify-write, as
 * grove-vision-ai-v2's nn_ov_bump() (issue #138).  A plain `++` from both
 * threads could lose a count.  Every other counter here has one writer.
 */
static inline void nncam_error(void)
{
	TX_INTERRUPT_SAVE_AREA

	TX_DISABLE
	nncam_errors++;
	TX_RESTORE
}

/*
 * First output row belonging to band @p band, for a model @p oh rows tall.
 *
 * Nearest-neighbour sampling puts output row oy on source row floor(oy*SRC_H/oh), so
 * oy belongs to band b exactly when that lands in [b*ROWS, (b+1)*ROWS) --
 * i.e. oy in [ceil(b*ROWS*oh / SRC_H), ceil((b+1)*ROWS*oh / SRC_H)).  For the 128
 * input this is the exact 32-rows-per-band split the design assumes (band b covers
 * output rows [32b, 32b+32), whose source rows [60b, 60b+58] lie wholly inside band
 * b), and there is no band-boundary sampling case at all.  Written in the general
 * form so a differently sized model input tiles correctly instead of silently
 * sampling across a band it was never handed; nn_camera_start() checks that the
 * tiling actually covers [0, oh).
 */
static inline unsigned nncam_oy_bound(unsigned band, unsigned oh)
{
	uint32_t n = (uint32_t)band * (uint32_t)CAMERA_BAND_ROWS * (uint32_t)oh;

	return (unsigned)((n + CAMERA_FRAME_HEIGHT - 1u) / CAMERA_FRAME_HEIGHT);
}

#endif /* NN_CAMERA_PRIV_H */
