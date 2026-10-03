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
static uint32_t nn_ov_prof_frames;

/*
 * What process() hands to draw() for the same frame -- since issue #129 only
 * the frame's number; the result draw() paints is the worker's, behind the
 * plugin lease.
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
 * only while the worker wants one (nn_handoff.h); the worker runs the invoke,
 * the decode and the publish.  There is no staging copy: the input the model
 * reads is the raw WDMA3 frame, which the pipeline's pin does not reach and the
 * datapath rewrites two frames later, so it is either copied or prepared on
 * the producer -- and the copy cost a quarter of the detector's frames (spike,
 * 2026-10-03).
 *
 * WHAT CROSSES.  The job below is written by the producer only while the
 * hand-over word is WANT, and read by the worker only after it TAKEs it; both
 * transitions are critical sections, which are compiler barriers, and the
 * worker copies the job out before it does anything else.
 */
struct nn_ov_job {
	uint32_t frame;                /* the producer's frame number          */
	uint32_t gen;                  /* the record generation (issue #118)   */
	uint32_t prep_ticks;           /* the producer's prep, EPK ticks       */
	struct nn_preproc_geom geom;   /* the transform this input was cut by  */
};
static struct nn_ov_job nn_ov_job;

/* Frames this sink was handed since arm.  Producer writes; draw() reads it for
 * the frame it is drawing (the one-delivery hand-off orders the two). */
static uint32_t nn_ov_frame_no;
static uint32_t nn_ov_cur_frame;

/*
 * What the latest publish of THIS stream was, for process()'s answer: draw this
 * frame or not.  Written by the worker inside the lease, read by the producer
 * as one byte.  NONE until the first decode is published.
 */
#define NN_OV_RES_NONE 0u
#define NN_OV_RES_OK   1u
#define NN_OV_RES_FAIL 2u
static volatile uint8_t nn_ov_result;
/* The producer's frame number of the result the plugin holds.  Written by the
 * worker and read by draw(), both under the plugin lease. */
static uint32_t nn_ov_res_frame;

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

/* A counter both the producer and the worker write. */
static void nn_ov_bump(uint32_t *c)
{
	TX_INTERRUPT_SAVE_AREA

	TX_DISABLE
	(*c)++;
	TX_RESTORE
}

/*
 * How the worker leaves one frame: a stream counts it in @p counter (if any) and
 * asks for the next frame; a one-shot records @p shot for the console and asks
 * for nothing more.  @return what nn_overlay_work() returns.
 */
static int nn_ov_end(int oneshot, uint32_t *counter, uint8_t shot)
{
	if (oneshot) {
		nn_ov_shot = shot;
		return 0;
	}
	if (counter != NULL)
		nn_ov_bump(counter);
	return 1;
}

static int nn_overlay_process(void *ctx, const void *pixels,
                              uint16_t w, uint16_t h)
{
	struct npu_tensor in;
	struct nn_preproc_geom geom;
	uint32_t e0, e1;
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

	nn_ov_frame_no++;
	nn_ov_cur_frame = nn_ov_frame_no;

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
	 * [!] THE INPUT IS THE PRODUCER'S ONLY WHILE THE WORKER WANTS A FRAME.
	 * WANT is stable from here to the HAND below: only this thread leaves it
	 * (HAND), and the stop's JOIN runs after this thread is confirmed out.
	 * Not wanted means the worker is still on an earlier frame -- this one is
	 * not inferred (skipped, and counted as busy apart).
	 */
	if (!nn_worker_wants()) {
		if (!oneshot) {
			nn_ov_bump(&nn_ov_stats.skipped);
			nn_ov_stats.busy++;      /* producer only */
		}
		return draw;
	}

	/* Stage clock (issue #60): `prep` is the producer's whole share now. */
	e0 = tx_glue_epk_timer_ticks();

	if (npu_input(&in) != NPU_OK ||
	    in.rank != 4 || in.dims[3] != 3 ||
	    nn_preproc_geom(CAM_FRAME_WIDTH, CAM_FRAME_HEIGHT,
	                    (uint32_t)in.dims[2], (uint32_t)in.dims[1],
	                    &geom) != 0 ||
	    in.bytes < (size_t)in.dims[2] * (size_t)in.dims[1] * 3u ||
	    nn_preproc_fill(camera_raw_frame(), CAM_FRAME_WIDTH,
	                    CAM_FRAME_HEIGHT, &geom, (uint8_t *)in.data) != 0) {
		/* A stream's worker still wants a frame and the next one tries
		 * again.  A one-shot ends here: the console is told why. */
		if (oneshot)
			nn_ov_shot = NN_OV_SHOT_PREP_FAILED;
		else
			nn_ov_bump(&nn_ov_stats.errors);
		return draw;
	}
	e1 = tx_glue_epk_timer_ticks();

	/* The record generation this frame publishes under (issue #118).  No
	 * boundary can move it while this producer is inside consume(): the
	 * stream's start takes its boundary before the sink is attached, and its
	 * stop only after the producer AND the worker are confirmed out. */
	nn_ov_job.frame      = nn_ov_frame_no;
	nn_ov_job.gen        = nn_rec_gen();
	nn_ov_job.prep_ticks = e1 - e0;
	nn_ov_job.geom       = geom;
	/* After the input is written and the job filled, never before. */
	if (!nn_worker_hand() && !oneshot)
		nn_ov_bump(&nn_ov_stats.errors);   /* not reachable: WANT above */
	return draw;
}

/*
 * A frame nothing decodes -- `nn run` on a bare model (issue #104) -- is still a
 * result: the outputs' shapes, published under the generation rule like any
 * decode, so `nn dets` and `nn run` report the inference that ran (issue #121).
 * Moved here from the console with `nn run` itself (issue #129).
 *
 * [!] NOT INLINED, AND THAT IS A STACK DECISION.  The descriptors are ~300 B;
 * inlined into nn_overlay_work() they would sit in the frame the plugin's
 * decode() is entered below.  Only the path with no plugin needs them.
 */
static __attribute__((noinline)) int nn_ov_publish_raw(uint32_t gen)
{
	struct nn_raw_outputs raw;
	unsigned n = npu_output_count(), i;

	memset(&raw, 0, sizeof raw);
	raw.count = (int32_t)n;
	for (i = 0u; i < n && i < NN_RAW_OUTPUTS_MAX; i++) {
		struct npu_tensor t;

		if (npu_output(i, &t) != NPU_OK)
			break;
		npu_desc_of(&raw.out[i], &t);
		raw.n = (uint8_t)(i + 1u);
	}
	return nn_rec_publish_raw(gen, &raw);
}

int nn_overlay_work(void)
{
	TX_INTERRUPT_SAVE_AREA
	struct nn_ov_job job;
	struct npu_tensor outs[NPU_DESC_MAX_OUTPUTS];
	unsigned n_out, i;
	uint32_t t0, t1;
	uint32_t e1, e2, e3;
	int nd;
	const int oneshot = nn_ov_oneshot;

	job = nn_ov_job;

	/*
	 * [!] A PENDING STOP IS NOT FOLLOWED BY AN INVOKE.  This is the last
	 * instant before the expensive, uninterruptible part; the stop's join
	 * then waits out at most an invoke already running.
	 */
	if (nn_ov_stop)
		return nn_ov_end(oneshot, &nn_ov_stats.skipped, NN_OV_SHOT_STOPPED);

	n_out = npu_output_count();
	if (n_out > NPU_DESC_MAX_OUTPUTS)
		return nn_ov_end(oneshot, &nn_ov_stats.errors,
		                 NN_OV_SHOT_NO_OUTPUTS);
	for (i = 0; i < n_out; i++)
		if (npu_output(i, &outs[i]) != NPU_OK)
			return nn_ov_end(oneshot, &nn_ov_stats.errors,
			                 NN_OV_SHOT_NO_OUTPUTS);

	/* No cache maintenance here.  The port does it inside Invoke(), at the
	 * two instants the arena changes hands (issue #46) -- on this thread now,
	 * the same two points; anything from out here is too early or too late. */
	e1 = tx_glue_epk_timer_ticks();
	t0 = (uint32_t)tx_time_get();
	if (npu_invoke() != NPU_OK)
		return nn_ov_end(oneshot, &nn_ov_stats.errors,
		                 NN_OV_SHOT_INVOKE_FAILED);
	t1 = (uint32_t)tx_time_get();
	e2 = tx_glue_epk_timer_ticks();

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
	 * console wait out a frame.
	 */
	if (!plugin_lease_take()) {
		if (!oneshot)
			nn_ov_bump(&nn_ov_stats.lease_timeouts);
		return nn_ov_end(oneshot, &nn_ov_stats.errors,
		                 NN_OV_SHOT_LEASE_TIMEOUT);
	}
	/*
	 * [!] NOTHING TO DECODE WITH, ONLY ON A ONE-SHOT (issue #104): a stream is
	 * refused at admission without a plugin.  The inference that ran is
	 * reported as the tensors it produced, published like any result.  Under
	 * the lease, because whether a plugin is there is decided by a load the
	 * gate keeps out -- but the lease is the rule for every path in.
	 */
	if (!nn_active_is_plugin()) {
		(void)nn_active_set_geom(&job.geom);   /* as `nn run` always did */
		(void)nn_ov_publish_raw(job.gen);
		plugin_lease_give();
		return nn_ov_end(oneshot, NULL, NN_OV_SHOT_PUBLISHED);
	}
	/*
	 * [!] THE GEOMETRY BEFORE THE DECODE, AND UNDER THE SAME HOLD (issue
	 * #127).  It is part of the plugin's result: decode() may call the base's
	 * to_frame(), which reads it, and a console holding the lease after this
	 * frame may ask the plugin for a report that does the same.  It is the
	 * geometry the producer cut THIS input by, carried in the job.  Written
	 * whatever the decode then says, because the record below is too.
	 */
	(void)nn_active_set_geom(&job.geom);   /* issue #103 */
	nd = nn_active_decode(outs, n_out);
	if (nd == NN_ACTIVE_NOT_HELD) {
		/* [!] Not reachable while the take above stands -- and if it ever
		 * does not, the decode did not run, so nothing is published.
		 * Counted by the entry check (plugin_lease_unheld()), not as a
		 * decoder error: the decoder was never asked. */
		plugin_lease_give();
		return nn_ov_end(oneshot, NULL, NN_OV_SHOT_NOT_HELD);
	}
	/*
	 * [!] PUBLISHED AT ONCE, WHATEVER IT SAYS (issue #118), under the
	 * generation the producer handed over.  `nn dets` reads the record, and
	 * the plugin's private result has just been rewritten -- so the record
	 * must describe this decode before anything else can ask, negative values
	 * included.  Still inside the lease, so a console that takes it next sees
	 * the record and the plugin's result describe the same frame, and the
	 * panel's lag reads the frame this result came from.
	 */
	(void)nn_rec_publish_external(nd, job.gen);
	nn_ov_res_frame = job.frame;
	nn_ov_result    = (nd < 0) ? NN_OV_RES_FAIL : NN_OV_RES_OK;
	plugin_lease_give();
	e3 = tx_glue_epk_timer_ticks();

	if (oneshot)
		return nn_ov_end(oneshot, NULL, NN_OV_SHOT_PUBLISHED);

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
		nn_ov_stats.last_ms   = t1 - t0;
		nn_ov_stats.last_ndet = nd;
		/* Only frames that completed every stage accumulate, so the three
		 * means describe one set: prep from the producer, the rest here. */
		nn_ov_prep_ticks   += job.prep_ticks;
		nn_ov_invoke_ticks += (uint32_t)(e2 - e1);
		nn_ov_decode_ticks += (uint32_t)(e3 - e2);
		nn_ov_prof_frames++;
		nn_ov_ndet = nd;
	}
	TX_RESTORE
	return 1;
}

static void nn_overlay_draw(void *ctx, uint16_t *fb, uint16_t fb_w,
                            uint16_t fb_h)
{
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
	 */
	{
		TX_INTERRUPT_SAVE_AREA
		struct plugin_painter paint;
		struct plugin_paint_budget bud;
		uint32_t spent, lag;

		/*
		 * [!] THE PLUGIN LEASE, TRIED ONCE INSIDE THE PANEL GUARD (issue
		 * #127).  Nothing a console or the worker does can be inside this
		 * plugin while it paints.  A refusal shows the frame without an
		 * overlay and is counted as this frame's miss -- the only one it can
		 * have, since the producer no longer asks (issue #129).
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
		if (!plugin_lease_try(PLUGIN_LEASE_PANEL))
			return;
		bud.pixels  = NN_OV_DRAW_PIXELS;
		bud.ops     = NN_OV_DRAW_OPS;
		bud.refused = 0u;
		plugin_paint_bind(&paint, &bud, fb, fb_w, fb_h);
		/* NN_ACTIVE_NOT_HELD paints nothing and is counted by the entry
		 * check itself; there is nothing more to do with it here. */
		(void)nn_active_draw(&paint);
		/* How many frames behind the picture this result is (issue #129):
		 * the frame being drawn less the frame the result was cut from,
		 * read under the same hold the worker wrote it in. */
		lag = nn_ov_cur_frame - nn_ov_res_frame;
		plugin_lease_give();

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
		nn_ov_stats.lag_sum += lag;
		nn_ov_stats.lag_n++;
		if (lag > nn_ov_stats.lag_max)
			nn_ov_stats.lag_max = lag;
		TX_RESTORE
	}
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
	nn_ov_stats.lag_sum        = 0u;
	nn_ov_stats.lag_n          = 0u;
	nn_ov_stats.lag_max        = 0u;
	nn_ov_stats.model_errors   = 0u;
	nn_ov_stats.decoder_errors = 0u;
	nn_ov_stats.last_ms    = 0u;
	nn_ov_stats.last_ndet  = 0;
	nn_ov_last_status      = 0;
	nn_ov_prep_ticks       = 0u;
	nn_ov_invoke_ticks     = 0u;
	nn_ov_decode_ticks     = 0u;
	nn_ov_prof_frames      = 0u;
	nn_ov_ndet             = 0;
	nn_ov_frame_no         = 0u;
	nn_ov_cur_frame        = 0u;
	nn_ov_res_frame        = 0u;
	nn_ov_result           = NN_OV_RES_NONE;
	nn_ov_oneshot          = 0u;
	nn_ov_shot             = NN_OV_SHOT_NONE;
	nn_ov_stop             = 0u;
	TX_RESTORE
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
	nn_ov_frame_no  = 0u;
	nn_ov_cur_frame = 0u;
	nn_ov_oneshot   = 1u;
	nn_ov_shot      = NN_OV_SHOT_NONE;
	nn_ov_stop      = 0u;
	TX_RESTORE
	return &nn_ov_vtable;
}

int nn_overlay_shot(void)
{
	return (int)nn_ov_shot;
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
	uint64_t prep, invoke, decode;
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
	TX_RESTORE

	/* The stage rows are only as good as their clock, and this port has the
	 * predicate for that -- the same one `thread` and `camera stats` use. */
	hz = tx_glue_epk_timer_hz();
	out->prof_ok     = (tx_glue_profile_ok(&why) && hz != 0u);
	out->prof_frames = frames;
	out->prep_us     = out->prof_ok ? nn_ov_us(prep,   hz) : 0u;
	out->invoke_us   = out->prof_ok ? nn_ov_us(invoke, hz) : 0u;
	out->decode_us   = out->prof_ok ? nn_ov_us(decode, hz) : 0u;
}
