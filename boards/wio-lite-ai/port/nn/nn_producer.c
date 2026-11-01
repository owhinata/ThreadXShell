/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Wio Lite AI ThreadX Shell Project
 */
/**
 * @file    nn_producer.c
 * @brief   The camera producer thread's half of the camera -> inference glue
 * (issue #140; see nn_camera.h for the design and nn_camera_priv.h for the state).
 *
 * Everything here runs on the camera's producer thread, inside the band fan-out
 * of port/camera/cam_band.c: the band callback, the frame path's prep (downsample
 * one band straight into the input tensor) and the wake of the worker.  Each band
 * must be done well inside a band period (~18.5 ms).
 */
#include "nn_camera_priv.h"

#include "camera.h"          /* CAMERA_BAND_ROWS, CAMERA_FRAME_WIDTH / _HEIGHT */
#include "nn.h"
#include "nn_core_frame.h"   /* the shared frame path (issue #130) */
#include "nn_input_contract.h" /* the per-element conversion (issue #162) */

#include "stm32h7xx_hal.h"   /* DWT */
#include "tx_api.h"
#include "mem_sections.h"    /* DTCM_BSS: CPU-only data out of AXI-SRAM (issue #46) */

#include <stddef.h>

/* ------------------------------------------------------------- preprocessing -- */

/*
 * Downsample output rows [oy0, oy_end) of the model input from one band.
 *
 * @p src is the band's first row (source row @p src_y0 of the full frame), tightly
 * packed RGB565, CAMERA_FRAME_WIDTH wide, in AXI-SRAM and already invalidated by the
 * camera driver.  It is 32-byte aligned, so unlike the donor -- which assembled each
 * pixel from two byte loads to stay alignment-agnostic on a pinned frame pointer --
 * this reads uint16_t directly.
 *
 * Layout is HWC (o = (oy*ow + ox) * oc), channel order RGB, matching a 1xHxWxC input.
 *
 * [!] WHAT ONE 8-BIT CHANNEL BECOMES IS THE SHARED CONVERSION (issue #162,
 * svc/nn/nn_input_contract.h), the same functions f746g-disco calls:
 * nn_input_f32() for a float32 input, nn_input_i8() for an int8 one.  This file
 * only samples the pixel and expands RGB565.
 *
 * [!] For a quantized input the value is the NORMALIZED float put through the
 * tensor's OWN scale/zero_point, not the donor's hardcoded (rgb - 128).  This
 * board's struct nn_tensor carries the quantization params and the donor's did not,
 * which is the only reason it had to assume (1/128, 0); assuming it here would be
 * silently wrong for any model quantized differently, and `nn norm` would do nothing
 * on a quantized input.  The 1/scale reciprocal is computed once per band rather
 * than per channel: a vdiv.f32 is ~14 cycles and there are 3 per pixel.
 *
 * [!] A USABLE SCALE IS GUARANTEED BY nn_camera_start(), which is why there is no
 * fallback here (owhinata/wio-lite-ai#51): its admission test, nn_input_admit(),
 * accepts an int8 input only with a positive NORMAL scale (so 1/scale is finite)
 * and a zero point inside int8, and a float32 input only 4-byte aligned.  There
 * used to be one -- the donor's (rgb - 128) when the scale read back as zero --
 * and it was not dead code: a PER-AXIS quantized tensor
 * arrives with TfLiteTensor::params.scale == 0, because its real parameters live in the
 * affine-quantization struct that svc/nn/nn.h does not expose.  So the fallback's
 * only reachable case was the one where it silently fed the model wrong pixels, with
 * the same hardcoded assumption the paragraph above rejects.  Refusing the model at
 * start() says so once, out loud, instead of per pixel, never.
 */
static void nncam_rows(const uint16_t *src, unsigned src_y0,
                       unsigned oy0, unsigned oy_end, struct nn_tensor *in)
{
	const unsigned ow = nncam_ow, oh = nncam_oh, oc = nncam_oc;
	const int is_f32 = (in->dtype == NN_DTYPE_FLOAT32);
	const int norm_signed = nncam_norm_signed;   /* read once per band */
	const float inv_scale = is_f32 ? 0.0f : (1.0f / in->scale);
	const int32_t zp = in->zero_point;
	unsigned oy, ox, c;

	for (oy = oy0; oy < oy_end; oy++) {
		unsigned sy = (unsigned)((uint32_t)oy * CAMERA_FRAME_HEIGHT / oh);
		const uint16_t *row = src + (size_t)(sy - src_y0) * CAMERA_FRAME_WIDTH;

		for (ox = 0; ox < ow; ox++) {
			unsigned sx = (unsigned)((uint32_t)ox * CAMERA_FRAME_WIDTH / ow);
			uint16_t px = row[sx];
			uint32_t o = ((uint32_t)oy * ow + ox) * oc;
			uint8_t rgb[3];

			/* Exact 5/6-bit -> 8-bit scaling, as the donor does.  The constant
			   divisors become a multiply-and-shift, so this costs no divide. */
			rgb[0] = (uint8_t)(((px >> 11) & 0x1Fu) * 255u / 31u);
			rgb[1] = (uint8_t)(((px >> 5) & 0x3Fu) * 255u / 63u);
			rgb[2] = (uint8_t)((px & 0x1Fu) * 255u / 31u);

			if (is_f32) {
				float *o32 = (float *)in->data + o;

				for (c = 0; c < oc && c < 3u; c++)
					o32[c] = nn_input_f32(rgb[c], norm_signed);
			} else {
				int8_t *o8 = (int8_t *)in->data + o;

				/* Rounding: +-0.5 then toward zero, as this board always
				   did.  Close to, not exactly, TFLM's QUANTIZE (std::round):
				   see nn_input_q8() for the two places they differ. */
				for (c = 0; c < oc && c < 3u; c++)
					o8[c] = nn_input_i8(rgb[c], norm_signed,
					                    inv_scale, zp);
			}
		}
	}
}

/* ----------------------------------------------------------- band ingest ------ */

/* One band, as the frame path's prep hook sees it. */
struct nncam_part {
	const uint16_t *px;
	unsigned        rows;
};

/*
 * Write one band into the input tensor: the frame path's prep hook, called on the
 * camera's producer thread only while the shared word is FILLING (issue #130).
 * Must finish well inside a band period (~18.5 ms); the DWT cycles below are what
 * proves it does, and `nn stream stats` reports the worst one.
 */
int nncam_prep(void *ctx, unsigned band, unsigned nparts)
{
	const struct nncam_part *p = ctx;
	struct nn_tensor *in;
	uint32_t t0, cyc;
	unsigned oy0, oy_end;

	(void)nparts;
	/* [!] Re-read every frame, never cached across a session: `nn model load`
	   rebuilds the interpreter and re-plans the arena, so this pointer moves.
	   (That load cannot happen WHILE we stream -- it needs the NN session, which
	   this stream holds -- but the cost of re-reading is one load.) */
	in = nn_input(nncam_model, 0);
	if (in == NULL || in->data == NULL) {
		/* Abandon this frame WITHOUT posting: the worker must not run inference
		   over a half-filled tensor, and the frame path hands the input back
		   to the next band 0 (ABANDON) -- the worker still wants a frame. */
		return -1;
	}

	oy0    = nncam_oy_bound(band, nncam_oh);
	oy_end = nncam_oy_bound(band + 1u, nncam_oh);

	t0 = DWT->CYCCNT;
	nncam_rows(p->px, band * p->rows, oy0, oy_end, in);
	cyc = DWT->CYCCNT - t0;
	nncam_ingest_last = cyc;
	if (cyc > nncam_ingest_max)
		nncam_ingest_max = cyc;
	return 0;
}

/* The producer has finished writing the last band: wake the worker.  [!] After
 * the hand-over, never before -- the post can only be observed once the
 * producer has given up the input (owhinata/wio-lite-ai#54). */
void nncam_infer_start(void *ctx)
{
	(void)ctx;
	(void)tx_semaphore_put(&nncam_frame_sem);
}

/* ------------------------------------------------- the one-shot's frame ------ */

/*
 * The one-shot frame the producer is on (issue #171), the producer's only:
 *   nncam_p_open   band 0 of this frame found the run open; bands 1..3 of a
 *                  frame whose band 0 was kept out do not reach the frame path
 *   nncam_p_run    the run that band 0 found open, and
 *   nncam_p_gen    the record's generation, sampled in the SAME hold -- what a
 *                  band this frame could not use is recorded against
 * Tied to the band 0 the producer is about to write, not to the worker's arm:
 * the worker re-samples its own on every arm, even one refused while a fill is
 * in flight, and handing that over would rewrite a frame's sample under it.  A
 * band 0 that writes over a fill cut part way samples again, which is right:
 * the bands after it belong to this band 0's frame.
 */
static uint32_t nncam_p_run DTCM_BSS;
static uint32_t nncam_p_gen DTCM_BSS;
static int      nncam_p_open DTCM_BSS;

/*
 * Band 0 of a one-shot frame: is the run still open?  Asked BEFORE the frame
 * path can BEGIN a fill, so a run that has ended -- its frame refused, its
 * inference failed -- never has another frame filled for it (FILLING keeps its
 * two ways out, svc/nn/nn_handoff.h).  One hold of nncam_det_lock: every holder
 * of it only copies, and none waits or sleeps inside it (the lease is always
 * taken outside), so the band waits at most one such copy.
 */
static int nncam_frame_open(void)
{
	uint32_t run, gen;

	if (tx_mutex_get(&nncam_det_lock, TX_WAIT_FOREVER) != TX_SUCCESS)
		return 0;
	run = nn_shot_open(&nncam_shot);
	gen = nn_det_record_gen(&nncam_rec);
	(void)tx_mutex_put(&nncam_det_lock);
	if (run == 0u)
		return 0;
	nncam_p_run = run;
	nncam_p_gen = gen;
	return 1;
}

/*
 * The producer abandoned a one-shot frame it could not use -- a band of the
 * wrong height, or no input tensor to write -- AFTER the ABANDON: the run ends
 * as PREP, if the generation sampled at this frame's band 0 is still the
 * record's (the test and the write in one hold of nncam_det_lock).  One a stop
 * got to first is the boundary's, not the run's end.
 */
static void nncam_frame_refused(void)
{
	if (tx_mutex_get(&nncam_det_lock, TX_WAIT_FOREVER) != TX_SUCCESS)
		return;
	(void)nn_shot_record(&nncam_shot, nncam_p_run, NN_SHOT_PREP,
	                     nn_det_record_admits(&nncam_rec, nncam_p_gen));
	(void)tx_mutex_put(&nncam_det_lock);
}

/* ----------------------------------------------------------- band ingest ------ */

/*
 * One band, on the camera's producer thread: nn's step of the band sink
 * (port/camera/cam_band.c), after the preview's.  The fill itself is the shared frame path's (issue #130): it
 * begins only at band 0 while the worker wants a frame, writes each band through
 * nncam_prep() while FILLING, and at band 3 hands the frame over and posts.
 */
void nncam_band(unsigned band, const uint16_t *px, unsigned rows)
{
	struct nncam_part part;

	/* [!] Not on a stream nn LOST (issue #142).  A confirmed end (a DCMI
	   overrun) emptied the camera's holders and marked nn; if the operator
	   restarts the camera, this step is still armed and is fed -- and must not
	   resume until nn itself re-arms (camera_stream_join() clears the mark).
	   First, so a lost step touches nothing: the worker drops the fill on its
	   own timeout path. */
	if (camera_stream_lost(&nncam_holder))
		return;

	/*
	 * [!] A ONE-SHOT TAKES FRAMES ONLY WHILE ITS RUN IS OPEN (issue #171).
	 * Asked at band 0, BEFORE the frame path can BEGIN a fill: a run that has
	 * ended leaves the word WANT behind the producer's ABANDON, and without
	 * this the next frame would be filled and handed for a run that is over.
	 * The answer holds for the frame: bands 1..3 of a frame kept out at band 0
	 * do not reach the frame path either.  nncam_oneshot is set by the start
	 * before the step is armed.  A stream does not ask (it is not a run) and
	 * takes no lock here.
	 */
	if (nncam_oneshot) {
		if (band == 0u)
			nncam_p_open = nncam_frame_open();
		if (!nncam_p_open)
			return;
	}

	/* The band tiling is derived from CAMERA_BAND_ROWS, and the driver's
	   contract is that every band is exactly that tall (port/camera/camera.h).  If
	   that ever stops being true the rows would be sampled from the wrong source
	   offsets and the image would simply be wrong -- so count it instead, and
	   drop the frame being filled (the producer's own ABANDON). */
	if (rows != CAMERA_BAND_ROWS) {
		if (nn_core_frame_abandon(&nncam_frame, &nncam_frame_ops) &&
		    nncam_oneshot)
			nncam_frame_refused();     /* after the ABANDON (#171) */
		nncam_error();
		return;
	}

	part.px   = px;
	part.rows = rows;
	switch (nn_core_on_frame(&nncam_frame, &nncam_frame_ops, &part, band,
	                         CAMERA_BANDS_PER_FRAME)) {
	case NN_CORE_FR_SKIPPED:
		nncam_skipped++;   /* a whole frame went by while the worker ran */
		break;
	case NN_CORE_FR_ABANDONED:
		/* nncam_prep() refused the band (no input tensor) and the frame
		 * path has ABANDONed the frame: a one-shot's run ends (#171). */
		if (nncam_oneshot)
			nncam_frame_refused();
		nncam_error();
		break;
	case NN_CORE_FR_RACED:
		/* The invariant this stream depends on: the worker and the producer
		 * never hold the input tensor at the same time.  Counted rather than
		 * assumed, because when it breaks the picture stays plausible -- part
		 * camera, part activations. */
		nncam_raced++;
		break;
	case NN_CORE_FR_HANDED:
		nncam_frames++;
		break;
	default:
		break;
	}
}
