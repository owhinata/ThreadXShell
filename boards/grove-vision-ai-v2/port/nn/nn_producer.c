/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    nn_producer.c
 * @brief   The camera producer's half of the camera -> inference glue (issue
 *          #140): the overlay's process(), run inside the panel sink's
 *          consume(), and the frame path's prep / wake.
 *
 * [!] PREP STAYS ON THE PRODUCER, INSIDE consume(), AND ONLY WHILE THE WORKER
 * WANTS A FRAME; the invoke .. publish is the worker's (nn_worker.c).  The
 * shared state is nn_camera_priv.h's, defined in nn_camera.c.
 */
#include "nn_camera.h"
#include "nn_camera_priv.h"

#include <stddef.h>

#include "camera.h"
#include "cam_dp.h"
#include "nn_preproc.h"
#include "nn_rec.h"
#include "npu.h"
#include "nn_worker.h"     /* the inference worker (issue #129) */
#include "nn_core_frame.h" /* the shared frame path (issue #130) */
#include "tx_glue.h"       /* the EPK's TIMER2: the stage clock (issue #60) */

/*
 * The producer's prep, into the input tensor, while the word is FILLING.
 * `pixels` is not used, and that is deliberate -- see nn_camera_process().
 */
int nn_ov_prep(void *ctx, unsigned part, unsigned nparts)
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

void nn_ov_infer_start(void *ctx)
{
	(void)ctx;
	nn_worker_wake();
}

int nn_camera_process(void *ctx, const void *pixels,
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
