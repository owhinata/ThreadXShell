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

#include "stm32h7xx_hal.h"   /* DWT */
#include "tx_api.h"

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
 * [!] For a quantized input the value is the NORMALIZED float put through the
 * tensor's OWN scale/zero_point, not the donor's hardcoded (rgb - 128).  This
 * board's struct nn_tensor carries the quantization params and the donor's did not,
 * which is the only reason it had to assume (1/128, 0); assuming it here would be
 * silently wrong for any model quantized differently, and `nn norm` would do nothing
 * on a quantized input.  The 1/scale reciprocal is computed once per band rather
 * than per channel: a vdiv.f32 is ~14 cycles and there are 3 per pixel.
 *
 * [!] scale > 0 IS GUARANTEED BY nn_camera_start(), which is why there is no fallback
 * here (owhinata/wio-lite-ai#51).  There used to be one -- the donor's (rgb - 128) when
 * the scale read back as zero -- and it was not dead code: a PER-AXIS quantized tensor
 * arrives with TfLiteTensor::params.scale == 0, because its real parameters live in the
 * affine-quantization struct that port/nn/nn.h does not expose.  So the fallback's
 * only reachable case was the one where it silently fed the model wrong pixels, with
 * the same hardcoded assumption the paragraph above rejects.  Refusing the model at
 * start() says so once, out loud, instead of per pixel, never.
 */
static void nncam_rows(const uint16_t *src, unsigned src_y0,
                       unsigned oy0, unsigned oy_end, struct nn_tensor *in)
{
	const unsigned ow = nncam_ow, oh = nncam_oh, oc = nncam_oc;
	const int is_f32 = (in->dtype == NN_DTYPE_FLOAT32);
	const float bias = nncam_norm_signed ? -1.0f : 0.0f;
	const float gain = nncam_norm_signed ? (1.0f / 127.5f) : (1.0f / 255.0f);
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
					o32[c] = (float)rgb[c] * gain + bias;
			} else {
				int8_t *o8 = (int8_t *)in->data + o;

				for (c = 0; c < oc && c < 3u; c++) {
					float f = ((float)rgb[c] * gain + bias) * inv_scale;
					/* Round half away from zero without libm: this
					   firmware links none, and lrintf() would pull it in
					   for three multiply-adds per pixel.  This is the
					   rounding TFLM's own QUANTIZE kernel does --
					   reference_ops::AffineQuantize() -> TfLiteRound()
					   -> std::round() -- so a model whose leading
					   QUANTIZE was stripped gets the identical tensor. */
					int q = (int)(f + (f >= 0.0f ? 0.5f : -0.5f)) + (int)zp;

					if (q < -128)
						q = -128;
					else if (q > 127)
						q = 127;
					o8[c] = (int8_t)q;
				}
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

	/* The band tiling is derived from CAMERA_BAND_ROWS, and the driver's
	   contract is that every band is exactly that tall (port/camera/camera.h).  If
	   that ever stops being true the rows would be sampled from the wrong source
	   offsets and the image would simply be wrong -- so count it instead, and
	   drop the frame being filled (the producer's own ABANDON). */
	if (rows != CAMERA_BAND_ROWS) {
		(void)nn_core_frame_abandon(&nncam_frame, &nncam_frame_ops);
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
