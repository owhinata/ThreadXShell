/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Wio Lite AI ThreadX Shell Project
 */
/**
 * @file    nn_worker.c
 * @brief   The inference worker's (`nn_work`, priority 18) half of the camera ->
 * inference glue (issue #140; see nn_camera.h for the design and nn_camera_priv.h
 * for the state).
 *
 * Everything here runs on the worker: arm, wait, take, nn_run(), then -- under
 * the result lease when a plugin is loaded -- the generation question, the
 * decode and the publish, and this board's counting.  Lease, then the record
 * lock, as everywhere.
 */
#include "nn_camera_priv.h"
#include "nncam_teardown.h"  /* the worker's give (#142 review) */

#include "camera.h"          /* camera_stream_lost(): nn's lost mark (#142) */
#include "nn.h"
#include "nn_active.h"
#include "plugin_lease.h"
#include "nn_det_record.h"
#include "nn_core_frame.h"   /* the shared frame path (issue #130) */
#include "nn_desc.h"         /* nn_tensor -> tensor_desc (issue #121) */
#include "nn_decode_count.h" /* how a decode's answer is counted (#130 6c) */

#include "tx_api.h"

/*
 * How long this thread waits for the result lease before giving up on a frame
 * (issue #110) is the lease's own one bound since issue #130:
 * PLUGIN_LEASE_WAIT_MS in port/plugin/plugin_lease.h.  Generous against the
 * things that legitimately hold it -- a console capturing a report, a panel
 * drawing -- and finite so that a wedged holder costs frames rather than the
 * worker.
 */
/*
 * How long the worker waits for a frame before re-checking the run flag.  Without a
 * bound it would wedge on a semaphore nothing will post: a DCMI overrun or a stream
 * that stops underneath ends the band flow with the worker still wanting a frame.
 */
#define NNCAM_FRAME_WAIT_TICKS 100u

/* ---------------------------------------------------------------- worker ------ */

/*
 * Publish "an inference ran and nothing decoded it", under the detection lock
 * (issue #116).
 *
 * This is what the worker has to say for a model with no plugin beside it: this
 * firmware carries no decoder, the outputs are there, and only an
 * interpretation of them is missing.
 *
 * @return non-zero if it was taken.  A publish from a session that has since
 *         ended is DROPPED -- svc/nn/nn_det_record.c has the rule and the reason --
 *         and the caller must not count it, because `nn run` waits on the
 *         inference counter and then reads the record.
 */
static int nncam_publish_raw(uint32_t gen)
{
	/*
	 * [!] THE OUTPUT SHAPES OF THE MODEL THAT RAN, TAKEN HERE (issue #121).
	 * This thread holds the session, so the model cannot change under it; the
	 * console that prints the report later holds nothing of the kind, and
	 * used to read the shapes of whatever model was open by then.  Built
	 * outside the lock; only the copy into the record is under it.  Static,
	 * not a local: only this thread builds it, and 300 B is not something
	 * nn_work's stack should carry for the one path that needs it.
	 */
	static struct nn_raw_outputs raw;
	int took, i, n;

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

	if (tx_mutex_get(&nncam_det_lock, TX_WAIT_FOREVER) != TX_SUCCESS)
		return 0;
	took = nn_det_record_publish_raw(&nncam_rec, gen, &raw);
	(void)tx_mutex_put(&nncam_det_lock);
	return took;
}

#if defined(CONFIG_NN_BACKEND_TFLM)
/*
 * The same, for a decode whose RESULT STAYED WITH THE PLUGIN (issue #110).
 *
 * What travels is the count and the generation rule; the boxes and the shared
 * decoder's diagnostics do not, because they describe a decoder that did not
 * run.
 *
 * [!] CALLED WITH THE RESULT LEASE STILL HELD.  Releasing it between the decode
 * and this would let the panel see the new private state paired with the old
 * record -- a count from the previous frame beside a picture from this one,
 * which is exactly the pairing the lock in here exists to prevent.
 */
static int nncam_publish_plugin(int n, uint32_t gen)
{
	int took;

	if (tx_mutex_get(&nncam_det_lock, TX_WAIT_FOREVER) != TX_SUCCESS)
		return 0;
	took = nn_det_record_publish_external(&nncam_rec, n, gen);
	(void)tx_mutex_put(&nncam_det_lock);
	return took;
}
#endif

#if defined(CONFIG_NN_BACKEND_TFLM)
/* Is a decode armed at @p gen still going to be published?  Asked by the worker
 * UNDER THE LEASE, before it lets a plugin rewrite its private result -- see
 * nncam_record_boundary() and nn_det_record_admits(). */
static int nncam_admits(uint32_t gen)
{
	int ok;

	if (tx_mutex_get(&nncam_det_lock, TX_WAIT_FOREVER) != TX_SUCCESS)
		return 0;
	ok = nn_det_record_admits(&nncam_rec, gen);
	(void)tx_mutex_put(&nncam_det_lock);
	return ok;
}
#endif

/* The generation the worker remembers when it ARMS -- see nn_det_record.h for
 * why sampling it after the wait would leave the re-arm boundary open. */
static uint32_t nncam_gen_now(void)
{
	uint32_t g;

	if (tx_mutex_get(&nncam_det_lock, TX_WAIT_FOREVER) != TX_SUCCESS)
		return 0u;
	g = nn_det_record_gen(&nncam_rec);
	(void)tx_mutex_put(&nncam_det_lock);
	return g;
}

/* ---- the frame path's hooks (svc/nn/nn_core_frame.h, issue #130) ------------- */

int nncam_publish_raw_hook(void *ctx, uint32_t gen)
{
	(void)ctx;
	return nncam_publish_raw(gen);
}

#if defined(CONFIG_NN_BACKEND_TFLM)
int nncam_admits_hook(void *ctx, uint32_t gen)
{
	(void)ctx;
	return nncam_admits(gen);
}

/* Where a plugin's decode() is called (issue #110).  Its depth is sampled
 * inside, at the indirect call (svc/nn/nn_active_core.c, issue #126). */
int nncam_decode(void *ctx, int *n)
{
	int nd = nn_active_decode(nncam_model);

	(void)ctx;
	/* [!] NN_ACTIVE_NOT_HELD: not reachable while the worker's take stands --
	 * and if it ever does not, the decode did not run, so nothing is
	 * published (issue #130, as on Grove).  Counted by the entry check
	 * (plugin_lease_unheld()), not as a worker error: the decoder was never
	 * asked. */
	if (nd == NN_ACTIVE_NOT_HELD)
		return -1;
	*n = nd;
	return 0;
}

int nncam_publish_hook(void *ctx, int n, uint32_t gen)
{
	(void)ctx;
	return nncam_publish_plugin(n, gen);
}
#endif

/*
 * This board's counting.  A publish the record took is an inference -- unless
 * the decoder refused it; one the generation rule dropped is not counted, and
 * neither is its decode.  A decode the plugin refused is counted apart by
 * nn_decode_count_of() (issue #130 step 6c, P8: grove-vision-ai-v2's table) --
 * there is no console on this thread, so a refusal not counted apart cannot be
 * told apart afterwards (issue #97).
 *
 * [!] A REFUSED DECODE IS AN ERROR AND NOT AN INFERENCE (issue #138), as on
 * every board: `infers` counts the results a reader can use, and a refusal
 * counted in both would make `infers + errors` count it twice.
 */
void nncam_account(void *ctx, enum nn_core_done what, int n, int took)
{
	(void)ctx;
	if (!took || (what != NN_CORE_DONE_RAW && what != NN_CORE_DONE_DECODED))
		return;
	if (what == NN_CORE_DONE_DECODED) {
		switch (nn_decode_count_of(n, nncam_oneshot)) {
		case NN_DC_MODEL:
			nncam_model_errors++;
			nncam_error();
			return;
		case NN_DC_DECODER:
			nncam_decoder_errors++;
			nncam_error();
			return;
		case NN_DC_NONE:
		default:
			break;
		}
	}
	/* RAW: no plugin, nothing decoded, nothing refused. */
	nncam_infers++;
}

/* ----------------------------------------------------------- worker loop ------ */

static void nncam_step(int first)
{
	uint32_t gen;
	unsigned drained = 0u;

	/*
	 * [!] DISCARD ANY POST THAT PREDATES THIS ARM, AND DO IT BEFORE ARMING.
	 *
	 * Before issue #130 a stale post was enough to break the ownership
	 * invariant FOREVER: the wait returned at once, the inference started with
	 * want_frame already set, the next band 0 began filling underneath it, and
	 * the state sustained itself.  The shared word closes that by
	 * construction -- a wake-up runs nothing unless it can TAKE a frame handed
	 * over, and nothing can begin a fill while one is RUNNING -- so the drain
	 * is now about WHICH frame runs, and it keeps the answer it always gave.
	 *
	 * The window that produces the stale post is real and routine: this wait times out
	 * after NNCAM_FRAME_WAIT_TICKS (100 ms) while a fill can legitimately take up to
	 * two frame periods (~148 ms) from arming, so the producer's post and the timeout
	 * can land together.  A frame whose post is drained here was handed over under
	 * the PREVIOUS arm, and it is thrown away with its post: discarding it costs
	 * one frame, and this arm samples its own generation below.  The first step of
	 * a session drops a frame left over from the last one the same way -- its
	 * post went with the start's own drain.
	 */
	while (tx_semaphore_get(&nncam_frame_sem, TX_NO_WAIT) == TX_SUCCESS) {
		nncam_stale_posts++;
		drained++;
	}
	if (first || drained != 0u)
		(void)nn_core_frame_discard(&nncam_frame, &nncam_frame_ops);

	/*
	 * [!] THE GENERATION IS SAVED HERE, at the arm, and not after the wait.
	 * Saving it once the semaphore returns would leave the re-arm boundary open:
	 * a stop and a fresh start could both happen while this thread sits in the
	 * wait, and the value read afterwards would be the NEW session's -- so the
	 * old frame would publish into it looking current.  Hence this worker ends
	 * every job parked (DONE_LAST) and arms itself here, after the sample.
	 */
	gen = nncam_gen_now();
	(void)nn_core_frame_want(&nncam_frame, &nncam_frame_ops);
	if (tx_semaphore_get(&nncam_frame_sem, NNCAM_FRAME_WAIT_TICKS) != TX_SUCCESS) {
		/* No frame within the bound.  The band flow may have ended without ever
		   posting -- a DCMI overrun ended the stream, and nn's holder was
		   marked lost (issue #142) -- in which case there is nothing left to
		   wait for.  Drop the fill with it (JOIN): the stream may have died part
		   way through a frame, and leaving it FILLING would let a re-armed
		   stream resume that frame from whichever band arrives first.  While
		   nn is lost the producer does not enter nn's step (nncam_band() returns
		   at its first line, even on a stream the operator restarted), so this
		   is one place it can safely be dropped.  Otherwise a fill in flight is
		   NOT taken away: the next step waits for it again. */
		if (camera_stream_lost(&nncam_holder))
			(void)nn_core_frame_join(&nncam_frame, &nncam_frame_ops);
		return;
	}
	if (!nncam_run)
		return;
	/* [!] A wake-up is not a job.  Only a hand-over this thread can TAKE is
	 * one; a post whose frame was already discarded above is stale. */
	if (!nn_core_frame_take(&nncam_frame, &nncam_frame_ops)) {
		nncam_stale_posts++;
		return;
	}

	/* RUNNING: while the word says so, NOTHING may write the input tensor. */
	if (nn_run(nncam_model) != 0) {
		nncam_error();
		nn_core_frame_done(&nncam_frame, &nncam_frame_ops, 0);
		return;
	}
	nncam_infer_cyc = nn_last_cycles(nncam_model);

#if defined(CONFIG_NN_BACKEND_TFLM)
	/*
	 * [!] THE LEASE SPANS THE DECODE AND THE PUBLISH, and it is taken
	 * BEFORE the slot is looked up.  The panel runs at a higher priority
	 * than this thread and nothing else separates them, so every part of
	 * "ask the plugin, then say what it answered" has to be one
	 * transaction: a panel that preempted between them would draw from
	 * state this call is halfway through writing, or pair a new count with
	 * an old picture.
	 *
	 * A wait, not a try: this thread is the one that has work to do, and
	 * the only other holders are a console command and a draw that does
	 * not wait.  The bound keeps a wedged holder from taking the worker
	 * with it.  The frame path checks the hold and gives it back; with no
	 * plugin there is nothing to hold.
	 */
	if (nn_active_is_plugin() && !plugin_lease_take()) {
		nncam_error();
		nn_core_frame_done(&nncam_frame, &nncam_frame_ops, 0);
		return;
	}
#endif
	/*
	 * The generation question, the decode and the publish under the lease --
	 * or, with no plugin, the publish of an inference nothing decoded (issue
	 * #116), which `nn run` waits on as much as on a decode.  Then this
	 * board's count and the word back to IDLE: the next step arms again.
	 */
	nn_core_on_infer_done(&nncam_frame, &nncam_frame_ops, NULL, gen, 0);
}

void nncam_entry(ULONG arg)
{
	int first;

	(void)arg;
	for (;;) {
		if (tx_semaphore_get(&nncam_start_sem, TX_WAIT_FOREVER) != TX_SUCCESS)
			continue;

		/* Set BEFORE the loop: while this is set the worker may be touching the
		   tensors, and nn_camera_stop() must not conclude otherwise.  A stop that
		   lands in the window between here and the test below simply finds the
		   flag clear, and the loop it is racing never executes a single step. */
		nncam_worker_busy = 1;
		for (first = 1; nncam_run; first = 0)
			nncam_step(first);

		/* If a stop gave up waiting for us it left the guards held on purpose --
		   see nn_camera_stop().  We are now the last one out, so we release.
		   [!] BEFORE clearing the busy flag (issue #142 review): with the order
		   the other way round, a console stop could see the flag clear, release
		   the old session itself, and a NEW start take the guards -- and this
		   give, arriving late, would hand back the new session's.  Given first,
		   no start can see this worker idle while its give is still to come. */
		/* [!] Not over a band step no stop has seen drained (issue #142
		   review, nncam_teardown.h): the producer may still be writing the
		   input tensor, so the session stays held and the stop whose drain
		   succeeds gives it back. */
		{
			struct nncam_td td = { 0, 0, 0, nncam_drain_pending != 0 };

			if (nncam_td_worker_gives(&td))
				nncam_guards_give();
		}
		nncam_worker_busy = 0;
	}
}
