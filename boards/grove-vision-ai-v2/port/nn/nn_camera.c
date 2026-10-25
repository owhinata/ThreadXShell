/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Face detection over the live preview (issue #48).  See nn_camera.h for why
 * this lives in the port rather than in cmds/.
 *
 * This file is the console side since issue #140: arm / arm_oneshot /
 * request_stop / join / stats, the one-shot's ending, the frame path's ops
 * table and the overlay vtable -- and it DEFINES every object the glue's files
 * share (nn_camera_priv.h says who writes and resets each).  The producer's
 * half is nn_producer.c, the worker's nn_worker.c, the panel's nn_panel.c.
 */
#include "nn_camera.h"
#include "nn_camera_priv.h"

#include <stddef.h>

#include "tx_api.h"        /* tx_time_get(): ThreadX ticks, 1 ms here */

#include "cam_holders.h"   /* struct cam_holder: nn's place on the camera (#142) */
#include "nn_active.h"
#include "plugin_lease.h"
#include "nn_rec.h"
#include "nn_core_frame.h" /* the shared frame path (issue #130) */
#include "tx_glue.h"       /* the EPK's TIMER2: the stage clock (issue #60) */

/*
 * [!] Set from the SHELL thread, read on the PRODUCER and WORKER threads.
 *
 * volatile, and that is the whole of the synchronisation: it is a single word,
 * every write is a plain store of 0 or 1, and no decision anywhere depends on
 * two reads of it agreeing.  A missed update costs one more inference before
 * the stream stops, which is exactly the cost the design already accepts (see
 * nn_camera_request_stop()).
 */
volatile uint8_t nn_ov_stop;

struct nn_camera_stats nn_ov_stats;

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
uint64_t nn_ov_prep_ticks;
uint64_t nn_ov_invoke_ticks;
uint64_t nn_ov_decode_ticks;
/* The worker's round trip, hand-over to publish, over the same frames (#129). */
uint64_t nn_ov_cycle_ticks;
uint32_t nn_ov_prof_frames;

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
 * panel thread that did not acknowledge a stop (see nn_camera.h).
 *
 * [!] NO BOX ARRAY SINCE ISSUE #104.  A stream only runs with a plugin loaded,
 * and a plugin's result is its own -- it paints through the painter, and this
 * file never learns what shape the result has.
 */
/* The most recent decode's status, so `nn stream`'s summary can say WHY it
 * annotated nothing rather than only that it did not. */
int           nn_ov_last_status;
int           nn_ov_ndet;

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
 * What one plugin draw() actually spends, and how often it was refused (issue
 * #103) -- the cap and why it is what it is are with the paint, in nn_panel.c.
 */
uint32_t nn_ov_draw_spent;     /* high-water, pixels charged  */
uint32_t nn_ov_draw_refused;   /* primitives refused for want */

/*
 * The job the producer hands the worker (struct nn_ov_job, nn_camera_priv.h,
 * where the producer / worker split is described).
 */
struct nn_ov_job nn_ov_job;

/*
 * The frame path's state (svc/nn_core_frame.h): the hand-over word, and the
 * frame numbers draw() reads its lag from.  Static and never freed, like the
 * rest of this file.
 */
struct nn_core_frame nn_ov_core;

/*
 * What the latest publish of THIS stream was, for process()'s answer: draw this
 * frame or not.  Written by the worker right after the publish, inside the
 * plugin lease (nn_ov_publish()), read by the producer
 * as one byte.  NONE until the first decode is published.
 */
volatile uint8_t nn_ov_result;   /* nn_ov_publish(), in the lease */

/*
 * nn's place among the camera's holders (issue #142).  `nn stream start` and
 * `nn run` hold the camera's stream to the panel -- starting it, and holding the
 * operator's place too, when nothing streams -- and nn's stop gives up this
 * place only, so the camera keeps streaming for the operator.
 */
const struct cam_holder nn_cam_holder = { "nn" };

/*
 * `nn run` (issue #129): the same producer and worker, for ONE frame.  Set by
 * nn_camera_arm_oneshot(), cleared by nn_camera_arm(); written only while the
 * worker is parked and the overlay is off the sink with nothing counted in it
 * (issue #142) -- the producer reads it only inside the overlay.
 *
 * [!] A ONE-SHOT COUNTS NOTHING HERE.  The stream statistics are what `nn stream
 * stats` reads after a stream has ended, and `nn run` never touched them; it
 * still does not.  What it reports is its own ending, below.
 */
uint8_t nn_ov_oneshot;
/* How the one-shot's frame ended, NN_OV_SHOT_* (nn_camera.h); NONE while it
 * has not.  Written once, by the producer (a frame it could not prepare) or by
 * the worker (after its publish, before it parks) -- never both, since the
 * producer writes it only before the hand-over and the worker only after. */
volatile uint8_t nn_ov_shot;
/* With NN_OV_SHOT_NO_OUTPUTS: the output that could not be read.  Written
 * before nn_ov_shot, by the same thread. */
volatile uint8_t nn_ov_shot_index;

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

static int nn_ov_is_plugin(void *ctx)
{
	(void)ctx;
	return nn_active_is_plugin();
}

const struct nn_core_frame_ops nn_ov_ops = {
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

int nn_camera_want(void)
{
	return nn_core_frame_want(&nn_ov_core, &nn_ov_ops);
}

int nn_camera_join(void)
{
	return nn_core_frame_join(&nn_ov_core, &nn_ov_ops);
}

static const struct cam_lcd_overlay nn_ov_vtable = {
	.ctx     = NULL,
	.process = nn_camera_process,
	.draw    = nn_camera_draw,
};

/*
 * `nn run`'s overlay: the same preparation and worker, and NO draw (issue
 * #142).  On a stream the operator is watching, a one-shot must not paint --
 * there is nothing of its own to paint until its worker has published, and
 * what the plugin would paint before that is the LAST stream's result.
 */
static const struct cam_lcd_overlay nn_ov_oneshot_vtable = {
	.ctx     = NULL,
	.process = nn_camera_process,
	.draw    = NULL,
};

const struct cam_lcd_overlay *nn_camera_arm(void)
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
	 * here -- arm() runs after the old stream is quiescent and before the
	 * overlay is put on the sink (issue #142: the camera itself may already
	 * be streaming for the operator, with nothing of nn's in it) -- and the
	 * reset is deliberately NOT done on stop, so
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
	 * hands it a frame until the overlay is put on -- so nothing below has a
	 * writer.
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

const struct cam_lcd_overlay *nn_camera_arm_oneshot(void)
{
	TX_INTERRUPT_SAVE_AREA

	/*
	 * Only what the producer and the worker read to decide; NOT the stream's
	 * counters, which describe the last stream until the next one is armed.
	 * Nothing writes these now: the worker is parked (the caller armed it
	 * IDLE -> WANT and the overlay is not on yet to hand it a frame).
	 */
	TX_DISABLE
	nn_ov_oneshot   = 1u;
	nn_ov_shot      = NN_OV_SHOT_NONE;
	nn_ov_shot_index = 0u;
	nn_ov_stop      = 0u;
	TX_RESTORE
	/* The frame numbers, and not the stream's lag figures. */
	nn_core_frame_reset(&nn_ov_core, &nn_ov_ops, 0);
	return &nn_ov_oneshot_vtable;
}

int nn_camera_shot(void)
{
	return (int)nn_ov_shot;
}

unsigned nn_camera_shot_index(void)
{
	return (unsigned)nn_ov_shot_index;
}

void nn_camera_request_stop(void)
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

void nn_camera_stats(struct nn_camera_stats *out)
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
	 * predicate for that -- the same one `thread` and `camera profile` use. */
	hz = tx_glue_epk_timer_hz();
	out->prof_ok     = (tx_glue_profile_ok(&why) && hz != 0u);
	out->prof_frames = frames;
	out->prep_us     = out->prof_ok ? nn_ov_us(prep,   hz) : 0u;
	out->invoke_us   = out->prof_ok ? nn_ov_us(invoke, hz) : 0u;
	out->decode_us   = out->prof_ok ? nn_ov_us(decode, hz) : 0u;
	out->cycle_us    = out->prof_ok ? nn_ov_us(cycle,  hz) : 0u;
}
