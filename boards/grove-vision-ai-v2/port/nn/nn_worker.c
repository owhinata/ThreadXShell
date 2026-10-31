/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    nn_worker.c
 * @brief   The inference worker thread and its join (issue #129), and since
 *          issue #140 the worker's half of the camera -> inference glue:
 *          nn_camera_work() (invoke, plugin lease wait) and the frame path's
 *          outputs / decode / publish / account hooks.  See nn_worker.h; the
 *          hand-over decisions are svc/nn/nn_handoff.c's, run by
 *          svc/nn/nn_core_frame.c (issue #130).  The shared state is
 *          nn_camera_priv.h's, defined in nn_camera.c.
 */
#include "nn_worker.h"

#include <stddef.h>

#include "tx_api.h"

#include "cam_lcd_sink.h"   /* CAM_PANEL_PRIO                       */
#include "camera.h"         /* CAM_PRODUCER_PRIO                    */
#include "cli_config.h"     /* CLI_INSTANCE_{PRIORITY,STACK_SIZE}   */
#include "nn_camera.h"     /* nn_camera_work(): the job; the word */
#include "nn_camera_priv.h" /* the glue's shared state (issue #140)  */
#include "npu_hw.h"         /* NPU_INFERENCE_TIMEOUT_TICKS          */
#include "plugin_lease.h"   /* PLUGIN_LEASE_WAIT_MS                 */
#include "nn_active.h"
#include "nn_core_frame.h"  /* the shared frame path (issue #130)   */
#include "nn_outputs.h"     /* what the worker does with the outputs (#129) */
#include "nn_rec.h"
#include "nn.h"
#include "nn_desc.h"
#include "tx_glue.h"        /* the EPK's TIMER2: the stage clock (issue #60) */

#define LOG_TAG "nn_worker"
#include "log.h"

_Static_assert(CAM_PANEL_PRIO < CAM_PRODUCER_PRIO &&
               CAM_PRODUCER_PRIO < NN_WORKER_PRIO &&
               NN_WORKER_PRIO < (unsigned)CLI_INSTANCE_PRIORITY,
               "panel < producer < worker < console (issue #129, option C); "
               "worker < mve is asserted in cmds/cmd_mve.c");
_Static_assert(NN_WORKER_STACK_BYTES >= (unsigned)CLI_INSTANCE_STACK_SIZE,
               "the worker becomes the decode's shallowest ceiling: never "
               "below the console's stack (issue #129)");
_Static_assert((NN_WORKER_STACK_BYTES % 8u) == 0u,
               "AAPCS stack alignment");

/*
 * How long a stop waits for the worker, in ticks of the wall clock.
 *
 * Derived, not chosen, from what a worker that is behaving can be inside of
 * when the stop comes in:
 *   - one invoke: the ethos-u driver can take its inference semaphore TWICE on
 *     its timeout/interrupt race path, each bounded by
 *     NPU_INFERENCE_TIMEOUT_TICKS (npu_hw.h)              2 x 1000 ticks
 *   - the plugin lease, which the worker takes bounded     PLUGIN_LEASE_WAIT_MS
 *   - the decode (tens of microseconds) and the time the producer and the
 *     panel preempt it, both above this thread             NN_WORKER_JOIN_SLACK
 * A stop pending means the worker does not START an invoke, so this covers at
 * most the one already running.
 *
 * [!] THE NUMBER IS NOT THE SAFETY.  What keeps a late worker harmless is the
 * state: a join that runs out is the terminal "worker did not return", and
 * nothing it may still touch is unlinked or released.  This only decides how
 * long an operator waits for that verdict.
 */
#define NN_WORKER_JOIN_SLACK_MS 250u
#define NN_WORKER_MS_TO_TICKS(ms) \
	(((ms) * (unsigned)TX_TIMER_TICKS_PER_SECOND + 999u) / 1000u)
#define NN_WORKER_JOIN_TICKS                                  \
	(2u * NPU_INFERENCE_TIMEOUT_TICKS +                      \
	 NN_WORKER_MS_TO_TICKS(PLUGIN_LEASE_WAIT_MS) +            \
	 NN_WORKER_MS_TO_TICKS(NN_WORKER_JOIN_SLACK_MS))

/* Event flag bits.  Wake-ups only: the word below is the truth. */
#define NN_WK_WAKE    0x1u   /* producer -> worker: something was handed over */
#define NN_WK_SETTLED 0x2u   /* worker -> stop: it went back to parked        */

static TX_THREAD             nn_worker_thread;
static UCHAR                 nn_worker_stack[NN_WORKER_STACK_BYTES]
	__attribute__((aligned(8)));
static TX_EVENT_FLAGS_GROUP  nn_worker_flags;
static volatile int          nn_worker_ok;

/* The hand-over word (svc/nn/nn_handoff.h) is the overlay's frame-path state
 * since issue #130 -- svc/nn/nn_core_frame.c steps it, one critical section per
 * transition -- and this file reaches it through nn_camera_take(),
 * nn_camera_want() and nn_camera_join(). */

static void nn_worker_entry(ULONG arg)
{
	ULONG got;

	(void)arg;

	for (;;) {
		(void)tx_event_flags_get(&nn_worker_flags, NN_WK_WAKE,
		                         TX_OR_CLEAR, &got, TX_WAIT_FOREVER);
		/* [!] A wake-up is not a job.  Only a hand-over this thread can
		 * TAKE is one; anything else was already taken or never was. */
		if (!nn_camera_take())
			continue;

		/* Invoke, lease, geometry, decode, publish.  The input and the
		 * outputs are this thread's until the job's DONE, which the frame
		 * path says only after the decode has finished reading them: a
		 * stream asks for the next frame, a one-shot (`nn run`) wanted one
		 * and parks for good (issue #129). */
		nn_camera_work();
		(void)tx_event_flags_set(&nn_worker_flags, NN_WK_SETTLED, TX_OR);
	}
}

void nn_worker_create_objects(void)
{
	if (tx_event_flags_create(&nn_worker_flags, "nn_worker") != TX_SUCCESS) {
		LOG_ERR("event flags not created; streams will be refused");
		return;
	}
	if (tx_thread_create(&nn_worker_thread, "nn_worker", nn_worker_entry, 0,
	                     nn_worker_stack, sizeof nn_worker_stack,
	                     NN_WORKER_PRIO, NN_WORKER_PRIO,
	                     TX_NO_TIME_SLICE, TX_AUTO_START) != TX_SUCCESS) {
		LOG_ERR("thread not created; streams will be refused");
		return;
	}
	nn_worker_ok = 1;
}

int nn_worker_ready(void)
{
	return nn_worker_ok;
}

int nn_worker_arm(void)
{
	if (!nn_worker_ok)
		return 0;
	return nn_camera_want();
}

void nn_worker_wake(void)
{
	/* Only an armed word can have been handed over, and only a worker that
	 * exists can have armed it -- so this is a formality, kept so a wake-up
	 * can never touch flags that were not created. */
	if (nn_worker_ok)
		(void)tx_event_flags_set(&nn_worker_flags, NN_WK_WAKE, TX_OR);
}

int nn_worker_join(void)
{
	ULONG t0 = tx_time_get();
	ULONG got, spent;

	for (;;) {
		if (nn_camera_join())
			return 0;
		/* Without the thread there is nothing that could ever settle the
		 * word, and no flags to wait on. */
		if (!nn_worker_ok)
			return -1;
		/* [!] WALL CLOCK, NOT A COUNT OF WAKE-UPS.  Stale SETTLED flags from
		 * earlier jobs return at once; counting them would run the deadline
		 * out against a worker that is behaving. */
		spent = tx_time_get() - t0;
		if (spent >= (ULONG)NN_WORKER_JOIN_TICKS)
			return -1;
		/* A SETTLED set between the JOIN above and this wait is still
		 * pending here, so the wake-up cannot be lost. */
		(void)tx_event_flags_get(&nn_worker_flags, NN_WK_SETTLED,
		                         TX_OR_CLEAR, &got,
		                         (ULONG)NN_WORKER_JOIN_TICKS - spent);
	}
}

/* ---- the job: invoke, lease, outputs, decode, publish, account ------------ */

/*
 * How the worker leaves one frame: a stream counts it in @p counter (if any); a
 * one-shot records @p shot for the console.  The frame path says DONE or
 * DONE_LAST after this.
 */
static void nn_ov_end(int oneshot, uint32_t *counter, uint8_t shot)
{
	if (oneshot) {
		nn_ov_shot = shot;
		return;
	}
	if (counter != NULL)
		nn_ov_bump(counter);
}

/* What the worker carries from its invoke into the frame path's hooks. */
struct nn_ov_work {
	struct nn_ov_job  job;
	struct nn_tensor  outs[NN_MAX_IO];
	unsigned          n_out;
	int               oneshot;
	uint32_t          t0, t1;   /* the invoke, in ticks   */
	uint32_t          e1, e2;   /* the invoke, EPK ticks  */
};

int nn_ov_admits(void *ctx, uint32_t gen)
{
	(void)ctx;
	return nn_rec_admits(gen);
}

/*
 * A frame nothing decodes -- `nn run` on a bare model (issue #104) -- is still a
 * result: the outputs' shapes, published under the generation rule like any
 * decode, so `nn dets` and `nn run` report the inference that ran (issue #121).
 * Moved here from the console with `nn run` itself (issue #129).
 *
 * [!] NOT INLINED, AND THAT IS A STACK DECISION.  The descriptors are ~300 B;
 * inlined into the worker's job they would sit in the frame the plugin's
 * decode() is entered below.  Only the path with no plugin needs them.
 */
/* One output, described for the record (nn_out_raw_fill()'s reader). */
static int nn_ov_desc(void *ctx, unsigned i, struct tensor_desc *out)
{
	const struct nn_tensor *t = nn_output(nn_ov_model, (int)i);

	(void)ctx;
	if (t == NULL)
		return -1;
	nn_desc_of(out, t);
	return 0;
}

static __attribute__((noinline)) int nn_ov_publish_raw_desc(uint32_t gen)
{
	struct nn_raw_outputs raw;

	/* Every output counted, the first NN_RAW_OUTPUTS_MAX described until one
	 * cannot be -- the report says how many there were and shows what it
	 * could (nn_outputs.h). */
	nn_out_raw_fill(&raw, (unsigned)nn_output_count(nn_ov_model), nn_ov_desc,
	                NULL);
	return nn_rec_publish_raw(gen, &raw);
}

int nn_ov_publish_raw(void *ctx, uint32_t gen)
{
	struct nn_ov_work *w = ctx;

	/* Nothing to decode with -- only on a one-shot (issue #104): a stream is
	 * refused at admission without a plugin.  The inference that ran is
	 * reported as the tensors it produced. */
	(void)nn_active_set_geom(&w->job.geom);   /* as `nn run` always did */
	return nn_ov_publish_raw_desc(gen);
}

/* One output, into the array the decode is handed (nn_out_collect()'s reader). */
static int nn_ov_read(void *ctx, unsigned i)
{
	struct nn_tensor *outs = ctx;
	const struct nn_tensor *t = nn_output(nn_ov_model, (int)i);

	if (t == NULL)
		return -1;
	outs[i] = *t;
	return 0;
}

int nn_ov_outputs(void *ctx)
{
	struct nn_ov_work *w = ctx;
	unsigned i;

	/*
	 * [!] THE PLUGIN QUESTION BEFORE THE OUTPUTS ARE READ (review of a438f76).
	 * What may be done with the outputs depends on it: a bare model is
	 * reported whatever its output count (the frame path's raw branch), and
	 * a decode is limited to NN_MAX_IO, which a stream refuses
	 * past and `nn run` truncates to, as each always did (nn_outputs.h).  The
	 * frame path asked it under the lease just before this; the outputs are
	 * read here, after the invoke, and the DONE that lets the producer write
	 * again comes only after the job.
	 */
	if (nn_out_plan(w->oneshot, 1, (unsigned)nn_output_count(nn_ov_model),
	                NN_MAX_IO, &w->n_out) != NN_OUT_DECODE)
		return -1;
	i = nn_out_collect(w->n_out, nn_ov_read, w->outs);
	if (i != w->n_out) {
		/* Nothing decoded, so nothing is published: the record keeps the
		 * plugin's last result, which is still the plugin's state. */
		nn_ov_shot_index = (uint8_t)i;
		return -1;
	}
	/*
	 * [!] THE GEOMETRY BEFORE THE DECODE, AND UNDER THE SAME HOLD (issue
	 * #127).  It is part of the plugin's result: decode() may call the base's
	 * to_frame(), which reads it, and a console holding the lease after this
	 * frame may ask the plugin for a report that does the same.  It is the
	 * geometry the producer cut THIS input by, carried in the job.  Written
	 * whatever the decode then says, because the record is too.
	 */
	(void)nn_active_set_geom(&w->job.geom);   /* issue #103 */
	return 0;
}

int nn_ov_decode(void *ctx, int *n)
{
	struct nn_ov_work *w = ctx;
	int nd = nn_active_decode(w->outs, w->n_out);

	/* [!] NN_ACTIVE_NOT_HELD: not reachable while the worker's take stands --
	 * and if it ever does not, the decode did not run, so nothing is
	 * published.  Counted by the entry check (plugin_lease_unheld()), not as
	 * a decoder error: the decoder was never asked. */
	if (nd == NN_ACTIVE_NOT_HELD)
		return -1;
	*n = nd;
	return 0;
}

int nn_ov_publish(void *ctx, int n, uint32_t gen)
{
	int took;

	(void)ctx;
	/* Under the generation the producer handed over (issue #118). */
	took = nn_rec_publish_external(n, gen);
	/* [!] process()'s answer is written HERE, still inside the lease and
	 * right after the publish, as it always was -- not in the account
	 * below, which runs after the lease is given back and would widen the
	 * window in which the first result is not yet drawn. */
	nn_ov_result = (n < 0) ? NN_OV_RES_FAIL : NN_OV_RES_OK;
	return took;
}

/*
 * This board's counting, after the lease is given back -- the table it always
 * had (issue #97: model and decoder failures apart; a one-shot counts nothing).
 */
void nn_ov_account(void *ctx, enum nn_core_done what, int nd, int took)
{
	TX_INTERRUPT_SAVE_AREA
	struct nn_ov_work *w = ctx;
	uint32_t e3;

	(void)took;   /* every decode is counted, as it always was */
	switch (what) {
	case NN_CORE_DONE_RAW:
		nn_ov_end(w->oneshot, NULL, NN_OV_SHOT_PUBLISHED);
		return;
	case NN_CORE_DONE_NO_OUTPUTS:
		nn_ov_end(w->oneshot, &nn_ov_stats.errors, NN_OV_SHOT_NO_OUTPUTS);
		return;
	case NN_CORE_DONE_NOT_HELD:
		nn_ov_end(w->oneshot, NULL, NN_OV_SHOT_NOT_HELD);
		return;
	case NN_CORE_DONE_RETIRED:
		/* Not reachable here: no boundary falls inside a session on this
		 * board (nn_rec.h).  Were it ever, the session ended under the job,
		 * which is a stop's answer and no error. */
		nn_ov_end(w->oneshot, NULL, NN_OV_SHOT_STOPPED);
		return;
	case NN_CORE_DONE_DECODED:
	default:
		break;
	}
	e3 = tx_glue_epk_timer_ticks();

	if (w->oneshot) {
		nn_ov_end(w->oneshot, NULL, NN_OV_SHOT_PUBLISHED);
		return;
	}

	TX_DISABLE
	if (nd < 0) {
		/* [!] There is no console on this path, so the only way a decode
		 * failure can be told apart afterwards is if it is counted apart
		 * (issue #97). */
		if (nd == BF_ERR_MODEL)
			nn_ov_stats.model_errors++;
		else
			nn_ov_stats.decoder_errors++;
		nn_ov_stats.errors++;
		nn_ov_last_status = nd;
	} else {
		nn_ov_last_status = BF_OK;
		nn_ov_stats.inferences++;
		nn_ov_stats.detections += (uint32_t)nd;
		nn_ov_stats.last_ms   = w->t1 - w->t0;
		nn_ov_stats.last_ndet = nd;
		/* Only frames that completed every stage accumulate, so the three
		 * means describe one set: prep from the producer, the rest here. */
		nn_ov_prep_ticks   += w->job.prep_ticks;
		nn_ov_invoke_ticks += (uint32_t)(w->e2 - w->e1);
		nn_ov_decode_ticks += (uint32_t)(e3 - w->e2);
		nn_ov_cycle_ticks  += (uint32_t)(e3 - w->job.t_hand);
		nn_ov_prof_frames++;
		nn_ov_ndet = nd;
	}
	TX_RESTORE
}

int nn_camera_take(void)
{
	return nn_core_frame_take(&nn_ov_core, &nn_ov_ops);
}

void nn_camera_work(void)
{
	struct nn_ov_work w;
	const int oneshot = nn_ov_oneshot;

	w.job     = nn_ov_job;
	w.n_out   = 0u;
	w.oneshot = oneshot;

	/*
	 * [!] A PENDING STOP IS NOT FOLLOWED BY AN INVOKE.  This is the last
	 * instant before the expensive, uninterruptible part; the stop's join
	 * then waits out at most an invoke already running.
	 */
	if (nn_ov_stop) {
		nn_ov_end(oneshot, &nn_ov_stats.skipped, NN_OV_SHOT_STOPPED);
		goto done;
	}

	/* No cache maintenance here.  The port does it inside Invoke(), at the
	 * two instants the arena changes hands (issue #46) -- on this thread now,
	 * the same two points; anything from out here is too early or too late. */
	w.e1 = tx_glue_epk_timer_ticks();
	w.t0 = (uint32_t)tx_time_get();
	if (nn_run(nn_ov_model) != 0) {
		nn_ov_end(oneshot, &nn_ov_stats.errors, NN_OV_SHOT_INVOKE_FAILED);
		goto done;
	}
	w.t1 = (uint32_t)tx_time_get();
	w.e2 = tx_glue_epk_timer_ticks();

	/*
	 * [!] THE PLUGIN LEASE, WAITED FOR AND BOUNDED (issue #129).  A console
	 * calling into the same plugin -- `nn thresh`, `nn dets` -- holds it for
	 * microseconds to milliseconds; the worker is not on the camera's or the
	 * panel's clock, so it waits rather than throwing a finished inference
	 * away.  A wait that runs out is an answer, not a licence: nothing of
	 * this frame is published, not the decode, not the record, not the
	 * geometry, and the record and the plugin's own result still describe the
	 * previous frame together.  Counted as an error.
	 *
	 * Taken after the invoke, not before it: the NPU does not touch the
	 * plugin, and holding the lease for the whole inference would make every
	 * console wait out a frame.  The frame path gives it back.
	 */
	if (!plugin_lease_take()) {
		if (!oneshot)
			nn_ov_bump(&nn_ov_stats.lease_timeouts);
		nn_ov_end(oneshot, &nn_ov_stats.errors, NN_OV_SHOT_LEASE_TIMEOUT);
		goto done;
	}
	/* Plugin question, outputs, geometry, decode, publish, account, DONE --
	 * the shared frame path, with this board's hooks above. */
	nn_core_on_infer_done(&nn_ov_core, &nn_ov_ops, &w, w.job.gen, !oneshot);
	return;

done:
	nn_core_frame_done(&nn_ov_core, &nn_ov_ops, !oneshot);
}
