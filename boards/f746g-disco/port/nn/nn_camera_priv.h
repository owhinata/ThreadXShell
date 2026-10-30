/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    nn_camera_priv.h
 * @brief   The state and hooks shared by the three files of the camera ->
 * inference glue (issue #140).
 *
 * Private to nn_camera.c, nn_producer.c and nn_worker.c; nothing else includes
 * it.  The glue is split by the thread a function runs on:
 *
 *   nn_camera.c    console (start / stop / stats / record reads) and the caller
 *                  of attach / detach (the sink's open() / close()); DEFINES every
 *                  object declared below, and the frame path's ops table
 *   nn_producer.c  the camera producer thread: the sink's consume(), the prep
 *                  into the input tensor, the hand-over
 *   nn_worker.c    the inference worker (priority 18): nn_run(), the resident
 *                  decoder's publish, the counting
 *
 * Every object here has ONE definition, in nn_camera.c.  Next to each one: who
 * writes it, and who resets it.
 */
#ifndef NN_CAMERA_PRIV_H
#define NN_CAMERA_PRIV_H

#include <stdint.h>

#include "tx_api.h"

#include "nn.h"
#include "nn_det_record.h"
#include "nn_core_frame.h"   /* the shared frame path and its hand-over word (#130) */
#include "nn_sess_release.h" /* enum nn_sess_who                                    */
#include "cam_own.h"         /* the owner lifecycle (issue #72)                        */
#include "frame_pipeline.h"  /* struct frame_sink / frame_desc */

#define NNCAM_WORKER_PRIORITY 18          /* full best-effort (below BG-17)        */

/* ---- objects (defined in nn_camera.c) ------------------------------------- */

/* The hand-over word: stepped only through svc/nn/nn_core_frame.c -- by the producer
 * (on_frame), the worker (want / take / done / on_infer_done) and the start /
 * stop (discard / join). */
extern struct nn_core_frame nncam_frame;
/* Written by the producer in its prep (while the word is FILLING); read by the
 * worker after its TAKE. */
extern uint32_t     nncam_job_gen;
/* Created by nn_camera.c; taken by all three files. */
extern TX_MUTEX     nncam_lock;
/* Posted by the producer (a hand-over), close() and the stop; taken by the
 * worker; drained by the attach (nncam_session_reset()). */
extern TX_SEMAPHORE nncam_sem;
/* Written by the start (nncam_open_model()); read by the producer and worker. */
extern struct nn_model  *nncam_model;
/* Written by the start (1) and the stop (0) only; read by all three. */
extern volatile int nncam_run;
/* Written by the worker only; read by the start and the stop. */
extern volatile int nncam_active;
/* The `nn stream` lifecycle: the start / stop, and the worker's settle. */
extern volatile enum cam_own_state nncam_own;

/*
 * The per-stream counters.  Reset (all five) by nn_camera_start() only.
 *   nncam_gen_frames, nncam_gen_drops : the producer only
 *   nncam_gen_infers, nncam_gen_decoder_errors : the worker only
 *   nncam_gen_errors : the producer AND the worker -- see nncam_error()
 */
extern uint32_t     nncam_gen_frames;
extern uint32_t     nncam_gen_infers;
extern uint32_t     nncam_gen_errors;
extern uint32_t     nncam_gen_drops;
extern uint32_t     nncam_gen_decoder_errors;
extern uint32_t     nncam_gen_model_errors;    /* the worker only (#159) */
/* Written by nn_camera_start() before nncam_run is raised; read by the worker. */
extern int          nncam_oneshot;
/* Moved by the attach (open), the detach (close) under nncam_lock; read by the
 * producer's prep under the same lock. */
extern volatile uint32_t nncam_epoch;

/* Model input geometry: written by the start (nncam_open_model()), read by the
 * producer while a stream runs. */
extern uint16_t nncam_in_w, nncam_in_h, nncam_in_c;
extern uint32_t nncam_in_bytes;
extern uint8_t  nncam_in_dtype;

/* The published decode, guarded by nncam_lock: published by the worker, its
 * generation sampled by the producer, boundaries and reads by nn_camera.c. */
extern struct nn_det_record nncam_rec;

/* Written by `nn norm` (console); read by the producer's preprocess. */
extern int nncam_norm_signed;

/*
 * The per-attach counters.  Reset (all of them) by nncam_session_reset(), on
 * every attach.
 *   frames, drops       : the producer only
 *   infers, last_us, detections : the worker only
 *   errors              : the producer AND the worker -- see nncam_error()
 *   start_tick          : the reset only
 */
struct nncam_stat {
	uint32_t frames;
	uint32_t drops;
	uint32_t infers;
	uint32_t errors;
	uint32_t last_us;
	uint32_t detections;
	uint32_t start_tick;
};
extern struct nncam_stat nnstat;

/* ---- hooks the threads call across the files ------------------------------ */

/* The frame path's hooks (defined in nn_camera.c, in the ops table there). */
extern const struct nn_core_frame_ops nncam_frame_ops;

/* nn_camera.c: release the nn session (the worker on its way out, the stop). */
void nncam_release_session(enum nn_sess_who who);

/* nn_producer.c: the nn step's consume(), and the frame path's prep / wake. */
int  nncam_consume(void *ctx, const struct frame_desc *f);
int  nncam_prep(void *ctx, unsigned part, unsigned nparts);
void nncam_infer_start(void *ctx);

/* nn_worker.c: the thread entry, and the frame path's publish / account. */
void nncam_entry(ULONG arg);
int  nncam_publish(void *ctx, uint32_t gen);
void nncam_account(void *ctx, enum nn_core_done what, int n, int took);
#if defined(CONFIG_NN_BACKEND_TFLM)
/* nn_worker.c: the plugin's half of the frame path (issue #159) -- the
 * generation question, the decode and its publish, all under the plugin lease. */
int  nncam_admits_hook(void *ctx, uint32_t gen);
int  nncam_decode(void *ctx, int *n);
int  nncam_publish_hook(void *ctx, int n, uint32_t gen);
#endif

/* ---- helpers used by both threads ----------------------------------------- */

/* The frame path's critical section (PRIMASK), on the producer and the worker. */
static inline unsigned nncam_cs_enter(void)
{
	TX_INTERRUPT_SAVE_AREA

	TX_DISABLE
	return (unsigned)interrupt_save;
}

static inline void nncam_cs_exit(unsigned posture)
{
	TX_INTERRUPT_SAVE_AREA

	interrupt_save = (UINT)posture;
	TX_RESTORE
}

/*
 * [!] nncam_gen_errors AND nnstat.errors HAVE TWO WRITERS -- the producer (no
 * input tensor in the prep, a RACED hand-over) and the worker (nn_run(), a
 * refused decode) -- so they move only through nncam_error(), one masked
 * read-modify-write for both, as grove-vision-ai-v2's nn_ov_bump() (issue
 * #138).  A plain `++` from both threads could lose a count.
 */
static inline void nncam_error(void)
{
	TX_INTERRUPT_SAVE_AREA

	TX_DISABLE
	nnstat.errors++;
	nncam_gen_errors++;
	TX_RESTORE
}

#endif /* NN_CAMERA_PRIV_H */
