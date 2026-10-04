/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Face detection over the live preview (issue #48).  See nn_overlay.h for why
 * this lives in the port rather than in cmds/.
 */
#include "nn_overlay.h"

#include <stddef.h>
#include <string.h>        /* memset */

#include "tx_api.h"        /* tx_time_get(): ThreadX ticks, 1 ms here */

#include "npu_desc.h"
#include "nn_active.h"
#include "plugin_paint.h"
#include "plugin_lease.h"
#include "plugin_abi.h"
#include "camera.h"
#include "cam_dp.h"
#include "cam_sensor.h"
#include "lcd_st7789.h"
#include "nn_preproc.h"
#include "nn_rec.h"
#include "npu.h"
#include "nn_worker.h"     /* the inference worker (issue #129) */
#include "nn_core_frame.h" /* the shared frame path (issue #130) */
#include "nn_outputs.h"    /* what the worker does with the outputs (#129) */
#include "tx_glue.h"       /* the EPK's TIMER2: the stage clock (issue #60) */

/*
 * [!] Set from the SHELL thread, read on the PRODUCER and WORKER threads.
 *
 * volatile, and that is the whole of the synchronisation: it is a single word,
 * every write is a plain store of 0 or 1, and no decision anywhere depends on
 * two reads of it agreeing.  A missed update costs one more inference before
 * the stream stops, which is exactly the cost the design already accepts (see
 * nn_overlay_request_stop()).
 */
static volatile uint8_t nn_ov_stop;

static struct nn_overlay_stats nn_ov_stats;

/*
 * The stage accumulators behind the struct's prep/invoke/decode (issue #60).
 *
 * EPK TIMER2 ticks, for the same reason the camera's profile uses them: this
 * is the clock that is already validated every time anyone asks
 * tx_glue_profile_ok(), and the sink number these stages have to sum against
 * is measured with it.  64-bit because a long preview overflows 32 at 6 MHz.
 *
 * Written by the inference worker only, inside a critical section since issue
 * #129: the worker runs below the producer, which can preempt it mid-add, and
 * every reader snapshots under TX_DISABLE.  `prep` is the producer's figure,
 * carried in the job and added by the worker with the rest.  Only frames that
 * completed all three stages accumulate, so the three means describe the same
 * set of frames; a frame that failed mid-way adds to none of them.
 */
static uint64_t nn_ov_prep_ticks;
static uint64_t nn_ov_invoke_ticks;
static uint64_t nn_ov_decode_ticks;
/* The worker's round trip, hand-over to publish, over the same frames (#129). */
static uint64_t nn_ov_cycle_ticks;
static uint32_t nn_ov_prof_frames;

/*
 * What process() hands to draw() for the same frame -- since issue #129 only
 * the frame's number (the frame path's frame_no since issue #130); the result
 * draw() paints is the worker's, behind the plugin lease.
 *
 * [!] THEY ARE NO LONGER THE SAME THREAD (issue #57).  process() runs on the
 * camera producer, inside consume(); draw() runs on the panel thread, inside the
 * blit that consume() handed over.  There is still no lock, and the reason is
 * not proximity any more but exclusion:
 *
 *   - the pipeline pre-pins ONE delivery per sink and, under FRAME_POLICY_DROP,
 *     refuses a second while the first is outstanding -- so process() cannot run
 *     again until the panel thread has released the frame;
 *   - the panel thread releases it only AFTER draw() has returned.
 *
 * So the two alternate strictly, and the hand-off (a semaphore, which on this
 * M55 port carries the context-switch DSB/ISB) publishes what process() wrote.
 * The invariant to protect is the panel thread's step order, in cam_lcd_sink.c:
 * nothing here may be touched after its frame_pipeline_put().
 *
 * Note that NONE of that is an argument about priorities, which is why issue #64
 * could reverse the two threads' ranking without touching this file.
 *
 * The plugin lease (issue #127) is a different matter: it keeps the worker's
 * decode, the panel's draw and a console's call out of the plugin at the same
 * time.
 *
 * Static because nothing here may ever be freed under a producer, a worker or a
 * panel thread that did not acknowledge a stop (see nn_overlay.h).
 *
 * [!] NO BOX ARRAY SINCE ISSUE #104.  A stream only runs with a plugin loaded,
 * and a plugin's result is its own -- it paints through the painter, and this
 * file never learns what shape the result has.
 */
/* The most recent decode's status, so `nn stream`'s summary can say WHY it
 * annotated nothing rather than only that it did not. */
static int           nn_ov_last_status;
static int           nn_ov_ndet;

/*
 * [!] THE STACK PROBE THAT STOOD HERE MOVED TO THE PLUGIN'S ENTRY (issue #119).
 * It was taken at this file's two call sites -- before nn_active_decode() and
 * nn_active_draw() -- so the frame nn_active_decode() builds before it calls
 * through was never in it, and the number read as the depth a plugin is entered
 * at was short of it.  It also saw only the producer and the panel, while the
 * same decode runs on a shell thread for `nn run` and `nn dets`.  The probe is
 * now in nn_active.c, beside each indirect call, one record per slot and per
 * thread (nn_probe.h).
 */
/*
 * What one plugin draw() may spend, and why these numbers.
 *
 * The panel guard is held for the whole of draw(), and everything else that
 * wants the panel is failing its non-blocking acquire meanwhile, so the work the
 * base does on a plugin's behalf is capped at a quarter of a frame.
 *
 * [!] THE JUSTIFICATION USED TO QUOTE 775 us FOR THE STAGED BLIT, AND THAT
 * NUMBER IS SIX MONTHS STALE (issue #105).  The staging loops were compiled -O3
 * when issue #42 removed the MVE ban that had forced -Os on them, and `held`
 * dropped to 197 us -- the board README carries the before/after table.  The
 * cap did not need changing, but the reasoning behind it was being read off a
 * measurement that no longer described the machine, and a plan for this issue
 * copied 775 into a hardware acceptance criterion before anyone noticed: it
 * would have passed a threefold regression as "no change".  Take a fresh
 * baseline; do not transcribe a constant out of a comment.
 *
 * A full frame is 320 x 240 = 76,800 pixels, so a quarter of a frame is 19,200:
 * enough for a label bar (320 x 16 = 5,120), and enough for every box
 * BF_MAX_DET allows now that an outline is charged for the pixels it writes
 * rather than the area it encloses (issue #105 -- before that, one 200x200 box
 * was refused on its own).
 *
 * [!] A CAP THAT CANNOT BE EXCEEDED IS NOT A CAP.  The stack allowances of this
 * issue were first written at the size of the whole thread stack, which made
 * the check unable to fire for the case it existed to catch.  This one is
 * deliberately below what a plugin might plausibly want, and what a draw
 * actually spends is reported by `nn stream stats` so it can be judged rather
 * than argued about.
 */
#define NN_OV_DRAW_PIXELS  (320u * 240u / 4u)
#define NN_OV_DRAW_OPS     64u

static uint32_t nn_ov_draw_spent;     /* high-water, pixels charged  */
static uint32_t nn_ov_draw_refused;   /* primitives refused for want */

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
static struct nn_ov_job nn_ov_job;

/*
 * The frame path's state (svc/nn_core_frame.h): the hand-over word, and the
 * frame numbers draw() reads its lag from.  Static and never freed, like the
 * rest of this file.
 */
static struct nn_core_frame nn_ov_core;

/*
 * What the latest publish of THIS stream was, for process()'s answer: draw this
 * frame or not.  Written by the worker right after the publish, inside the
 * plugin lease (nn_ov_publish()), read by the producer
 * as one byte.  NONE until the first decode is published.
 */
#define NN_OV_RES_NONE 0u
#define NN_OV_RES_OK   1u
#define NN_OV_RES_FAIL 2u
static volatile uint8_t nn_ov_result;   /* nn_ov_publish(), in the lease */

/*
 * `nn run` (issue #129): the same producer and worker, for ONE frame.  Set by
 * nn_overlay_arm_oneshot(), cleared by nn_overlay_arm(); written only while the
 * worker is parked and nothing is attached.
 *
 * [!] A ONE-SHOT COUNTS NOTHING HERE.  The stream statistics are what `nn stream
 * stats` reads after a stream has ended, and `nn run` never touched them; it
 * still does not.  What it reports is its own ending, below.
 */
static uint8_t nn_ov_oneshot;
/* How the one-shot's frame ended, NN_OV_SHOT_* (nn_overlay.h); NONE while it
 * has not.  Written once, by the producer (a frame it could not prepare) or by
 * the worker (after its publish, before it parks) -- never both, since the
 * producer writes it only before the hand-over and the worker only after. */
static volatile uint8_t nn_ov_shot;
/* With NN_OV_SHOT_NO_OUTPUTS: the output that could not be read.  Written
 * before nn_ov_shot, by the same thread. */
static volatile uint8_t nn_ov_shot_index;

/* A counter both the producer and the worker write. */
static void nn_ov_bump(uint32_t *c)
{
	TX_INTERRUPT_SAVE_AREA

	TX_DISABLE
	(*c)++;
	TX_RESTORE
}

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

/* ---- the frame path's hooks (svc/nn_core_frame.h, issue #130) ------------- */

static unsigned nn_ov_cs_enter(void)
{
	TX_INTERRUPT_SAVE_AREA

	TX_DISABLE
	return (unsigned)interrupt_save;
}

static void nn_ov_cs_exit(unsigned posture)
{
	TX_INTERRUPT_SAVE_AREA

	interrupt_save = (UINT)posture;
	TX_RESTORE
}

/*
 * The producer's prep, into the input tensor, while the word is FILLING.
 * `pixels` is not used, and that is deliberate -- see nn_overlay_process().
 */
static int nn_ov_prep(void *ctx, unsigned part, unsigned nparts)
{
	struct npu_tensor in;
	struct nn_preproc_geom geom;
	uint32_t e0, e1;

	(void)ctx;
	(void)part;
	(void)nparts;
	/* Stage clock (issue #60): `prep` is the producer's whole share now. */
	e0 = tx_glue_epk_timer_ticks();

	if (npu_input(&in) != NPU_OK ||
	    in.rank != 4 || in.dims[3] != 3 ||
	    nn_preproc_geom(CAM_FRAME_WIDTH, CAM_FRAME_HEIGHT,
	                    (uint32_t)in.dims[2], (uint32_t)in.dims[1],
	                    &geom) != 0 ||
	    in.bytes < (size_t)in.dims[2] * (size_t)in.dims[1] * 3u ||
	    nn_preproc_fill(camera_raw_frame(), CAM_FRAME_WIDTH,
	                    CAM_FRAME_HEIGHT, &geom, (uint8_t *)in.data) != 0)
		return -1;
	e1 = tx_glue_epk_timer_ticks();

	/* The record generation this frame publishes under (issue #118).  No
	 * boundary can move it while this producer is inside consume(): the
	 * stream's start takes its boundary before the sink is attached, and its
	 * stop only after the producer AND the worker are confirmed out. */
	nn_ov_job.gen        = nn_rec_gen();
	nn_ov_job.prep_ticks = e1 - e0;
	nn_ov_job.t_hand     = e1;
	nn_ov_job.geom       = geom;
	return 0;
}

static void nn_ov_infer_start(void *ctx)
{
	(void)ctx;
	nn_worker_wake();
}

/* What the worker carries from its invoke into the frame path's hooks. */
struct nn_ov_work {
	struct nn_ov_job  job;
	struct npu_tensor outs[NPU_DESC_MAX_OUTPUTS];
	unsigned          n_out;
	int               oneshot;
	uint32_t          t0, t1;   /* the invoke, in ticks   */
	uint32_t          e1, e2;   /* the invoke, EPK ticks  */
};

static int nn_ov_is_plugin(void *ctx)
{
	(void)ctx;
	return nn_active_is_plugin();
}

static int nn_ov_admits(void *ctx, uint32_t gen)
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
	struct npu_tensor t;

	(void)ctx;
	if (npu_output(i, &t) != NPU_OK)
		return -1;
	npu_desc_of(out, &t);
	return 0;
}

static __attribute__((noinline)) int nn_ov_publish_raw_desc(uint32_t gen)
{
	struct nn_raw_outputs raw;

	/* Every output counted, the first NN_RAW_OUTPUTS_MAX described until one
	 * cannot be -- the report says how many there were and shows what it
	 * could (nn_outputs.h). */
	nn_out_raw_fill(&raw, npu_output_count(), nn_ov_desc, NULL);
	return nn_rec_publish_raw(gen, &raw);
}

static int nn_ov_publish_raw(void *ctx, uint32_t gen)
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
	struct npu_tensor *outs = ctx;

	return (npu_output(i, &outs[i]) == NPU_OK) ? 0 : -1;
}

static int nn_ov_outputs(void *ctx)
{
	struct nn_ov_work *w = ctx;
	unsigned i;

	/*
	 * [!] THE PLUGIN QUESTION BEFORE THE OUTPUTS ARE READ (review of a438f76).
	 * What may be done with the outputs depends on it: a bare model is
	 * reported whatever its output count (the frame path's raw branch), and
	 * a decode is limited to NPU_DESC_MAX_OUTPUTS, which a stream refuses
	 * past and `nn run` truncates to, as each always did (nn_outputs.h).  The
	 * frame path asked it under the lease just before this; the outputs are
	 * read here, after the invoke, and the DONE that lets the producer write
	 * again comes only after the job.
	 */
	if (nn_out_plan(w->oneshot, 1, npu_output_count(), NPU_DESC_MAX_OUTPUTS,
	                &w->n_out) != NN_OUT_DECODE)
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

static int nn_ov_decode(void *ctx, int *n)
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

static int nn_ov_publish(void *ctx, int n, uint32_t gen)
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
static void nn_ov_account(void *ctx, enum nn_core_done what, int nd, int took)
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

static const struct nn_core_frame_ops nn_ov_ops = {
	.cs_enter          = nn_ov_cs_enter,
	.cs_exit           = nn_ov_cs_exit,
	.present           = NULL,   /* the sink presents; see cam_lcd_sink.h */
	.prep              = nn_ov_prep,
	.infer_start       = nn_ov_infer_start,
	.is_plugin         = nn_ov_is_plugin,
	.admits            = nn_ov_admits,
	.publish_raw       = nn_ov_publish_raw,
	.outputs           = nn_ov_outputs,
	.decode            = nn_ov_decode,
	.publish           = nn_ov_publish,
	.account           = nn_ov_account,
	.lease_try         = plugin_lease_try,
	.lease_held        = plugin_lease_held,
	.lease_give        = plugin_lease_give,
	.lease_note_unheld = plugin_lease_note_unheld,
};

int nn_overlay_want(void)
{
	return nn_core_frame_want(&nn_ov_core, &nn_ov_ops);
}

int nn_overlay_take(void)
{
	return nn_core_frame_take(&nn_ov_core, &nn_ov_ops);
}

int nn_overlay_join(void)
{
	return nn_core_frame_join(&nn_ov_core, &nn_ov_ops);
}

static int nn_overlay_process(void *ctx, const void *pixels,
                              uint16_t w, uint16_t h)
{
	int draw;
	const int oneshot = nn_ov_oneshot;

	(void)ctx;
	(void)w;
	(void)h;
	/*
	 * [!] `pixels` IS NOT THE MODEL'S INPUT, and that is deliberate.
	 *
	 * The sink hands over the PACKED RGB565 slot -- the image on its way to
	 * the panel, carrying the white balance, gamma and saturation tuned by
	 * eye for the glass.  The model reads camera_raw_frame() instead: the
	 * planar B/G/R the datapath wrote, which is the same frame (cam_publish
	 * packs it into the slot and only then publishes) but not the same
	 * pixels.
	 *
	 * Keeping the model on the raw frame is what makes `nn stream`
	 * comparable with `nn detect`, and keeps issue #45's working detection
	 * as the baseline: #48 changes the geometry, and changing the colour
	 * path in the same commit would leave nothing to compare against.
	 * Feeding it the gamma-encoded image is a real experiment, and it is a
	 * separate one.
	 */
	(void)pixels;

	if (!oneshot)
		nn_ov_stats.frames++;    /* producer only */

	/*
	 * [!] THE ANSWER IS "DRAW THIS FRAME OR NOT", NOT "WAS IT INFERRED" (issue
	 * #129).  The plugin paints its LATEST result on whatever frame is shown,
	 * so a frame the worker was too busy to take is still annotated -- one or
	 * more frames late -- as long as the stream's latest decode succeeded.
	 * Declined only before the first result, after a failed decode, and while
	 * a stop is pending; the sink counts those as shown unannotated.
	 */
	draw = (nn_ov_result == NN_OV_RES_OK) ? 0 : -1;

	/* Nothing started yet, so this is free; no draw on a panel about to be
	 * given up. */
	if (nn_ov_stop) {
		if (!oneshot)
			nn_ov_bump(&nn_ov_stats.skipped);
		return -1;
	}
	/* A one-shot whose frame already ended -- handed over, or refused below --
	 * takes no other: the frames after it go straight back. */
	if (oneshot && nn_ov_shot != NN_OV_SHOT_NONE)
		return -1;

	/*
	 * [!] THE INPUT IS THE PRODUCER'S ONLY WHILE THE WORKER WANTS A FRAME
	 * (svc/nn_core_frame.c).  The whole frame is this board's one part, so
	 * the frame path BEGINs, preps and HANDs in this one call -- or reports
	 * that the worker is still on an earlier frame: this one is not inferred
	 * (skipped, and counted as busy apart).  WANT cannot be taken from under
	 * this call: only this thread leaves FILLING, and the stop's JOIN runs
	 * after this thread is confirmed out.
	 */
	switch (nn_core_on_frame(&nn_ov_core, &nn_ov_ops, NULL, 0u, 1u)) {
	case NN_CORE_FR_SKIPPED:
		if (!oneshot) {
			nn_ov_bump(&nn_ov_stats.skipped);
			nn_ov_stats.busy++;      /* producer only */
		}
		break;
	case NN_CORE_FR_ABANDONED:
		/* A stream's worker still wants a frame and the next one tries
		 * again.  A one-shot ends here: the console is told why. */
		if (oneshot)
			nn_ov_shot = NN_OV_SHOT_PREP_FAILED;
		else
			nn_ov_bump(&nn_ov_stats.errors);
		break;
	case NN_CORE_FR_RACED:
		if (!oneshot)
			nn_ov_bump(&nn_ov_stats.errors);   /* not reachable */
		break;
	default:
		break;   /* handed over, and the worker woken */
	}
	return draw;
}

void nn_overlay_work(void)
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
	if (npu_invoke() != NPU_OK) {
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

/* ---- the panel ------------------------------------------------------------- */

/* The picture draw() was handed. */
struct nn_ov_canvas {
	uint16_t *fb;
	uint16_t  w, h;
};

static int nn_ov_record(void *ctx, struct nn_det_snapshot *snap)
{
	(void)ctx;
	nn_rec_snapshot(snap, NULL);
	return 1;
}

static void nn_ov_paint(void *ctx)
{
	TX_INTERRUPT_SAVE_AREA
	const struct nn_ov_canvas *cv = ctx;
	struct plugin_painter paint;
	struct plugin_paint_budget bud;
	uint32_t spent;

	bud.pixels  = NN_OV_DRAW_PIXELS;
	bud.ops     = NN_OV_DRAW_OPS;
	bud.refused = 0u;
	plugin_paint_bind(&paint, &bud, cv->fb, cv->w, cv->h);
	/* NN_ACTIVE_NOT_HELD paints nothing and is counted by the entry check
	 * itself; there is nothing more to do with it here. */
	(void)nn_active_draw(&paint);

	/*
	 * What it actually spent, so the cap can be judged against something
	 * rather than defended in the abstract.
	 *
	 * [!] BOTH IN ONE CRITICAL SECTION (issue #105).  A console reading
	 * these two is asking one question -- how close did a frame come to
	 * the cap, and did anything get refused -- and a preemption between
	 * the two stores answers it with this frame's spend beside the
	 * previous count.  The reader disabling interrupts cannot undo that,
	 * so the writer has to be atomic as well.  It is a handful of
	 * instructions on the panel thread, off the pixel path.
	 */
	spent = NN_OV_DRAW_PIXELS - bud.pixels;
	TX_DISABLE
	if (spent > nn_ov_draw_spent)
		nn_ov_draw_spent = spent;
	nn_ov_draw_refused += bud.refused;
	TX_RESTORE
}

static const struct nn_core_panel nn_ov_panel = {
	.frame_lock = NULL,   /* the panel guard is the caller's, held already */
	.may_draw   = NULL,
	.record     = nn_ov_record,
	.paint      = nn_ov_paint,
};

static void nn_overlay_draw(void *ctx, uint16_t *fb, uint16_t fb_w,
                            uint16_t fb_h)
{
	struct nn_ov_canvas cv;

	(void)ctx;

	/*
	 * The plugin paints its own result (issue #103), because the firmware does
	 * not know what shape that result has -- which is the whole point of
	 * issue #78.
	 *
	 * [!] AND THERE IS NO OTHER BRANCH SINCE ISSUE #104.  A resident path used
	 * to follow this one, mapping bf_det boxes through nn_preproc_box() and
	 * drawing them here.  With no decoder in the firmware nothing can reach it:
	 * nn_detector_ready() refuses to start a stream unless a plugin is loaded
	 * AND draws, so by the time the panel thread is calling this, both are true.
	 *
	 * [!] THE PLUGIN LEASE, TRIED ONCE INSIDE THE PANEL GUARD (issue #127),
	 * by the shared frame path (issue #130).  Nothing a console or the worker
	 * does can be inside this plugin while it paints.  A refusal shows the
	 * frame without an overlay and is counted as this frame's miss -- the only
	 * one it can have, since the producer no longer asks (issue #129).
	 *
	 * [!] THE ORDER IS PANEL GUARD, THEN LEASE -- wio's is the other way
	 * round (lease, then frame lock), because here draw() is called from
	 * inside the guard and there is no earlier point to ask.  It cannot
	 * close a cycle because this thread only TRIES: it never waits while
	 * holding the guard.  What would close one is a lease holder that
	 * waits for the panel guard, a camera API mutex or a pipeline lock;
	 * no holder does, and none touches the LCD.  Released before the
	 * callback returns, and nothing else is waited for in between.
	 */
	cv.fb = fb;
	cv.w  = fb_w;
	cv.h  = fb_h;
	(void)nn_core_draw(&nn_ov_core, &nn_ov_ops, &nn_ov_panel, &cv);
	/* The lag of what was painted (issue #129), counted now: the staged
	 * frame is this draw's, and nothing later on this board says more. */
	nn_core_on_present_done(&nn_ov_core, &nn_ov_ops);
}

static const struct cam_lcd_overlay nn_ov_vtable = {
	.ctx     = NULL,
	.process = nn_overlay_process,
	.draw    = nn_overlay_draw,
};

const struct cam_lcd_overlay *nn_overlay_arm(void)
{
	TX_INTERRUPT_SAVE_AREA

	/*
	 * [!] THE DRAW PAIR IS RESET HERE, AND IT WAS NOT (issue #105).  Every
	 * other counter in this function is per-stream; these two survived across
	 * generations, so measuring the classifier and then the detector reported
	 * the classifier's high-water for both -- which is precisely the number
	 * the budget is judged by.
	 *
	 * Under the same critical section the writer and the reader use, so the
	 * three agree on what one snapshot means.  There is no writer to race
	 * here -- arm() runs after the old stream is quiescent and before the new
	 * sink is attached -- and the reset is deliberately NOT done on stop, so
	 * `nn stream stats` after a stop still describes the run that just ended.
	 */
	TX_DISABLE
	nn_ov_draw_spent   = 0u;
	nn_ov_draw_refused = 0u;
	TX_RESTORE
	/* The lease's frames missed, per stream like the rest (issue #127), and
	 * likewise not on stop.  Its own critical section, inside the call. */
	plugin_lease_misses_reset();

	/*
	 * The worker is parked -- the caller armed it IDLE -> WANT and nothing
	 * hands it a frame until the attach -- so nothing below has a writer.
	 */
	TX_DISABLE
	nn_ov_stats.inferences = 0u;
	nn_ov_stats.detections = 0u;
	nn_ov_stats.skipped    = 0u;
	nn_ov_stats.errors     = 0u;
	nn_ov_stats.busy           = 0u;
	nn_ov_stats.lease_timeouts = 0u;
	nn_ov_stats.model_errors   = 0u;
	nn_ov_stats.decoder_errors = 0u;
	nn_ov_stats.last_ms    = 0u;
	nn_ov_stats.last_ndet  = 0;
	nn_ov_last_status      = 0;
	nn_ov_prep_ticks       = 0u;
	nn_ov_invoke_ticks     = 0u;
	nn_ov_decode_ticks     = 0u;
	nn_ov_cycle_ticks      = 0u;
	nn_ov_stats.frames     = 0u;
	nn_ov_prof_frames      = 0u;
	nn_ov_ndet             = 0;
	nn_ov_result           = NN_OV_RES_NONE;
	nn_ov_oneshot          = 0u;
	nn_ov_shot             = NN_OV_SHOT_NONE;
	nn_ov_stop             = 0u;
	TX_RESTORE
	/* The frame numbers and the lag figures (issue #129), which live with the
	 * frame path's state since issue #130.  Its own critical section. */
	nn_core_frame_reset(&nn_ov_core, &nn_ov_ops, 1);
	return &nn_ov_vtable;
}

const struct cam_lcd_overlay *nn_overlay_arm_oneshot(void)
{
	TX_INTERRUPT_SAVE_AREA

	/*
	 * Only what the producer and the worker read to decide; NOT the stream's
	 * counters, which describe the last stream until the next one is armed.
	 * Nothing writes these now: the worker is parked (the caller armed it
	 * IDLE -> WANT and nothing has been attached to hand it a frame).
	 */
	TX_DISABLE
	nn_ov_oneshot   = 1u;
	nn_ov_shot      = NN_OV_SHOT_NONE;
	nn_ov_shot_index = 0u;
	nn_ov_stop      = 0u;
	TX_RESTORE
	/* The frame numbers, and not the stream's lag figures. */
	nn_core_frame_reset(&nn_ov_core, &nn_ov_ops, 0);
	return &nn_ov_vtable;
}

int nn_overlay_shot(void)
{
	return (int)nn_ov_shot;
}

unsigned nn_overlay_shot_index(void)
{
	return (unsigned)nn_ov_shot_index;
}

void nn_overlay_request_stop(void)
{
	nn_ov_stop = 1u;
}

/* Ticks -> total us, in 64-bit so the multiply cannot wrap first.  The result
 * is truncated to 32 bits, which holds hours of accumulated stage time -- the
 * same exposure cam_lcd_sink.c's blit_us already accepts. */
static uint32_t nn_ov_us(uint64_t ticks, uint32_t hz)
{
	return (hz != 0u) ? (uint32_t)((ticks * 1000000u) / hz) : 0u;
}

void nn_overlay_stats(struct nn_overlay_stats *out)
{
	TX_INTERRUPT_SAVE_AREA
	const char *why = NULL;
	uint64_t prep, invoke, decode, cycle;
	uint32_t frames, hz;

	if (out == NULL)
		return;

	/*
	 * One critical section for the lot: the 64-bit accumulators are written
	 * by the worker thread, and half of a 64-bit add is not a slightly
	 * wrong number but a wildly wrong one.  Same treatment as the camera's
	 * profile and the panel sink's, for the same reason.
	 */
	TX_DISABLE
	*out   = nn_ov_stats;
	prep   = nn_ov_prep_ticks;
	invoke = nn_ov_invoke_ticks;
	decode = nn_ov_decode_ticks;
	cycle  = nn_ov_cycle_ticks;
	frames = nn_ov_prof_frames;
	/*
	 * [!] THE DRAW PAIR IS COPIED HERE, INSIDE, and the panel thread updates
	 * it inside one too (issue #105).  A reader-side critical section alone
	 * cannot make these two agree: the panel thread writes the high-water and
	 * then adds to the refusals, and a console preempting BETWEEN those two
	 * stores sees the new spend beside the old refusal count no matter what it
	 * disables afterwards.  Disabling interrupts does not reach backwards.
	 */
	out->draw_spent   = nn_ov_draw_spent;
	out->draw_refused = nn_ov_draw_refused;
	/* The lag figures, which the frame path updates under this same kind of
	 * critical section (svc/nn_core_frame.c, issue #130). */
	out->lag_sum      = nn_ov_core.lag_sum;
	out->lag_n        = nn_ov_core.lag_n;
	out->lag_max      = nn_ov_core.lag_max;
	TX_RESTORE

	/* The stage rows are only as good as their clock, and this port has the
	 * predicate for that -- the same one `thread` and `camera stats` use. */
	hz = tx_glue_epk_timer_hz();
	out->prof_ok     = (tx_glue_profile_ok(&why) && hz != 0u);
	out->prof_frames = frames;
	out->prep_us     = out->prof_ok ? nn_ov_us(prep,   hz) : 0u;
	out->invoke_us   = out->prof_ok ? nn_ov_us(invoke, hz) : 0u;
	out->decode_us   = out->prof_ok ? nn_ov_us(decode, hz) : 0u;
	out->cycle_us    = out->prof_ok ? nn_ov_us(cycle,  hz) : 0u;
}
