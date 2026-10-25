/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    nn_worker.c
 * @brief   The inference worker thread's half of the camera -> inference glue
 * (issue #140; see nn_camera.c for the design and nn_camera_priv.h for the state).
 *
 * Everything here runs on the worker (priority 18): the thread itself, the
 * frame path's worker half (take -> nn_run() -> on_infer_done), and the
 * resident decoder's publish with the counting that goes with it.
 */
#include <stdint.h>

#include "tx_api.h"

#include "nn.h"
#include "nn_camera_priv.h"
#include "nn_decoder.h"   /* the shared BlazeFace decoder, via this board's adapter */
#include "nn_det_record.h"
#include "nn_core_frame.h"   /* the shared frame path and its hand-over word (#130) */
#include "nn_sess_release.h" /* when the session may go back (#130)               */
#include "nn_decode_count.h" /* how a decode's answer is counted (#130 6c)         */
#include "nn_top.h"          /* the classes, taken before the next inference (#121) */
#include "cam_own.h"         /* the owner lifecycle (issue #72)                        */

#include "stm32f7xx_hal.h"   /* HAL_RCC_GetHCLKFreq */

#define LOG_TAG "nncam"
#include "log.h"

#define NNCAM_POLL_TICKS      100u        /* sem wait -> stop latency               */

/* ---- the frame path's publish / account (svc/nn/nn_core_frame.h, issue #130) -- */

/*
 * The publish of one inference -- on this board, THE RESIDENT DECODER'S.  The
 * frame path hands a board with no plugin mechanism this slot (its decode slot is
 * the plugin's, reached only under the plugin lease, which this board does not
 * have), so the BlazeFace decode that has always run here runs here: on the
 * worker, while the word still says RUNNING -- the output tensors are read for
 * the last time inside this call -- and published under @p gen, the generation
 * the frame was prepared under.  Non-zero if the record took it.
 */
int nncam_publish(void *ctx, uint32_t gen)
{
	struct bf_det tmp[BF_MAX_DET];
	struct bf_result bfr;
	struct nn_top5 top;
	int nd, took;

	(void)ctx;
	/* Model-specific decode (BlazeFace).  A safe no-op (returns BF_ERR_MODEL) for
	 * other models, so this stays model-agnostic at the sink level.  Publish the
	 * boxes and the diagnostics that go with them under the lock. */
	nd = nn_decoder_run(nncam_model, tmp, BF_MAX_DET, &bfr);

	/*
	 * [!] NOT A DETECTOR: THE CLASSES ARE TAKEN NOW (issue #121, D3).
	 * This thread is the only reader of the output tensor while the word says
	 * RUNNING, and the producer may write the input again -- which can share
	 * the arena with the outputs -- the moment it does not, so this is the one
	 * moment its classes are still this result's.  The shared command used to
	 * read them at print time -- after a stream stop, from the frame whose
	 * publish had been dropped.  The walk is outside nncam_lock; only the copy
	 * is under it.
	 */
	if (nd == BF_ERR_MODEL) {
		struct tensor_desc d;
		struct nn_tensor *o = nn_output(nncam_model, 0);

		if (o != NULL) {
			nn_decoder_desc(&d, o);
			nn_top_of(&d, &top);
		} else {
			nn_top_of(NULL, &top);
		}
	}

	tx_mutex_get(&nncam_lock, TX_WAIT_FOREVER);
	/*
	 * [!] A NEGATIVE RETURN IS NO LONGER FOLDED INTO ZERO (issue #57/#97).
	 * It used to be, and that was the worst place for it: "0 faces" reads
	 * as a measurement, so a decoder that had never been initialised --
	 * a build fault, permanent, on every frame -- would have shown up as a
	 * perfectly healthy stream finding nobody.  The record keeps -1, and
	 * the status with it -- and since issue #118 it keeps its own code:
	 * BF_ERR_UNINIT is not BF_ERR_MODEL.
	 */
	/* [!] WHETHER IT WAS TAKEN IS THE RECORD'S TO COUNT (issue #118).
	 * `infers` counts inferences and keeps that meaning; `nn run` waits on
	 * the record's accepted count, which moves here, under this lock, only
	 * for a publish the generation rule took. */
	took = nn_det_record_publish(&nncam_rec, tmp, nd, &bfr, gen,
	                             (nd == BF_ERR_MODEL) ? &top : NULL);
	tx_mutex_put(&nncam_lock);
	nnstat.detections = (nd > 0) ? (uint32_t)nd : 0u;
	/* [!] A REFUSAL IS COUNTED APART (issue #130 step 6c, P8), HERE, because
	 * this is where the decoder's answer is: the frame path hands this slot no
	 * count to pass on.  BF_ERR_MODEL is the top-5 result above, not a
	 * refusal (D6); any other negative is a wiring fault.  Counted like the
	 * inference it belongs to -- whether or not the record took it.
	 * [!] AN ERROR AND NOT AN INFERENCE (issue #138), as on every board:
	 * `infers` counts the results a reader can use. */
	if (nn_decode_count_of(nd, nncam_oneshot) == NN_DC_DECODER) {
		nncam_error();
		nncam_gen_decoder_errors++;
	} else {
		nnstat.infers++;
		nncam_gen_infers++;
	}
	return took;
}

/* This board's counting is done in the publish, where the decoder's answer is:
 * the inference, the detections and a decoder refusal (issue #138 moved the
 * inference there from nn_run()'s return).  Nothing is left to count here. */
void nncam_account(void *ctx, enum nn_core_done what, int n, int took)
{
	(void)ctx;
	(void)what;
	(void)n;
	(void)took;
}

/* ---- inference worker ----------------------------------------------------- */

/*
 * Run the frame handed over, if there is one (issue #130: the shared frame
 * path's worker half).  The wait is a WAKE-UP, NOT A JOB: whether it returned a
 * post or timed out, only a TAKE says there is a frame to run -- a post can be
 * stale (a stop's, a detach's, or one whose frame was already taken), and a frame
 * handed over can outlast its post (an attach drains the semaphore).
 */
static void nncam_step(void)
{
	uint32_t hclk = HAL_RCC_GetHCLKFreq();
	uint32_t mhz = hclk / 1000000u ? hclk / 1000000u : 1u;
	uint32_t gen;

	(void)tx_semaphore_get(&nncam_sem, NNCAM_POLL_TICKS);
	if (!nncam_run)
		return;                             /* a stop: leave what was handed over   */
	if (!nn_core_frame_take(&nncam_frame, &nncam_frame_ops))
		return;
	/* The producer's, sampled before it wrote the input; the TAKE orders it. */
	gen = nncam_job_gen;

	/* RUNNING: while the word says so, NOTHING may write the input tensor. */
	if (nn_run(nncam_model) != 0) {
		nncam_error();
		nn_core_frame_done(&nncam_frame, &nncam_frame_ops, nncam_run ? 1 : 0);
		return;
	}
	/* Counted in the publish, once the decoder has answered (issue #138). */
	nnstat.last_us = nn_last_cycles(nncam_model) / mhz;

	/*
	 * The decode and the publish under the generation the frame was prepared
	 * under (nncam_publish(), this board's resident decoder), then the word back
	 * to WANT -- or to IDLE when a stop has come in, so a stop that finds the
	 * worker parked finds nothing armed.  [!] After the outputs are read for the
	 * last time, never before: from WANT the producer may write the input again.
	 */
	nn_core_on_infer_done(&nncam_frame, &nncam_frame_ops, NULL, gen,
	                      nncam_run ? 1 : 0);
}

void nncam_entry(ULONG arg)
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
		/* Ask for a frame (IDLE -> WANT).  nn_camera_start() parked the word
		 * before raising nncam_run, so this is the arm a stream begins with;
		 * refused only if a word left armed by the last session is still
		 * armed, which asks for the same thing. */
		(void)nn_core_frame_want(&nncam_frame, &nncam_frame_ops);
		LOG_INF("inference running (prio %u)", (unsigned)NNCAM_WORKER_PRIORITY);
		/* Run while enabled.  A base detach (producer_dead) is a PAUSE, not a stop
		 * (owhinata/stm32f746g-disco#100): no frames flow so the worker just idles on the poll
		 * timeout until the base re-attaches (nncam_open re-arms the session).  Only
		 * nn_camera_stop clears nncam_run, so the session release stays with the AI intent
		 * owner. */
		while (nncam_run)
			nncam_step();
		/* [!] THE SESSION GOES BACK BEFORE WE ANNOUNCE THAT WE PARKED, and the
		 * order is the correctness (issue #72).  nncam_active = 0 is what a stop
		 * waits on; the moment it is visible the stop may release the session,
		 * commit the lifecycle to IDLE and let a NEW `nn stream start` acquire a
		 * fresh session -- and if this release were still to come, it would then
		 * hand back the new stream's session, because the holds flag it reads is
		 * the one that start has just set.  Releasing first makes that
		 * impossible: after this line we own nothing a later start could be
		 * given.
		 *
		 * The run loop exits only on nncam_run=0 (an `nn stream stop`) -- a base
		 * detach is a pause that keeps run=1.  Releasing here is what frees the
		 * session even when nn_camera_stop() timed out mid-nn_run() (idempotent
		 * with nn_camera_stop's own release via the holds flag).  [!] Only once
		 * that stop's sink drain succeeded (issue #130): before it, the producer
		 * may still be writing the input tensor, and the hold stays for the
		 * stop that confirms the drain. */
		nncam_release_session(NN_SESS_WORKER_OUT);
		nncam_active = 0;                   /* parked; nn_camera_stop() waits on this */
		/* And finish the lifecycle if a stop gave up waiting for us: it committed
		 * SETTLING and returned -2, so this parking is the event that lets a later
		 * `nn stream start` back in (issue #72).  A no-op in every other state --
		 * a stop still inside its own drain commits its own result, and must not
		 * have it overwritten from here. */
		cam_own_settle(&nncam_own);
		LOG_INF("inference stopped (%lu infers)", (unsigned long)nnstat.infers);
	}
}
