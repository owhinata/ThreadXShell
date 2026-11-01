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
 * frame path's worker half (take -> nn_run() -> on_infer_done), the raw publish
 * ("nobody interprets these") when no plugin is in force, and -- with one (issue
 * #159) -- the plugin's decode and publish under the plugin lease, and this
 * board's counting.  Lease, then the record lock (nncam_lock), as everywhere.
 */
#include <stdint.h>

#include "tx_api.h"

#include "nn.h"
#include "nn_camera_priv.h"
#include "nn_desc.h"      /* nn_tensor -> tensor_desc (the raw publish)      */
#include "nn_det_record.h"
#include "nn_core_frame.h"   /* the shared frame path and its hand-over word (#130) */
#include "nn_sess_release.h" /* when the session may go back (#130)               */
#include "nn_decode_count.h" /* how a decode's answer is counted (#130 6c)         */
#include "cam_own.h"         /* the owner lifecycle (issue #72)                        */

#include "stm32f7xx_hal.h"   /* HAL_RCC_GetHCLKFreq */

#if defined(CONFIG_NN_BACKEND_TFLM)
#include "nn_active.h"       /* the plugin entries (issue #159) */
#include "plugin_lease.h"    /* the one lock into the plugin    */
#endif

#define LOG_TAG "nncam"
#include "log.h"

#define NNCAM_POLL_TICKS      100u        /* sem wait -> stop latency               */

/*
 * The one-shot's side of a job (issue #171), the worker's only:
 *   nncam_w_run      the run this worker serves, sampled under nncam_lock as it
 *                    enters its run loop (the start began it before raising
 *                    nncam_run, and begins the next only once this has parked)
 *   nncam_acc_what   what nn_core_on_infer_done() made of the last job, and
 *   nncam_acc_took   whether the record took it -- left by nncam_account() for
 *                    the record after the call returns, never recorded there
 */
static uint32_t          nncam_w_run;
static enum nn_core_done nncam_acc_what;
static int               nncam_acc_took;

/* ---- the frame path's publish / account (svc/nn/nn_core_frame.h, issue #130) -- */

/* One publish's counts (nn_decode_tally_of()): the per-attach and the
 * per-stream counters, and the detections it found.  The worker only. */
static void nncam_tally(enum nn_decode_tally t, int nd)
{
	switch (t) {
	case NN_TALLY_INFER:
		nnstat.detections = (nd > 0) ? (uint32_t)nd : 0u;
		nnstat.infers++;
		nncam_gen_infers++;
		return;
	case NN_TALLY_DECODER:
		nncam_error();
		nncam_gen_decoder_errors++;
		return;
	case NN_TALLY_MODEL:
		nncam_error();
		nncam_gen_model_errors++;
		return;
	case NN_TALLY_NOTHING:
	default:
		return;
	}
}

/*
 * Publish "an inference ran and nothing decoded it" (issue #159 Step 4c;
 * wio-lite-ai's since its issue #116).  The frame path calls this with no
 * plugin in force: this firmware carries no decoder, the outputs are there, and
 * only an interpretation of them is missing.  Under @p gen, the generation the
 * frame was prepared under -- [!] A RAW PUBLISH IS UNDER THE GENERATION RULE
 * TOO, or `nn run` on a bare model would wait for a result that never comes.
 * Non-zero if the record took it; the counting is nncam_account()'s.
 */
int nncam_publish(void *ctx, uint32_t gen)
{
	/*
	 * [!] THE OUTPUT SHAPES OF THE MODEL THAT RAN, TAKEN HERE (issue #121).
	 * This thread holds the session, so the model cannot change under it; the
	 * console that prints the report later holds nothing of the kind.  Built
	 * outside the lock; only the copy into the record is under it.  Static,
	 * not a local: only this thread builds it, and ~300 B is not something the
	 * worker's stack should carry for it.
	 */
	static struct nn_raw_outputs raw;
	int took, i, n;

	(void)ctx;
	n = nn_output_count(nncam_model);
	raw.count = n;
	raw.n = 0u;
	for (i = 0; i < n && i < NN_RAW_OUTPUTS_MAX; i++) {
		struct nn_tensor *t = nn_output(nncam_model, i);

		if (t == NULL)
			break;
		nn_desc_of(&raw.out[i], t);
		raw.n = (uint8_t)(i + 1);
	}

	tx_mutex_get(&nncam_lock, TX_WAIT_FOREVER);
	took = nn_det_record_publish_raw(&nncam_rec, gen, &raw);
	tx_mutex_put(&nncam_lock);
	return took;
}

#if defined(CONFIG_NN_BACKEND_TFLM)
/*
 * The plugin's half (issue #159; wio-lite-ai's since #110).  Called by the
 * frame path with the plugin lease held -- nn_core_on_infer_done() checks it.
 */

/* Is a decode armed at @p gen still going to be published?  Asked UNDER THE
 * LEASE, before the plugin rewrites its private result (issue #118). */
int nncam_admits_hook(void *ctx, uint32_t gen)
{
	int ok;

	(void)ctx;
	tx_mutex_get(&nncam_lock, TX_WAIT_FOREVER);
	ok = nn_det_record_admits(&nncam_rec, gen);
	tx_mutex_put(&nncam_lock);
	return ok;
}

/* Where a plugin's decode() is called.  Its depth is sampled inside, at the
 * indirect call (svc/nn/nn_active_core.c). */
int nncam_decode(void *ctx, int *n)
{
	int nd = nn_active_decode(nncam_model);

	(void)ctx;
	/* NN_ACTIVE_NOT_HELD: not reachable while the worker's take stands; if it
	 * ever is, the decode did not run and nothing is published (counted by
	 * the entry check, plugin_lease_unheld(), not as a worker error). */
	if (nd == NN_ACTIVE_NOT_HELD)
		return -1;
	*n = nd;
	return 0;
}

/*
 * The count and the generation rule travel; the result stays with the plugin.
 * [!] CALLED WITH THE LEASE STILL HELD: releasing it between the decode and
 * this would let the GUIX draw pair the new private state with the old record.
 */
int nncam_publish_hook(void *ctx, int n, uint32_t gen)
{
	int took;

	(void)ctx;
	tx_mutex_get(&nncam_lock, TX_WAIT_FOREVER);
	took = nn_det_record_publish_external(&nncam_rec, n, gen);
	tx_mutex_put(&nncam_lock);
	return took;
}
#endif

/*
 * This board's counting, wio-lite-ai's way: a publish the record took is an
 * inference -- unless a plugin's decoder refused it, which is an error counted
 * apart by nn_decode_count_of() -- and one the generation rule dropped is not
 * counted, and neither is its decode (#122 P18).  A RAW publish found nothing
 * and refused nothing: an inference with no detections.
 */
void nncam_account(void *ctx, enum nn_core_done what, int n, int took)
{
	(void)ctx;
	/* For the one-shot's record after nn_core_on_infer_done() (issue #171). */
	nncam_acc_what = what;
	nncam_acc_took = took;
	if (what == NN_CORE_DONE_RAW) {
		nncam_tally(nn_decode_tally_of(took, NN_DC_NONE), 0);
		return;
	}
	if (what != NN_CORE_DONE_DECODED)
		return;
	nncam_tally(nn_decode_tally_of(took, nn_decode_count_of(n,
	                                         nncam_oneshot)), n);
}

/* ---- the one-shot's end of a job (issue #171, svc/nn/nn_shot.h) ------------- */

/* Arm for another frame of the one-shot: only while its run is still open (the
 * record's answer, @p open) AND nncam_run is still up -- read HERE, after the
 * record, so a stop that came in before it is seen.  A stop that lands after
 * this read finds WANT, and its JOIN (or the next start's) parks it, as for a
 * stream. */
static void nncam_shot_rearm(int open)
{
	if (open && nncam_run)
		(void)nn_core_frame_want(&nncam_frame, &nncam_frame_ops);
}

/*
 * A job taken and ended before nn_core_on_infer_done(): nn_run() failed
 * (@p why INVOKE) or the lease was not had (LEASE_TIMEOUT).  A stream goes back
 * to WANT while it runs, as it always did.  A one-shot records why, in ONE
 * hold of nncam_lock with the question "is the generation the frame was
 * prepared under (@p gen) still the record's" -- a detach or a stop that moved
 * it first makes this frame the boundary's, not the run's end, and the worker
 * arms for the next one -- then parks the word (one frame per job: DONE_LAST)
 * and re-arms only if the run is still open.
 */
static void nncam_job_failed(int oneshot, enum nn_shot_reason why, uint32_t gen)
{
	int open;

	if (!oneshot) {
		nn_core_frame_done(&nncam_frame, &nncam_frame_ops, nncam_run ? 1 : 0);
		return;
	}
	tx_mutex_get(&nncam_lock, TX_WAIT_FOREVER);
	open = nn_shot_record(&nncam_shot, nncam_w_run, why,
	                      nn_det_record_admits(&nncam_rec, gen));
	tx_mutex_put(&nncam_lock);
	nn_core_frame_done(&nncam_frame, &nncam_frame_ops, 0);
	nncam_shot_rearm(open);
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
	/* Set by the start before nncam_run was raised; fixed while it is up. */
	const int oneshot = nncam_oneshot;
	uint32_t gen;
	int open;

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
		nncam_job_failed(oneshot, NN_SHOT_INVOKE, gen);
		return;
	}
	/* Counted in nncam_account(), once the publish has answered (#138). */
	nnstat.last_us = nn_last_cycles(nncam_model) / mhz;

#if defined(CONFIG_NN_BACKEND_TFLM)
	/*
	 * [!] THE LEASE SPANS THE DECODE AND THE PUBLISH, taken BEFORE the slot is
	 * looked up (issue #159, wio-lite-ai's #110).  The GUIX thread outranks
	 * this one and nothing else separates them, so "ask the plugin, then say
	 * what it answered" has to be one transaction.  A wait, bounded by the
	 * lease's one bound: a wedged holder costs frames, not the worker.  The
	 * frame path checks the hold and gives it back; with no plugin there is
	 * nothing to hold.
	 */
	if (nn_active_is_plugin() && !plugin_lease_take()) {
		nncam_error();
		nncam_job_failed(oneshot, NN_SHOT_LEASE_TIMEOUT, gen);
		return;
	}
#endif

	/*
	 * The decode and the publish under the generation the frame was prepared
	 * under (a plugin's, or nncam_publish()'s raw one), then the word back
	 * to WANT -- or to IDLE when a stop has come in, so a stop that finds the
	 * worker parked finds nothing armed.  [!] After the outputs are read for the
	 * last time, never before: from WANT the producer may write the input again.
	 *
	 * [!] A ONE-SHOT PARKS THE WORD AFTER EVERY JOB (issue #171): one frame
	 * per job, and whether there is to be another is the run's answer, not
	 * nncam_run's.  The account left what became of the job; the record
	 * takes it in ONE hold of nncam_lock with the generation question -- a
	 * publish the record took ends the run whatever the generation, a job a
	 * boundary retired does not end it -- and the worker re-arms only on a
	 * run still open.  A stream keeps going back to WANT while it runs.
	 */
	nn_core_on_infer_done(&nncam_frame, &nncam_frame_ops, NULL, gen,
	                      oneshot ? 0 : (nncam_run ? 1 : 0));
	if (!oneshot)
		return;
	tx_mutex_get(&nncam_lock, TX_WAIT_FOREVER);
	open = nn_shot_after_job(&nncam_shot, nncam_w_run, nncam_acc_what,
	                         nncam_acc_took,
	                         nn_det_record_admits(&nncam_rec, gen));
	tx_mutex_put(&nncam_lock);
	nncam_shot_rearm(open);
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
		/* The one-shot's run, begun by the start before nncam_run was raised
		 * (issue #171): the one this worker records against until it parks. */
		if (nncam_oneshot) {
			tx_mutex_get(&nncam_lock, TX_WAIT_FOREVER);
			nncam_w_run = nn_shot_open(&nncam_shot);
			tx_mutex_put(&nncam_lock);
		}
		/* Ask for a frame (IDLE -> WANT).  nn_camera_start() parked the word
		 * before raising nncam_run, so this is the arm a stream begins with;
		 * refused only if a word left armed by the last session is still
		 * armed, which asks for the same thing.  A one-shot arms only on an
		 * open run -- always so here, nothing can end it before this arm. */
		if (!nncam_oneshot || nncam_w_run != 0u)
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
