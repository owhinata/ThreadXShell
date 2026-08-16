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

#include "blazeface.h"
#include "camera.h"
#include "cam_dp.h"
#include "cam_sensor.h"
#include "lcd_st7789.h"
#include "nn_preproc.h"
#include "npu.h"

/* Enough descriptors for any model handed to the decoder; BlazeFace needs
 * four.  Matches the cap in cmd_nn.c for the same reason it exists there. */
#define NN_OVERLAY_MAX_OUTPUTS 8

/* Box colour and thickness.  Green because it is the one hue the camera's
 * unbalanced output never produces at full saturation, so a box is never
 * mistaken for something in the scene. */
#define NN_OVERLAY_RGB565  0x07E0u
#define NN_OVERLAY_STROKE  2u

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
 */
static struct bf_det nn_ov_det[BF_MAX_DET];
static int           nn_ov_ndet;
static struct nn_preproc_geom nn_ov_geom;
static int           nn_ov_geom_ok;

static int nn_overlay_process(void *ctx, const void *pixels,
                              uint16_t w, uint16_t h)
{
	struct npu_tensor in;
	struct npu_tensor outs[NN_OVERLAY_MAX_OUTPUTS];
	unsigned n_out, i;
	uint32_t t0, t1;
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
	 * Keeping the model on the raw frame is what makes `nn preview`
	 * comparable with `nn detect`, and keeps issue #45's working detection
	 * as the baseline: #48 changes the geometry, and changing the colour
	 * path in the same commit would leave nothing to compare against.
	 * Feeding it the gamma-encoded image is a real experiment, and it is a
	 * separate one.
	 */
	(void)pixels;

	nn_ov_ndet    = 0;
	nn_ov_geom_ok = 0;

	/* Stop check 1 of 3: nothing started yet, so this is free. */
	if (nn_ov_stop) {
		nn_ov_stats.skipped++;
		return -1;
	}

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
	if (n_out > NN_OVERLAY_MAX_OUTPUTS) {
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

	nd = blazeface_decode(outs, n_out, nn_ov_det, BF_MAX_DET);
	if (nd < 0) {
		nn_ov_stats.errors++;
		return -1;
	}

	nn_ov_stats.inferences++;
	nn_ov_stats.detections += (uint32_t)nd;
	nn_ov_stats.last_ms   = t1 - t0;
	nn_ov_stats.last_ndet = nd;
	nn_ov_ndet    = nd;
	nn_ov_geom_ok = 1;

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

	/*
	 * Bound by lcd_st7789.h's callback contract: the panel guard is held,
	 * so nothing here blocks, sleeps, re-enters the driver or takes another
	 * lock.  lcd_rect_wire() is pure, which is what makes it the exception.
	 */
	if (!nn_ov_geom_ok)
		return;

	for (int k = 0; k < nn_ov_ndet; k++) {
		struct nn_preproc_box b;

		/* The SAME transform the input was built with, inverted -- and
		 * the reason it is a function rather than four multiplications
		 * here.  It also rejects the non-finite box a degenerate model
		 * can produce, before anything is cast to an integer. */
		if (nn_preproc_box(&nn_ov_geom, nn_ov_det[k].x, nn_ov_det[k].y,
		                   nn_ov_det[k].w, nn_ov_det[k].h, &b) != 0)
			continue;

		/* The frame maps 1:1 onto the panel at the origin (the sink
		 * blits the whole 320x240 frame at 0,0 in landscape), so frame
		 * pixels ARE framebuffer pixels.  If that ever stops being
		 * true, this is the line that has to know. */
		lcd_rect_wire(fb, fb_w, fb_h, b.x0, b.y0, b.x1, b.y1,
		              NN_OVERLAY_RGB565, NN_OVERLAY_STROKE);
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
	nn_ov_stats.last_ms    = 0u;
	nn_ov_stats.last_ndet  = 0;
	nn_ov_ndet             = 0;
	nn_ov_geom_ok          = 0;
	nn_ov_stop             = 0u;
	return &nn_ov_vtable;
}

void nn_overlay_request_stop(void)
{
	nn_ov_stop = 1u;
}

void nn_overlay_stats(struct nn_overlay_stats *out)
{
	if (out == NULL)
		return;
	*out = nn_ov_stats;
}
