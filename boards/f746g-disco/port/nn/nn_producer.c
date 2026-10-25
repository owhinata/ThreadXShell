/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    nn_producer.c
 * @brief   The camera producer thread's half of the camera -> inference glue
 * (issue #140; see nn_camera.c for the design and nn_camera_priv.h for the state).
 *
 * Everything here runs on the camera producer thread, inside the sink's
 * consume(): the frame path's prep (resize + convert straight into the input
 * tensor) and the wake of the worker.  camera_frame_put() stays the LAST
 * statement of consume().
 */
#include <stdint.h>

#include "tx_api.h"

#include "nn.h"
#include "nn_camera_priv.h"
#include "nn_det_record.h"
#include "nn_core_frame.h"   /* the shared frame path and its hand-over word (#130) */
#include "frame_pipeline.h"  /* struct frame_desc */

/* ---- RGB565 -> model input: nearest-neighbour resize + convert ------------ */

/* Convert one camera frame @p f (RGB565) into @p dst (HxWxC), formatted for the
 * model input dtype.  Reads only the sampled source pixels.
 *   - FLOAT32: RGB in [-1,1] (NNCAM_NORM_SIGNED) or [0,1] -- BlazeFace.
 *   - INT8   : uint8 - 128 (symmetric ~[-1,1], scale 1/128) -- MNIST/null stub. */
static void nncam_preprocess(const struct frame_desc *f, void *dst)
{
	const uint8_t *base = (const uint8_t *)f->data;
	uint32_t stride = f->stride ? f->stride : (uint32_t)f->width * 2u;
	uint16_t ow = nncam_in_w, oh = nncam_in_h, oc = nncam_in_c;
	uint16_t sw = f->width, sh = f->height;
	int is_f32 = (nncam_in_dtype == NN_DTYPE_FLOAT32);

	for (uint16_t oy = 0; oy < oh; oy++) {
		uint16_t sy = (uint16_t)((uint32_t)oy * sh / oh);
		const uint8_t *row = base + (uint32_t)sy * stride;
		for (uint16_t ox = 0; ox < ow; ox++) {
			uint16_t sx = (uint16_t)((uint32_t)ox * sw / ow);
			uint16_t px = (uint16_t)(row[sx * 2] | ((uint16_t)row[sx * 2 + 1] << 8));
			uint8_t rgb[3];
			uint32_t o = ((uint32_t)oy * ow + ox) * oc;

			rgb[0] = (uint8_t)(((px >> 11) & 0x1Fu) * 255u / 31u);
			rgb[1] = (uint8_t)(((px >> 5) & 0x3Fu) * 255u / 63u);
			rgb[2] = (uint8_t)((px & 0x1Fu) * 255u / 31u);

			if (is_f32) {
				float *o32 = (float *)dst + o;
				for (uint16_t c = 0; c < oc && c < 3; c++)
					o32[c] = nncam_norm_signed
					       ? (float)rgb[c] / 127.5f - 1.0f   /* [-1,1] */
					       : (float)rgb[c] / 255.0f;         /* [0,1]  */
			} else {
				int8_t *o8 = (int8_t *)dst + o;
				for (uint16_t c = 0; c < oc && c < 3; c++)
					o8[c] = (int8_t)((int)rgb[c] - 128);
			}
		}
	}
}

/* ---- the frame path's hooks (svc/nn/nn_core_frame.h, issue #130) ------------- */

/*
 * Preprocess the frame (@p ctx, the frame_desc consume() was handed, pin held)
 * STRAIGHT INTO the model's input tensor.  Called by the frame path only while
 * the word is FILLING, so the worker is not inside the interpreter.  One part:
 * this board is handed whole frames.
 *
 * [!] THE EPOCH TEST STAYS, and it is what makes a frame that straddles a base
 * detach or a stop go nowhere: the session is latched before the write and
 * re-read after it, and a frame whose session ended in between is refused --
 * the frame path then ABANDONs it (FILLING -> WANT) and the next frame starts
 * again.  Its generation is sampled under the same hold as the epoch, so even a
 * frame that passes is published only if no boundary moved since.
 */
int nncam_prep(void *ctx, unsigned part, unsigned nparts)
{
	const struct frame_desc *f = (const struct frame_desc *)ctx;
	struct nn_tensor *in;
	uint32_t epoch0;
	int stale;

	(void)part;
	(void)nparts;
	tx_mutex_get(&nncam_lock, TX_WAIT_FOREVER);
	epoch0 = nncam_epoch;
	nncam_job_gen = nn_det_record_gen(&nncam_rec);
	tx_mutex_put(&nncam_lock);

	/* The geometry was checked against this tensor at the start, and a model
	 * cannot be loaded or unloaded while a stream runs; a tensor that has gone
	 * anyway is an error, and nothing is written. */
	in = nn_input(nncam_model, 0);
	if (in == NULL || in->data == NULL || in->bytes < nncam_in_bytes) {
		nncam_error();
		return -1;
	}
	/* Resize + convert while the slot is still pinned by the caller (reads f->data). */
	nncam_preprocess(f, in->data);

	tx_mutex_get(&nncam_lock, TX_WAIT_FOREVER);
	stale = (nncam_epoch != epoch0 || !nncam_run);
	tx_mutex_put(&nncam_lock);
	return stale ? -1 : 0;
}

/* The producer has handed the frame over: wake the worker.  [!] After the
 * hand-over, never before -- the post means nothing until the word says HANDED. */
void nncam_infer_start(void *ctx)
{
	(void)ctx;
	(void)tx_semaphore_put(&nncam_sem);
}

/* ---- frame-pipeline sink: consume() ------------------------------------ */

/* Offer one delivered frame to the frame path, and count what became of it.
 * Reads f->data (through the prep).  Does NOT release the pipeline pin -- the
 * consume() caller owns pin release. */
static void nncam_ingest(const struct frame_desc *f)
{
	switch (nn_core_on_frame(&nncam_frame, &nncam_frame_ops, (void *)f, 0u, 1u)) {
	case NN_CORE_FR_HANDED:
		nnstat.frames++;
		nncam_gen_frames++;                 /* per stream, not per attach (#99)     */
		break;
	case NN_CORE_FR_SKIPPED:
		/* The worker was inside a job (or not armed yet): this frame is not
		 * inferred.  Not staged for later -- there is nowhere to stage it. */
		nnstat.drops++;
		nncam_gen_drops++;
		break;
	case NN_CORE_FR_RACED:
		/* The word left FILLING under a producer that was still writing:
		 * an invariant broken, counted rather than assumed. */
		nncam_error();
		break;
	case NN_CORE_FR_ABANDONED:   /* the session ended mid-prep, or no tensor (counted there) */
	case NN_CORE_FR_WROTE:       /* not reachable with one part                             */
	case NN_CORE_FR_NONE:
	default:
		break;
	}
}

/* The nn step of the camera sink (issue #142).  The sink calls it between
   entering and leaving the step and puts the pin itself, after the last step
   (port/camera/cam_sink.c) -- so what lets nn_camera_stop() settle for ONE count
   is the step's count reaching zero after the stop disarmed it.  Everything this
   step does happens inside the call; nothing may be left running after it
   returns, or that count would not see it. */
int nncam_consume(void *ctx, const struct frame_desc *f)
{
	(void)ctx;

	if (nncam_run)
		nncam_ingest(f);                    /* into the input tensor (pin held)     */
	return 0;
}
