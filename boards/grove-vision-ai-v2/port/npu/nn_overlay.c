/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Face detection over the live preview (issue #48).  See nn_overlay.h for why
 * this lives in the port rather than in cmds/.
 */
#include "nn_overlay.h"

#include <stddef.h>

#include "tx_api.h"        /* tx_time_get(): ThreadX ticks, 1 ms here */

#include "npu_desc.h"
#include "nn_active.h"
#include "plugin_paint.h"
#include "plugin_abi.h"
#include "camera.h"
#include "cam_dp.h"
#include "cam_sensor.h"
#include "lcd_st7789.h"
#include "nn_preproc.h"
#include "npu.h"
#include "tx_glue.h"       /* the EPK's TIMER2: the stage clock (issue #60) */

/*
 * [!] Set from the SHELL thread, read on the PRODUCER thread.
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
 * Written on the producer thread only, with no critical section -- the same
 * discipline as camera.c's cam_prof, and sound for the same reason: every
 * reader snapshots under TX_DISABLE, and the console thread that reads them
 * cannot preempt the producer mid-add (it is strictly below it).  Only frames
 * that completed all three stages accumulate, so the three means describe the
 * same set of frames; a frame that failed mid-way vanishes into `sink`'s
 * remainder, which is where every other anomaly in that column already goes.
 */
static uint64_t nn_ov_prep_ticks;
static uint64_t nn_ov_invoke_ticks;
static uint64_t nn_ov_decode_ticks;
static uint32_t nn_ov_prof_frames;

/*
 * This frame's detections, produced by process() and consumed by draw().
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
 * Static because nothing here may ever be freed under a producer -- or now a
 * panel thread -- that did not acknowledge a stop (see nn_overlay.h).
 *
 * [!] NO BOX ARRAY SINCE ISSUE #104.  A stream only runs with a plugin loaded,
 * and a plugin's result is its own -- it paints through the painter, and this
 * file never learns what shape the result has.
 */
/* The most recent decode's status, so `nn stream`'s summary can say WHY it
 * annotated nothing rather than only that it did not. */
static int           nn_ov_last_status;
static int           nn_ov_ndet;
static struct nn_preproc_geom nn_ov_geom;

/*
 * How deep the stack already is at the instant a decoder is CALLED (issue #103).
 *
 * [!] THIS IS NOT THE THREAD'S PEAK, AND THE DIFFERENCE IS THE WHOLE REASON IT
 * EXISTS.  `thread` reports a peak by scanning ThreadX's 0xEF fill: the deepest
 * the thread ever got, anywhere on any path.  What a plugin admission policy
 * needs is how much is ALREADY SPENT at the one instant a plugin is entered,
 * because what it may have is the rest.  Issue #101 wrote `2048 - 544` as if the
 * peak answered that question; it does not, and this is the measurement that
 * does.
 *
 * Taken at the resident decoder's call sites, which are exactly the sites a
 * plugin will occupy in Step 1b -- so the number is about the PLACE, not about
 * whoever is standing in it, and it is worth having before the plugin exists.
 *
 * The address of a local is used rather than __get_PSP(): in Thread mode SP is
 * PSP, and a local sits within a few bytes of it without assuming which stack
 * pointer the compiler kept anything in.
 */
/*
 * What one plugin draw() may spend, and why these numbers.
 *
 * The panel guard is held for the whole of draw(), and everything else that
 * wants the panel is failing its non-blocking acquire meanwhile.  The staged
 * blit that follows costs about 775 us (issue #71), so a draw that approached
 * that would double the window.
 *
 * A full frame is 320 x 240 = 76,800 pixels, and at roughly four cycles a pixel
 * on a 400 MHz core that is already about 770 us -- the whole staging budget.
 * So the cap is a quarter of a frame: enough for a label bar (320 x 16 = 5,120)
 * or for every box BF_MAX_DET allows, and far enough below the frame that the
 * panel path's timing is not the thing being spent.
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

static uint32_t nn_ov_depth_decode;   /* high-water, producer thread */
static uint32_t nn_ov_depth_draw;     /* high-water, panel thread    */

static void nn_ov_note_depth(uint32_t *hw)
{
	TX_THREAD *t = tx_thread_identify();
	uint8_t    here;
	uintptr_t  sp = (uintptr_t)&here;
	uintptr_t  lo, hi;
	uint32_t   used;

	if (t == NULL)
		return;                     /* not on a thread; nothing to say */
	lo = (uintptr_t)t->tx_thread_stack_start;
	hi = lo + (uintptr_t)t->tx_thread_stack_size;
	if (sp < lo || sp > hi)
		return;                     /* not this thread's stack after all */

	used = (uint32_t)(hi - sp);
	if (used > *hw)
		*hw = used;
	/* One writer per site -- process() only ever runs on the producer and
	 * draw() only on the panel thread, and the pipeline's one-outstanding-frame
	 * rule keeps even those two from overlapping.  A reader may see a stale
	 * value; it cannot see a torn one, because a u32 store is atomic here. */
}

static int nn_overlay_process(void *ctx, const void *pixels,
                              uint16_t w, uint16_t h)
{
	struct npu_tensor in;
	struct npu_tensor outs[NPU_DESC_MAX_OUTPUTS];
	unsigned n_out, i;
	uint32_t t0, t1;
	uint32_t e0, e1, e2, e3;
	int nd;

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

	nn_ov_ndet    = 0;

	/* Stop check 1 of 3: nothing started yet, so this is free. */
	if (nn_ov_stop) {
		nn_ov_stats.skipped++;
		return -1;
	}

	/*
	 * Stage clocks (issue #60).  Everything from here to the invoke is
	 * `prep`: the tensor and geometry setup is microseconds, so the row
	 * effectively reads as the crop/resize -- but it is measured from HERE
	 * so that prep + invoke + decode covers this function without a gap,
	 * and the difference against `camera stats`' sink row is exactly the
	 * hand-off plus whatever preempted the producer inside it.
	 */
	e0 = tx_glue_epk_timer_ticks();

	if (npu_input(&in) != NPU_OK) {
		nn_ov_stats.errors++;
		return -1;
	}
	if (in.rank != 4 || in.dims[3] != 3) {
		nn_ov_stats.errors++;
		return -1;
	}
	if (nn_preproc_geom(CAM_FRAME_WIDTH, CAM_FRAME_HEIGHT,
	                    (uint32_t)in.dims[2], (uint32_t)in.dims[1],
	                    &nn_ov_geom) != 0) {
		nn_ov_stats.errors++;
		return -1;
	}
	if (in.bytes < (size_t)in.dims[2] * (size_t)in.dims[1] * 3u) {
		nn_ov_stats.errors++;
		return -1;
	}

	n_out = npu_output_count();
	if (n_out > NPU_DESC_MAX_OUTPUTS) {
		nn_ov_stats.errors++;
		return -1;
	}
	for (i = 0; i < n_out; i++)
		if (npu_output(i, &outs[i]) != NPU_OK) {
			nn_ov_stats.errors++;
			return -1;
		}

	if (nn_preproc_fill(camera_raw_frame(), CAM_FRAME_WIDTH,
	                    CAM_FRAME_HEIGHT, &nn_ov_geom,
	                    (uint8_t *)in.data) != 0) {
		nn_ov_stats.errors++;
		return -1;
	}
	e1 = tx_glue_epk_timer_ticks();

	/*
	 * Stop check 2 of 3, and the one that matters: this is the last instant
	 * before the expensive, uninterruptible part.  Everything above is
	 * microseconds; what follows can be the whole NPU timeout if an
	 * interrupt is lost.
	 */
	if (nn_ov_stop) {
		nn_ov_stats.skipped++;
		return -1;
	}

	/* No cache maintenance here.  The port does it inside Invoke(), at the
	 * two instants the arena changes hands (issue #46); anything from out
	 * here is either too early or too late. */
	t0 = (uint32_t)tx_time_get();
	if (npu_invoke() != NPU_OK) {
		nn_ov_stats.errors++;
		return -1;
	}
	t1 = (uint32_t)tx_time_get();
	e2 = tx_glue_epk_timer_ticks();

	/* The producer's call site.  Recorded BEFORE the call, so the number is the
	 * depth a callee inherits rather than the depth including it. */
	nn_ov_note_depth(&nn_ov_depth_decode);

	nd = nn_active_decode(outs, n_out);
	if (nd < 0) {
		/* [!] There is no console on this path, so the only way a decode
		 * failure can be told apart afterwards is if it is counted apart
		 * (issue #97).  A bare errors++ makes "the open model is not
		 * BlazeFace" and "the decoder was never initialised" the same
		 * number, and they call for opposite investigations. */
		if (nd == BF_ERR_MODEL)
			nn_ov_stats.model_errors++;
		else
			nn_ov_stats.decoder_errors++;
		nn_ov_stats.errors++;
		nn_ov_last_status = nd;
		return -1;
	}
	nn_ov_last_status = BF_OK;
	e3 = tx_glue_epk_timer_ticks();

	nn_ov_stats.inferences++;
	nn_ov_stats.detections += (uint32_t)nd;
	nn_ov_stats.last_ms   = t1 - t0;
	nn_ov_stats.last_ndet = nd;
	nn_ov_prep_ticks   += (uint32_t)(e1 - e0);
	nn_ov_invoke_ticks += (uint32_t)(e2 - e1);
	nn_ov_decode_ticks += (uint32_t)(e3 - e2);
	nn_ov_prof_frames++;
	nn_ov_ndet    = nd;
	nn_active_set_geom(&nn_ov_geom);   /* issue #103 */

	/* Stop check 3 of 3: a stop that arrived during the inference should not
	 * be followed by drawing on a panel the caller is about to stop using. */
	if (nn_ov_stop) {
		nn_ov_stats.skipped++;
		return -1;
	}
	return 0;
}

static void nn_overlay_draw(void *ctx, uint16_t *fb, uint16_t fb_w,
                            uint16_t fb_h)
{
	(void)ctx;

	/* The panel thread's call site -- the tight one.  This is the number the
	 * draw allowance is computed from. */
	nn_ov_note_depth(&nn_ov_depth_draw);

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
		struct plugin_painter paint;
		struct plugin_paint_budget bud;

		bud.pixels  = NN_OV_DRAW_PIXELS;
		bud.ops     = NN_OV_DRAW_OPS;
		bud.refused = 0u;
		plugin_paint_bind(&paint, &bud, fb, fb_w, fb_h);
		nn_active_draw(&paint);

		/* What it actually spent, so the cap can be judged against something
		 * rather than defended in the abstract. */
		if (NN_OV_DRAW_PIXELS - bud.pixels > nn_ov_draw_spent)
			nn_ov_draw_spent = NN_OV_DRAW_PIXELS - bud.pixels;
		nn_ov_draw_refused += bud.refused;
	}
}

static const struct cam_lcd_overlay nn_ov_vtable = {
	.ctx     = NULL,
	.process = nn_overlay_process,
	.draw    = nn_overlay_draw,
};

const struct cam_lcd_overlay *nn_overlay_arm(void)
{
	nn_ov_stats.inferences = 0u;
	nn_ov_stats.detections = 0u;
	nn_ov_stats.skipped    = 0u;
	nn_ov_stats.errors     = 0u;
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
	nn_ov_stop             = 0u;
	return &nn_ov_vtable;
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
	 * by the producer thread, and half of a 64-bit add is not a slightly
	 * wrong number but a wildly wrong one.  Same treatment as the camera's
	 * profile and the panel sink's, for the same reason.
	 */
	TX_DISABLE
	*out   = nn_ov_stats;
	prep   = nn_ov_prep_ticks;
	invoke = nn_ov_invoke_ticks;
	decode = nn_ov_decode_ticks;
	frames = nn_ov_prof_frames;
	TX_RESTORE

	/* The stage rows are only as good as their clock, and this port has the
	 * predicate for that -- the same one `thread` and `camera stats` use. */
	hz = tx_glue_epk_timer_hz();
	out->prof_ok     = (tx_glue_profile_ok(&why) && hz != 0u);
	out->prof_frames = frames;
	out->prep_us     = out->prof_ok ? nn_ov_us(prep,   hz) : 0u;
	out->invoke_us   = out->prof_ok ? nn_ov_us(invoke, hz) : 0u;
	out->decode_us   = out->prof_ok ? nn_ov_us(decode, hz) : 0u;
	out->depth_decode  = nn_ov_depth_decode;
	out->depth_draw    = nn_ov_depth_draw;
	out->draw_spent    = nn_ov_draw_spent;
	out->draw_refused  = nn_ov_draw_refused;
}
