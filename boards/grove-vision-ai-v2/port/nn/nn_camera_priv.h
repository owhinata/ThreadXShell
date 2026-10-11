/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    nn_camera_priv.h
 * @brief   The state and hooks shared by the files of the camera -> inference
 *          glue (issue #140).
 *
 * Private to nn_camera.c, nn_producer.c, nn_worker.c and nn_panel.c; nothing
 * else includes it.  The glue is split by the thread a function runs on:
 *
 *   nn_camera.c    console (arm / arm_oneshot / request_stop / join / stats /
 *                  the one-shot's ending); DEFINES every object declared below,
 *                  the frame path's ops table and the overlay vtable, and the
 *                  hooks more than one thread calls through the table (the
 *                  critical section, the plugin question)
 *   nn_producer.c  the camera producer, inside consume(): process(), the prep
 *                  into the input tensor, the wake of the worker
 *   nn_worker.c    the inference worker (`nn_worker`, priority 12): the thread
 *                  and its join, nn_camera_work() -- invoke, lease wait -- and
 *                  the frame path's outputs / decode / publish / account
 *   nn_panel.c     the panel thread's nn half (cam_lcd_sink.c's draw()): the
 *                  record snapshot and the plugin's paint, the lease only TRIED
 *
 * Every object here has ONE definition, in nn_camera.c.  Next to each one: who
 * writes it, and who resets it.  The lock order is this board's own: the panel
 * guard, then a non-blocking lease try (nn_camera_draw()); no lease holder
 * waits for the panel guard, a camera API mutex or a pipeline lock.
 */
#ifndef NN_CAMERA_PRIV_H
#define NN_CAMERA_PRIV_H

#include <stdint.h>

#include "tx_api.h"

#include "nn_camera.h"     /* struct nn_camera_stats, NN_OV_SHOT_*            */
#include "nn_core_frame.h" /* the shared frame path (issue #130)              */
#include "nn_preproc.h"    /* struct nn_preproc_geom, carried in the job      */

/*
 * ---- The producer / worker split (issue #129, Epic #122 U1) ----------------
 *
 * The producer prepares a frame STRAIGHT INTO the model's input tensor, and
 * only while the worker wants one (svc/nn_handoff.h); the worker runs the
 * invoke, the decode and the publish.  There is no staging copy: the input the
 * model reads is the raw WDMA3 frame, which the pipeline's pin does not reach
 * and the datapath rewrites two frames later, so it is either copied or
 * prepared on the producer -- and the copy cost a quarter of the detector's
 * frames (spike, 2026-10-03).
 *
 * SINCE ISSUE #130 the hand-over itself is the shared frame path's
 * (svc/nn_core_frame.c): process() calls nn_core_on_frame() with the whole
 * frame as its one part, the worker ends a job through nn_core_on_infer_done(),
 * and draw() paints through nn_core_draw().  What stays here is this board's:
 * the prep, the invoke, the lease wait, the outputs and geometry, and every
 * counter.
 *
 * WHAT CROSSES.  The job below is written by the producer only while the
 * hand-over word is FILLING, and read by the worker only after it TAKEs it;
 * both transitions are critical sections, which are compiler barriers, and the
 * worker copies the job out before it does anything else.
 */
struct nn_ov_job {
	uint32_t gen;                  /* the record generation (issue #118)   */
	uint32_t prep_ticks;           /* the producer's prep, EPK ticks       */
	uint32_t t_hand;               /* EPK ticks at the hand-over           */
	struct nn_preproc_geom geom;   /* the transform this input was cut by  */
};

/*
 * What the latest publish of THIS stream was, for process()'s answer: draw this
 * frame or not (nn_ov_result below).
 */
#define NN_OV_RES_NONE 0u
#define NN_OV_RES_OK   1u
#define NN_OV_RES_FAIL 2u

/* ---- objects (defined in nn_camera.c) ------------------------------------- */

/* Written by nn_camera_request_stop() (1) and the arms (0), on the console;
 * read by the producer and the worker.  See nn_camera.c. */
extern volatile uint8_t nn_ov_stop;

/*
 * The stream counters.  Reset (all of them) by nn_camera_arm() only; read by
 * nn_camera_stats().
 *   frames, busy                    : the producer only
 *   inferences, detections, lease_timeouts, model_errors, decoder_errors,
 *   last_ms, last_ndet              : the worker only
 *   skipped, errors                 : the producer AND the worker -- see
 *                                     nn_ov_bump(); the worker's in-CS adds in
 *                                     nn_ov_account() are the same discipline
 */
extern struct nn_camera_stats nn_ov_stats;

/* The stage accumulators (issue #60): written by the worker only, inside a
 * critical section (nn_ov_account()); reset by nn_camera_arm(); read by
 * nn_camera_stats(). */
extern uint64_t nn_ov_prep_ticks;
extern uint64_t nn_ov_invoke_ticks;
extern uint64_t nn_ov_decode_ticks;
extern uint64_t nn_ov_cycle_ticks;
extern uint32_t nn_ov_prof_frames;
/* The last decode's status / count: written by the worker (nn_ov_account()),
 * reset by nn_camera_arm().  [!] READ BY NOTHING -- they were write-only
 * before issue #140 as well, when the compiler dropped them as unused
 * statics; the split only made them visible. */
extern int      nn_ov_last_status;
extern int      nn_ov_ndet;

/* The draw pair (issue #105): written by the panel only, both in one critical
 * section (nn_ov_paint()); reset by nn_camera_arm(); read by
 * nn_camera_stats(). */
extern uint32_t nn_ov_draw_spent;
extern uint32_t nn_ov_draw_refused;

/* Written by the producer's prep while the word is FILLING; copied out by the
 * worker after its TAKE. */
extern struct nn_ov_job nn_ov_job;

/* The frame path's state: stepped only through svc/nn_core_frame.c -- by the
 * producer (on_frame), the worker (take / on_infer_done / done), the panel
 * (draw / present_done) and the console (want / join / reset). */
extern struct nn_core_frame nn_ov_core;

/* Written by the worker (nn_ov_publish(), inside the lease); reset by the
 * arm; read by the producer as one byte. */
extern volatile uint8_t nn_ov_result;

/* Written by the arms only, while the worker is parked and nothing is
 * attached; read by the producer and the worker. */
extern uint8_t nn_ov_oneshot;
/* Written once per one-shot by the producer (before the hand-over) or the
 * worker (after its publish); reset by the arms; read by the console. */
extern volatile uint8_t nn_ov_shot;
/* Written by the worker (nn_ov_outputs()) before nn_ov_shot; reset by
 * nn_camera_arm_oneshot(); read by the console. */
extern volatile uint8_t nn_ov_shot_index;

/* ---- hooks the threads call across the files ------------------------------ */

/* nn_camera.c: the frame path's ops table, and the hooks in it that more than
 * one thread calls. */
extern const struct nn_core_frame_ops nn_ov_ops;

/* nn_producer.c: the overlay's process(), and the frame path's prep / wake. */
int  nn_camera_process(void *ctx, const void *pixels, uint16_t w, uint16_t h);
int  nn_ov_prep(void *ctx, unsigned part, unsigned nparts);
void nn_ov_infer_start(void *ctx);

/* nn_worker.c: the frame path's worker-side hooks (nn_camera_work() is in
 * nn_camera.h). */
int  nn_ov_admits(void *ctx, uint32_t gen);
int  nn_ov_publish_raw(void *ctx, uint32_t gen);
int  nn_ov_outputs(void *ctx);
int  nn_ov_decode(void *ctx, int *n);
int  nn_ov_publish(void *ctx, int n, uint32_t gen);
void nn_ov_account(void *ctx, enum nn_core_done what, int nd, int took);

/* nn_panel.c: the overlay's draw(). */
void nn_camera_draw(void *ctx, uint16_t *fb, uint16_t fb_w, uint16_t fb_h);

/* ---- helpers used by more than one thread --------------------------------- */

/*
 * [!] nn_ov_stats.skipped AND .errors HAVE TWO WRITERS -- the producer and the
 * worker -- so each of their increments outside a critical section goes
 * through this, one masked read-modify-write (issue #138).
 */
/* A counter both the producer and the worker write. */
static inline void nn_ov_bump(uint32_t *c)
{
	TX_INTERRUPT_SAVE_AREA

	TX_DISABLE
	(*c)++;
	TX_RESTORE
}

#endif /* NN_CAMERA_PRIV_H */
