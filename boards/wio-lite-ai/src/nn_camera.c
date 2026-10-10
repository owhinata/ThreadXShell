/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Wio Lite AI ThreadX Shell Project
 */
/**
 * @file    nn_camera.c
 * @brief   Camera -> BlazeFace inference glue (owhinata/wio-lite-ai#9 phase 3).  See
 * nn_camera.h for the design and, in particular, for why there is no staging buffer.
 */
#include "nn_camera.h"

#include "cam_band.h"
#include "camera.h"
#include "nn.h"
#if BSP_ENABLE_LCD
#include "cam_preview.h"
#endif
#include "nn_active.h"
#include "nn_report.h"
#include "plugin_lease.h"
#include "nn_det_record.h"
#include "nn_core_frame.h"   /* the shared frame path (issue #130) */
#include "nn_desc.h"         /* nn_tensor -> tensor_desc (issue #121) */
#include "nn_decode_count.h" /* how a decode's answer is counted (#130 6c) */
#include "psram.h"

#include "stm32h7xx_hal.h"   /* HAL_GetTick, SystemCoreClock, DWT */
#include "tx_api.h"

#define LOG_TAG "nncam"
#include "log.h"
#include "mem_sections.h"    /* DTCM_BSS: CPU-only data out of AXI-SRAM (issue #46) */

#include <stddef.h>
#include <string.h>

/*
 * Priority 18, which nothing else in this firmware uses.
 *
 * BELOW the CLI (16) so `nn stream stop` always reaches the console -- inference is
 * a monolithic ~373 ms call with no yield in it, which is precisely why it must sit
 * under the thing that stops it.  BELOW the preview (12) and the camera producer
 * (10) so inference never delays the display or the band deadline.
 */
#define NNCAM_PRIO   18u

/*
 * How long this thread waits for the result lease before giving up on a frame
 * (issue #110) is the lease's own one bound since issue #130:
 * PLUGIN_LEASE_WAIT_MS in port/plugin/plugin_lease.h.  Generous against the
 * things that legitimately hold it -- a console capturing a report, a panel
 * drawing -- and finite so that a wedged holder costs frames rather than the
 * worker.
 */
/*
 * 3,072 B in DTCM.  The same inference measured a 1,940 B peak on the CLI thread in
 * phase 2c, and blazeface_decode() adds ~250 B (a 64-byte NMS bitmap plus the
 * detection array), so this is ~1.4x the expected peak.
 *
 * [!] That margin is thinner than it looks from the phase 2c notes, which recorded
 * "DTCM free 15,808 B".  That figure counted the 8 KB main-stack reservation as
 * free; the number the linker ASSERT actually enforces (_dtcm_used_end <=
 * _smsp_stack) was 7,616 B, and this stack spends 3,072 of it.  `free` prints the
 * high-water mark for exactly this reason -- there is no MSPLIM on ARMv7-M and an
 * MPU guard page would lock the part up rather than report (see mem_sections.h).
 */
#define NNCAM_STACK  NNCAM_STACK_BYTES   /* the value is in nn_camera.h (#108) */

/*
 * How long the worker waits for a frame before re-checking the run flag.  Without a
 * bound it would wedge on a semaphore nothing will post: a DCMI overrun or a stream
 * that stops underneath ends the band flow with the worker still wanting a frame.
 */
#define NNCAM_FRAME_WAIT_TICKS 100u

/*
 * How long nn_camera_stop() waits for the worker to leave the run loop, in ticks
 * (1 ms).  One inference is ~373 ms and the worker is below the caller in priority,
 * so it normally clears in well under half of this; the bound exists so a wedged
 * worker reports rather than hangs the console.
 */
#define NNCAM_STOP_TICKS 1500

static TX_THREAD    nncam_thread;
static UCHAR        nn_worker_stack[NNCAM_STACK] DTCM_BSS __attribute__((aligned(8)));
static TX_SEMAPHORE nncam_start_sem;   /* worker idles here between streams   */
static TX_SEMAPHORE nncam_frame_sem;   /* producer -> worker: your frame is in */
static TX_MUTEX     nncam_det_lock;    /* guards the published detections      */
static int          nncam_created;

static struct nn_model *nncam_model;

/* Set by start/stop (thread context), read by the worker and the band callback. */
static volatile int nncam_run;
/* The running session was started by `nn stream start` (a panel was required),
 * so it is the one kind a re-arm may take back up (issue #120).  Written only
 * by nn_camera_start() on the shell side, before nncam_run is raised. */
static int nncam_rearmable;
/* The running session is a `nn run`'s (no panel was required), whose decodes
 * are not counted in the stream's errors (issue #130 step 6c, as on
 * grove-vision-ai-v2): it reports its own result.  Written only by
 * nn_camera_start() on the shell side, before nncam_run is raised, and kept by
 * a re-arm (which only a stream's session takes). */
static int nncam_oneshot;
/* The worker is inside the run loop, i.e. it may touch the tensors at any moment. */
static volatile int nncam_worker_busy;
/*
 * WHO MAY TOUCH THE INPUT TENSOR (issue #130): the shared hand-over word,
 * svc/nn_handoff.h, stepped only by svc/nn_core_frame.c.  It replaced three
 * flags that said the same thing in pieces -- `want_frame` (WANT), `filling`
 * (FILLING, latched at band 0 and never mid-frame) and `infer_active` (RUNNING:
 * the arena reuses the input's space for intermediates, so a producer write
 * there is corruption).  One word cannot hold two of them at once.
 */
static struct nn_core_frame nncam_frame;
/* Do we still hold the NN session + the OCTOSPI1 guard? */
static volatile int nncam_holds_guards;

static uint32_t nncam_infers, nncam_frames, nncam_skipped, nncam_errors;
/* Of nncam_errors: decodes the plugin refused, apart (issue #97 / #130 step 6c,
 * nn_decode_count.h).  Written by the worker only. */
static uint32_t nncam_model_errors, nncam_decoder_errors;
/* Diagnostics for the ownership invariant (owhinata/wio-lite-ai#54).  `raced` must stay
   0: it counts bands
 * that found the input taken from under them while they wrote it -- since issue #130
 * the shared word's own check (NN_CORE_FR_RACED).  `stale` counts posts the
 * pre-arm drain threw away, and wake-ups that found no hand-over to take. */
static uint32_t nncam_raced, nncam_stale_posts;
static uint32_t nncam_ingest_last, nncam_ingest_max, nncam_infer_cyc;
static uint32_t nncam_start_tick;

/* Stack already spent at the two sites a plugin will occupy (issue #108).  One
 * writer each -- the worker for DECODE, the preview thread for DRAW -- and a u32
 * store cannot tear here, so a reader may see a stale value but never a torn
 * one.  Only ever written with a real measurement (see nn_camera_note_depth_at()),
 * so the reset at start cannot be raced into reporting a stale high-water. */
static uint32_t nncam_depth_decode, nncam_depth_draw, nncam_depth_shell;

static int nncam_norm_signed;   /* 0 = [0,1] (default), 1 = [-1,1] */
static int nncam_overlay;

/*
 * The published decode: boxes AND the diagnostics that describe them, plus the
 * session generation that keeps a stopped session's in-flight decode from landing
 * in a live one.  The decisions live in svc/nn_det_record.c so they can be tested
 * -- the ordering they guard against cannot be produced deterministically on
 * hardware -- and the storage and the lock are ours.
 */
static struct nn_det_record nncam_rec;

/* Input geometry, latched at start so the band callback does no shape work. */
static unsigned nncam_ow, nncam_oh, nncam_oc;

/* ------------------------------------------------------------------ guards ---- */

/*
 * Idempotent under a PRIMASK critical section, because two different threads can
 * legitimately try to be the last one out: nn_camera_stop() on the console, and the
 * worker on its way off the run loop after a stop that timed out.  Exactly one of
 * them performs the release.
 */
static void nncam_guards_give(void)
{
	uint32_t primask = __get_PRIMASK();
	int mine;

	__disable_irq();
	mine = nncam_holds_guards;
	nncam_holds_guards = 0;
	__set_PRIMASK(primask);

	if (mine) {
		psram_release();
		nn_session_release();
	}
}

/* ------------------------------------------------------------- preprocessing -- */

/*
 * First output row belonging to band @p band, for a model @p oh rows tall.
 *
 * Nearest-neighbour sampling puts output row oy on source row floor(oy*SRC_H/oh), so
 * oy belongs to band b exactly when that lands in [b*ROWS, (b+1)*ROWS) --
 * i.e. oy in [ceil(b*ROWS*oh / SRC_H), ceil((b+1)*ROWS*oh / SRC_H)).  For the 128
 * input this is the exact 32-rows-per-band split the design assumes (band b covers
 * output rows [32b, 32b+32), whose source rows [60b, 60b+58] lie wholly inside band
 * b), and there is no band-boundary sampling case at all.  Written in the general
 * form so a differently sized model input tiles correctly instead of silently
 * sampling across a band it was never handed; nn_camera_start() checks that the
 * tiling actually covers [0, oh).
 */
static unsigned nncam_oy_bound(unsigned band, unsigned oh)
{
	uint32_t n = (uint32_t)band * (uint32_t)CAMERA_BAND_ROWS * (uint32_t)oh;

	return (unsigned)((n + CAMERA_FRAME_HEIGHT - 1u) / CAMERA_FRAME_HEIGHT);
}

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
static int nncam_prep(void *ctx, unsigned band, unsigned nparts)
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

/* ---------------------------------------------------------------- worker ------ */

/*
 * Publish "an inference ran and nothing decoded it", under the detection lock
 * (issue #116).
 *
 * This is what the worker has to say for a model with no plugin beside it: this
 * firmware carries no decoder, the outputs are there, and only an
 * interpretation of them is missing.
 *
 * @return non-zero if it was taken.  A publish from a session that has since
 *         ended is DROPPED -- svc/nn_det_record.c has the rule and the reason --
 *         and the caller must not count it, because `nn run` waits on the
 *         inference counter and then reads the record.
 */
static int nncam_publish_raw(uint32_t gen)
{
	/*
	 * [!] THE OUTPUT SHAPES OF THE MODEL THAT RAN, TAKEN HERE (issue #121).
	 * This thread holds the session, so the model cannot change under it; the
	 * console that prints the report later holds nothing of the kind, and
	 * used to read the shapes of whatever model was open by then.  Built
	 * outside the lock; only the copy into the record is under it.  Static,
	 * not a local: only this thread builds it, and 300 B is not something
	 * nn_work's stack should carry for the one path that needs it.
	 */
	static struct nn_raw_outputs raw;
	int took, i, n;

	n = nn_output_count(nncam_model);
	raw.count = n;
	raw.n = 0u;
	for (i = 0; i < n && i < NN_RAW_OUTPUTS_MAX; i++) {
		struct nn_tensor *t = nn_output(nncam_model, i);

		if (t == NULL)
			break;
		nn_desc_of(&raw.out[i], t);
		raw.n = (uint8_t)(i + 1);
	}

	if (tx_mutex_get(&nncam_det_lock, TX_WAIT_FOREVER) != TX_SUCCESS)
		return 0;
	took = nn_det_record_publish_raw(&nncam_rec, gen, &raw);
	(void)tx_mutex_put(&nncam_det_lock);
	return took;
}

#if defined(CONFIG_NN_BACKEND_TFLM)
/*
 * The same, for a decode whose RESULT STAYED WITH THE PLUGIN (issue #110).
 *
 * What travels is the count and the generation rule; the boxes and the shared
 * decoder's diagnostics do not, because they describe a decoder that did not
 * run.
 *
 * [!] CALLED WITH THE RESULT LEASE STILL HELD.  Releasing it between the decode
 * and this would let the panel see the new private state paired with the old
 * record -- a count from the previous frame beside a picture from this one,
 * which is exactly the pairing the lock in here exists to prevent.
 */
static int nncam_publish_plugin(int n, uint32_t gen)
{
	int took;

	if (tx_mutex_get(&nncam_det_lock, TX_WAIT_FOREVER) != TX_SUCCESS)
		return 0;
	took = nn_det_record_publish_external(&nncam_rec, n, gen);
	(void)tx_mutex_put(&nncam_det_lock);
	return took;
}
#endif

/* Start, re-arm or end a session: move the generation under the lock, so an
 * in-flight decode from the previous one lands nowhere.  [!] THE RESULT STAYS
 * (issue #118): it is still the last thing this model produced, and `nn dets`
 * after a `nn run` or a stopped stream reads it. */
static void nncam_record_boundary(void)
{
#if defined(CONFIG_NN_BACKEND_TFLM)
	/*
	 * [!] UNDER THE RESULT LEASE WHEN IT CAN BE HAD (issue #118).  The worker
	 * asks "is my generation still current?" and then decodes and publishes,
	 * all under the lease -- so a boundary taken under it too lands either
	 * before the question (no decode runs, the plugin's result still describes
	 * the record) or after the publish (the frame is the last result).  Never
	 * in between, which is what left a stopped stream's last result with no
	 * account: wio infers for ~410 ms, so a stop almost always lands inside
	 * one, and the decode that followed rewrote the plugin's result for a
	 * publish that was then dropped.
	 *
	 * The wait is bounded by one decode.  If it expires the boundary is taken
	 * anyway -- a stop does not wait on a plugin -- and the record's own
	 * reportable rule covers the decode that may then run: the count stays,
	 * the account is withheld.  Lease before the record lock, as everywhere.
	 */
	int leased = plugin_lease_take();
#endif

	if (tx_mutex_get(&nncam_det_lock, TX_WAIT_FOREVER) == TX_SUCCESS) {
		nn_det_record_boundary(&nncam_rec);
		(void)tx_mutex_put(&nncam_det_lock);
	}
#if defined(CONFIG_NN_BACKEND_TFLM)
	if (leased)
		plugin_lease_give();
#endif
}

#if defined(CONFIG_NN_BACKEND_TFLM)
/* Is a decode armed at @p gen still going to be published?  Asked by the worker
 * UNDER THE LEASE, before it lets a plugin rewrite its private result -- see
 * nncam_record_boundary() and nn_det_record_admits(). */
static int nncam_admits(uint32_t gen)
{
	int ok;

	if (tx_mutex_get(&nncam_det_lock, TX_WAIT_FOREVER) != TX_SUCCESS)
		return 0;
	ok = nn_det_record_admits(&nncam_rec, gen);
	(void)tx_mutex_put(&nncam_det_lock);
	return ok;
}
#endif

/* See nn_camera.h. */
void nn_camera_record_invalidate(void)
{
	/* Before the first start the lock does not exist, and neither does a
	 * result: nothing has ever published into the record. */
	if (!nncam_created)
		return;
	if (tx_mutex_get(&nncam_det_lock, TX_WAIT_FOREVER) != TX_SUCCESS)
		return;
	nn_det_record_invalidate(&nncam_rec);
	(void)tx_mutex_put(&nncam_det_lock);
}

/* The generation the worker remembers when it ARMS -- see nn_det_record.h for
 * why sampling it after the wait would leave the re-arm boundary open. */
static uint32_t nncam_gen_now(void)
{
	uint32_t g;

	if (tx_mutex_get(&nncam_det_lock, TX_WAIT_FOREVER) != TX_SUCCESS)
		return 0u;
	g = nn_det_record_gen(&nncam_rec);
	(void)tx_mutex_put(&nncam_det_lock);
	return g;
}

/* See nn_camera.h. */
void nn_camera_note_depth_at(enum nn_camera_site site, uintptr_t sp)
{
	TX_THREAD *t = tx_thread_identify();
	uintptr_t  lo, hi;
	uint32_t   used;
	uint32_t  *hw = (site == NNCAM_SITE_DRAW)  ? &nncam_depth_draw :
	                (site == NNCAM_SITE_SHELL) ? &nncam_depth_shell :
	                                             &nncam_depth_decode;

	if (t == NULL)
		return;                     /* not on a thread; nothing to say */
	lo = (uintptr_t)t->tx_thread_stack_start;
	hi = lo + (uintptr_t)t->tx_thread_stack_size;
	if (sp < lo || sp > hi)
		return;                     /* not this thread's stack after all */
	used = (uint32_t)(hi - sp);
	if (used > *hw)
		*hw = used;
}

/* ---- the frame path's hooks (svc/nn_core_frame.h, issue #130) ------------- */

static unsigned nncam_cs_enter(void)
{
	TX_INTERRUPT_SAVE_AREA

	TX_DISABLE
	return (unsigned)interrupt_save;
}

static void nncam_cs_exit(unsigned posture)
{
	TX_INTERRUPT_SAVE_AREA

	interrupt_save = (UINT)posture;
	TX_RESTORE
}

/* The producer has finished writing the last band: wake the worker.  [!] After
 * the hand-over, never before -- the post can only be observed once the
 * producer has given up the input (owhinata/wio-lite-ai#54). */
static void nncam_infer_start(void *ctx)
{
	(void)ctx;
	(void)tx_semaphore_put(&nncam_frame_sem);
}

static int nncam_is_plugin(void *ctx)
{
	(void)ctx;
#if defined(CONFIG_NN_BACKEND_TFLM)
	return nn_active_is_plugin();
#else
	return 0;   /* the null backend carries no plugin mechanism */
#endif
}

static int nncam_publish_raw_hook(void *ctx, uint32_t gen)
{
	(void)ctx;
	return nncam_publish_raw(gen);
}

#if defined(CONFIG_NN_BACKEND_TFLM)
static int nncam_admits_hook(void *ctx, uint32_t gen)
{
	(void)ctx;
	return nncam_admits(gen);
}

/* Where a plugin's decode() is called (issue #110).  Its depth is sampled
 * inside, at the indirect call (svc/nn_active_core.c, issue #126). */
static int nncam_decode(void *ctx, int *n)
{
	int nd = nn_active_decode(nncam_model);

	(void)ctx;
	/* [!] NN_ACTIVE_NOT_HELD: not reachable while the worker's take stands --
	 * and if it ever does not, the decode did not run, so nothing is
	 * published (issue #130, as on Grove).  Counted by the entry check
	 * (plugin_lease_unheld()), not as a worker error: the decoder was never
	 * asked. */
	if (nd == NN_ACTIVE_NOT_HELD)
		return -1;
	*n = nd;
	return 0;
}

static int nncam_publish_hook(void *ctx, int n, uint32_t gen)
{
	(void)ctx;
	return nncam_publish_plugin(n, gen);
}
#endif

/*
 * This board's counting.  A publish the record took is an inference, whatever
 * the decoder said; one the generation rule dropped is not counted, and neither
 * is its decode.  A decode the plugin refused is counted apart by
 * nn_decode_count_of() (issue #130 step 6c, P8: grove-vision-ai-v2's table) --
 * there is no console on this thread, so a refusal not counted apart cannot be
 * told apart afterwards (issue #97).
 */
static void nncam_account(void *ctx, enum nn_core_done what, int n, int took)
{
	(void)ctx;
	if (!took || (what != NN_CORE_DONE_RAW && what != NN_CORE_DONE_DECODED))
		return;
	nncam_infers++;
	if (what != NN_CORE_DONE_DECODED)
		return;   /* no plugin: nothing decoded, nothing refused */
	switch (nn_decode_count_of(n, nncam_oneshot)) {
	case NN_DC_MODEL:
		nncam_model_errors++;
		nncam_errors++;
		break;
	case NN_DC_DECODER:
		nncam_decoder_errors++;
		nncam_errors++;
		break;
	case NN_DC_NONE:
	default:
		break;
	}
}

static const struct nn_core_frame_ops nncam_frame_ops = {
	.cs_enter          = nncam_cs_enter,
	.cs_exit           = nncam_cs_exit,
	.present           = NULL,   /* the preview is its own band client */
	.prep              = nncam_prep,
	.infer_start       = nncam_infer_start,
	.is_plugin         = nncam_is_plugin,
	.publish_raw       = nncam_publish_raw_hook,
	.outputs           = NULL,   /* nn_active_decode() reads them itself */
	.account           = nncam_account,
#if defined(CONFIG_NN_BACKEND_TFLM)
	.admits            = nncam_admits_hook,
	.decode            = nncam_decode,
	.publish           = nncam_publish_hook,
	.lease_try         = plugin_lease_try,
	.lease_held        = plugin_lease_held,
	.lease_give        = plugin_lease_give,
	.lease_note_unheld = plugin_lease_note_unheld,
#endif
};

/* ----------------------------------------------------------- band ingest ------ */

/*
 * One band, on the camera's producer thread, fanned out by app/cam_band.c after the
 * preview has had it.  The fill itself is the shared frame path's (issue #130): it
 * begins only at band 0 while the worker wants a frame, writes each band through
 * nncam_prep() while FILLING, and at band 3 hands the frame over and posts.
 */
static void nncam_band(unsigned band, const uint16_t *px, unsigned rows)
{
	struct nncam_part part;

	/* The band tiling is derived from CAMERA_BAND_ROWS, and the driver's
	   contract is that every band is exactly that tall (port/camera/camera.h).  If
	   that ever stops being true the rows would be sampled from the wrong source
	   offsets and the image would simply be wrong -- so count it instead, and
	   drop the frame being filled (the producer's own ABANDON). */
	if (rows != CAMERA_BAND_ROWS) {
		(void)nn_core_frame_abandon(&nncam_frame, &nncam_frame_ops);
		nncam_errors++;
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
		nncam_errors++;
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

/* ----------------------------------------------------------- worker loop ------ */

static void nncam_step(int first)
{
	uint32_t gen;
	unsigned drained = 0u;

	/*
	 * [!] DISCARD ANY POST THAT PREDATES THIS ARM, AND DO IT BEFORE ARMING.
	 *
	 * Before issue #130 a stale post was enough to break the ownership
	 * invariant FOREVER: the wait returned at once, the inference started with
	 * want_frame already set, the next band 0 began filling underneath it, and
	 * the state sustained itself.  The shared word closes that by
	 * construction -- a wake-up runs nothing unless it can TAKE a frame handed
	 * over, and nothing can begin a fill while one is RUNNING -- so the drain
	 * is now about WHICH frame runs, and it keeps the answer it always gave.
	 *
	 * The window that produces the stale post is real and routine: this wait times out
	 * after NNCAM_FRAME_WAIT_TICKS (100 ms) while a fill can legitimately take up to
	 * two frame periods (~148 ms) from arming, so the producer's post and the timeout
	 * can land together.  A frame whose post is drained here was handed over under
	 * the PREVIOUS arm, and it is thrown away with its post: discarding it costs
	 * one frame, and this arm samples its own generation below.  The first step of
	 * a session drops a frame left over from the last one the same way -- its
	 * post went with the start's own drain.
	 */
	while (tx_semaphore_get(&nncam_frame_sem, TX_NO_WAIT) == TX_SUCCESS) {
		nncam_stale_posts++;
		drained++;
	}
	if (first || drained != 0u)
		(void)nn_core_frame_discard(&nncam_frame, &nncam_frame_ops);

	/*
	 * [!] THE GENERATION IS SAVED HERE, at the arm, and not after the wait.
	 * Saving it once the semaphore returns would leave the re-arm boundary open:
	 * a stop and a fresh start could both happen while this thread sits in the
	 * wait, and the value read afterwards would be the NEW session's -- so the
	 * old frame would publish into it looking current.  Hence this worker ends
	 * every job parked (DONE_LAST) and arms itself here, after the sample.
	 */
	gen = nncam_gen_now();
	(void)nn_core_frame_want(&nncam_frame, &nncam_frame_ops);
	if (tx_semaphore_get(&nncam_frame_sem, NNCAM_FRAME_WAIT_TICKS) != TX_SUCCESS) {
		/* No frame within the bound.  The band flow may have ended without ever
		   posting -- a DCMI overrun, or the stream stopped underneath us -- in
		   which case there is nothing left to wait for.  cam_band_stream_lost()
		   is evaluated lazily by whoever asks, and this is one of the askers.
		   Drop the fill with it (JOIN): the stream may have died part way
		   through a frame, and leaving it FILLING would let a re-armed stream
		   resume that frame from whichever band arrives first.  No producer
		   exists while the stream is lost, so this is one place it can safely
		   be dropped.  Otherwise a fill in flight is NOT taken away: the next
		   step waits for it again. */
		if (cam_band_stream_lost())
			(void)nn_core_frame_join(&nncam_frame, &nncam_frame_ops);
		return;
	}
	if (!nncam_run)
		return;
	/* [!] A wake-up is not a job.  Only a hand-over this thread can TAKE is
	 * one; a post whose frame was already discarded above is stale. */
	if (!nn_core_frame_take(&nncam_frame, &nncam_frame_ops)) {
		nncam_stale_posts++;
		return;
	}

	/* RUNNING: while the word says so, NOTHING may write the input tensor. */
	if (nn_run(nncam_model) != 0) {
		nncam_errors++;
		nn_core_frame_done(&nncam_frame, &nncam_frame_ops, 0);
		return;
	}
	nncam_infer_cyc = nn_last_cycles(nncam_model);

#if defined(CONFIG_NN_BACKEND_TFLM)
	/*
	 * [!] THE LEASE SPANS THE DECODE AND THE PUBLISH, and it is taken
	 * BEFORE the slot is looked up.  The panel runs at a higher priority
	 * than this thread and nothing else separates them, so every part of
	 * "ask the plugin, then say what it answered" has to be one
	 * transaction: a panel that preempted between them would draw from
	 * state this call is halfway through writing, or pair a new count with
	 * an old picture.
	 *
	 * A wait, not a try: this thread is the one that has work to do, and
	 * the only other holders are a console command and a draw that does
	 * not wait.  The bound keeps a wedged holder from taking the worker
	 * with it.  The frame path checks the hold and gives it back; with no
	 * plugin there is nothing to hold.
	 */
	if (nn_active_is_plugin() && !plugin_lease_take()) {
		nncam_errors++;
		nn_core_frame_done(&nncam_frame, &nncam_frame_ops, 0);
		return;
	}
#endif
	/*
	 * The generation question, the decode and the publish under the lease --
	 * or, with no plugin, the publish of an inference nothing decoded (issue
	 * #116), which `nn run` waits on as much as on a decode.  Then this
	 * board's count and the word back to IDLE: the next step arms again.
	 */
	nn_core_on_infer_done(&nncam_frame, &nncam_frame_ops, NULL, gen, 0);
}

static void nncam_entry(ULONG arg)
{
	int first;

	(void)arg;
	for (;;) {
		if (tx_semaphore_get(&nncam_start_sem, TX_WAIT_FOREVER) != TX_SUCCESS)
			continue;

		/* Set BEFORE the loop: while this is set the worker may be touching the
		   tensors, and nn_camera_stop() must not conclude otherwise.  A stop that
		   lands in the window between here and the test below simply finds the
		   flag clear, and the loop it is racing never executes a single step. */
		nncam_worker_busy = 1;
		for (first = 1; nncam_run; first = 0)
			nncam_step(first);
		nncam_worker_busy = 0;

		/* If a stop gave up waiting for us it left the guards held on purpose --
		   see nn_camera_stop().  We are now the last one out, so we release. */
		nncam_guards_give();
	}
}

/* ------------------------------------------------------------------- API ------ */

/*
 * Creation is serialized by the NN session, which nn_camera_start() takes before
 * calling this -- so no separate latch is needed even with two consoles.
 *
 * Unwound on partial failure rather than left half-created: these are static control
 * blocks, so a retry would re-create an object ThreadX already knows about, which is
 * a different (and much more confusing) failure than the one that got us here.
 */
static int nncam_create_objects(void)
{
	if (nncam_created)
		return NNCAM_OK;

	if (tx_semaphore_create(&nncam_start_sem, "nncam_st", 0) != TX_SUCCESS)
		return NNCAM_ERR_INIT;
	if (tx_semaphore_create(&nncam_frame_sem, "nncam_fr", 0) != TX_SUCCESS)
		goto del_start;
	if (tx_mutex_create(&nncam_det_lock, "nncam_dt", TX_INHERIT) != TX_SUCCESS)
		goto del_frame;
	if (tx_thread_create(&nncam_thread, "nn_work", nncam_entry, 0,
	                     nn_worker_stack, sizeof nn_worker_stack,
	                     NNCAM_PRIO, NNCAM_PRIO,
	                     TX_NO_TIME_SLICE, TX_AUTO_START) != TX_SUCCESS)
		goto del_mutex;

	nncam_created = 1;
	return NNCAM_OK;

del_mutex:
	(void)tx_mutex_delete(&nncam_det_lock);
del_frame:
	(void)tx_semaphore_delete(&nncam_frame_sem);
del_start:
	(void)tx_semaphore_delete(&nncam_start_sem);
	LOG_ERR("worker objects could not be created");
	return NNCAM_ERR_INIT;
}

int nn_camera_running(void) { return nncam_run; }

int nn_camera_start(int colorbar, int require_draw)
{
	struct nn_model *m = NULL;
	struct nn_tensor *in;
	int rc;

	if (nncam_run) {
		/*
		 * Already running -- unless the stream died underneath us, in which case
		 * THIS IS THE DOCUMENTED RE-ARM.  Everything except the stream is still
		 * intact: we hold the session and the OCTOSPI1 guard, the worker is alive
		 * in its bounded wait, and the NN claim was never dropped.  So the re-arm
		 * is exactly one call, and it must NOT re-take the guards.
		 *
		 * Refusing here instead would make `nn stream stats`' own advice ("re-issue
		 * `nn stream start` to re-arm") wrong, which is worse than having no
		 * recovery hint at all.
		 *
		 * [!] ONLY A STREAM'S SESSION IS RE-ARMED, AND ONLY BY A STREAM START
		 * (issue #120).  This path does not ask the draw question -- a
		 * legitimate re-arm needs no answer, because the session that admitted
		 * the stream is still held and the decoder cannot have been replaced
		 * since it was asked.  A `nn run`'s session never asked it at all, so a
		 * `nn stream start` that re-armed one would light a panel nothing had
		 * agreed to draw on.  The lifecycle already refuses that start (a
		 * one-shot is never re-armed, svc/nn_stream_life.c); this is the same
		 * rule stated where the worker is, so the two cannot drift apart.
		 */
		if (!cam_band_stream_lost() || !nncam_rearmable || !require_draw)
			return NNCAM_ERR_RUNNING;

		/* Drop the fill before the stream comes back (JOIN, issue #130).  A
		   stream that died mid-frame leaves it FILLING, and without dropping it
		   the first re-armed band would resume a frame that began before the
		   outage -- the first inference would then run over a tensor half of
		   which is stale.  The worker's timeout path joins too; doing it here as
		   well is what makes the result independent of which of the two ran
		   first (the re-arm can easily arrive inside the worker's 100 ms window,
		   or while it is mid-inference -- a frame handed over or running is not
		   taken away, and its publish lands in the old generation).  Safe from
		   here: cam_band_stream_lost() being true means there is no producer
		   right now. */
		(void)nn_core_frame_join(&nncam_frame, &nncam_frame_ops);

		/*
		 * [!] AND THE DECODE RECORD CROSSES A BOUNDARY TOO (issue #99).  A
		 * re-arm is a NEW generation as far as `nn stream` is concerned, and
		 * this is what makes the worker agree: it advances the record
		 * generation, so an inference that was still in flight when the stream
		 * died cannot publish across the boundary and be counted as this
		 * generation's work.  The last result stays (issue #118); the new
		 * generation does not claim it, because what it produced is counted
		 * from a base its commit takes after this call.
		 *
		 * BEFORE the band is re-claimed, so the producer never resumes while the
		 * old generation is still the current one.
		 */
		nncam_record_boundary();

		if (cam_band_claim(CAM_BAND_NN, colorbar, nncam_band) != CAM_BAND_OK) {
			/* cam_band_claim() unwound our claim, but nncam_run and the guards
			   are still ours -- and with no claim left the lost latch cannot
			   re-arm itself, so a second `nn stream start` would just report
			   "already running".  Say what actually clears it instead of leaving
			   the user to discover a state with no obvious way out. */
			return NNCAM_ERR_REARM;
		}
		return NNCAM_OK;
	}
	if (nn_model_open(&m) != 0)
		return NNCAM_ERR_MODEL;

	/* Software claim first, hardware claim second -- taking the peripheral first
	   would mean holding it just to report that software was busy (the order
	   cmd_nn.c documents and phase 2a recorded). */
	if (nn_session_try_acquire() != 0)
		return NNCAM_ERR_SESSION;

	in = nn_input(m, 0);
	if (nn_input_count(m) < 1 || in == NULL || in->data == NULL ||
	    in->ndim != 4 || in->dims[0] != 1) {
		nn_session_release();
		return NNCAM_ERR_MODEL;
	}
	if (in->dtype != NN_DTYPE_FLOAT32 && in->dtype != NN_DTYPE_INT8) {
		nn_session_release();
		return NNCAM_ERR_GEOM;
	}
	/*
	 * [!] The quantization parameters have to be USABLE, not merely present (#51).
	 * nn_tensor carries ONE scale, and the backend fills it from
	 * TfLiteTensor::params.scale -- which a PER-AXIS quantized tensor leaves at zero,
	 * keeping its real parameters in the affine-quantization struct this API does not
	 * expose.  So a zero here does not mean "unit scale", it means "the number you
	 * need is somewhere you cannot see", and quantizing with any assumed constant
	 * would feed the model wrong pixels with nothing to show for it.  Refuse instead;
	 * nncam_rows() then needs no per-pixel fallback (see its comment).
	 */
	if (in->dtype == NN_DTYPE_INT8 && !(in->scale > 0.0f)) {
		nn_session_release();
		return NNCAM_ERR_QUANT;
	}
	nncam_oh = in->dims[1];
	nncam_ow = in->dims[2];
	nncam_oc = in->dims[3];
	/*
	 * [!] THE CHANNEL COUNT IS CHECKED BECAUSE nncam_rows() STOPS AT THREE (issue #57).
	 * It writes `c < oc && c < 3` channels from an RGB565 pixel, so a four-channel
	 * input would leave channel 3 of every pixel holding whatever the arena last had --
	 * no fault, no overrun, just one plane of stale activations fed to the model as if
	 * it were image data.  That is the failure mode this file already refuses a
	 * per-axis scale over: silently wrong pixels with nothing to show for it.  1 (the
	 * red channel alone) and 3 (RGB) are what the sampler can actually produce.
	 *
	 * The tiling then has to cover the input exactly, or some output rows would never
	 * be written (and would then be inferred from whatever the arena last held).
	 */
	if ((nncam_oc != 1u && nncam_oc != 3u) ||
	    nncam_oh == 0u || nncam_ow == 0u ||
	    nncam_oy_bound(0u, nncam_oh) != 0u ||
	    nncam_oy_bound(CAMERA_BANDS_PER_FRAME, nncam_oh) != nncam_oh ||
	    (uint32_t)nncam_ow * nncam_oh * nncam_oc *
	            (in->dtype == NN_DTYPE_FLOAT32 ? 4u : 1u) > in->bytes) {
		nn_session_release();
		return NNCAM_ERR_GEOM;
	}

	if (!psram_ready() || !psram_acquire_shared()) {
		nn_session_release();
		return NNCAM_ERR_PSRAM;
	}
	nncam_holds_guards = 1;

#if defined(CONFIG_NN_BACKEND_TFLM)
	/*
	 * [!] ASK THE DECODER BEFORE LIGHTING A CAMERA (issue #110).  This is the
	 * one admission point both `nn run` and `nn stream start` pass through, and
	 * everything above it has checked the INPUT tensor -- whether the bands
	 * tile onto it, whether its quantisation is usable.  Nothing has asked
	 * whether anything can read the OUTPUTS.
	 *
	 * A plugin was shipped WITH the model it reads, so a plugin that cannot
	 * read this one is a container whose two halves do not belong together,
	 * and finding that out per frame is a stream that runs and silently never
	 * annotates.
	 *
	 * [!] WITH NO PLUGIN THE SHAPE QUESTION PASSES AND THE DRAW QUESTION DOES
	 * NOT (issue #116).  Nothing reads the outputs at all now, so no shape can
	 * be wrong -- and this is the admission `nn run` shares, which on a bare
	 * model runs the inference and reports the tensors themselves.  What is
	 * refused is the PANEL: `require_draw` is set only by `nn stream start`,
	 * and a live overlay with nothing to draw is the failure this pair of
	 * questions exists to catch.
	 */
	{
		/* Under the lease like every other entry into a plugin: the NN session
		 * is held here so no load can replace it, but a console's param_set
		 * takes no session and would otherwise run concurrently with this.
		 *
		 * [!] AND A TIMEOUT IS NOT A YES.  Both questions below decide whether
		 * to light a camera; answering either from an unheld plugin is the
		 * shape the review caught in the stream's old pre-check. */
		int shapes, draws;

		if (!plugin_lease_take()) {
			nncam_guards_give();
			return NNCAM_ERR_DECBUSY;
		}
		shapes = nn_active_shapes_ok(m);
		draws  = require_draw ? nn_active_can_draw() : 1;
		plugin_lease_give();

		/* [!] Compared against 1 (issue #130): NN_ACTIVE_NOT_HELD is
		 * negative, so `!answer` would read it as yes.  Not reachable while
		 * the take above stands; if it ever is, nothing was asked, and that
		 * is the same answer as a lease that could not be had. */
		if (shapes == NN_ACTIVE_NOT_HELD || draws == NN_ACTIVE_NOT_HELD) {
			nncam_guards_give();
			return NNCAM_ERR_DECBUSY;
		}
		if (shapes != 1) {
			nncam_guards_give();
			return NNCAM_ERR_SHAPES;
		}
		if (draws != 1) {
			nncam_guards_give();
			return NNCAM_ERR_NODRAW;
		}
	}
#else
	/*
	 * [!] THE SAME ANSWER WITHOUT THE MACHINERY (issue #116).  The `null`
	 * backend has no plugin mechanism compiled in at all, so there is nothing
	 * that could ever decode here -- which makes the answer to "will anything
	 * annotate this stream" a constant no.  Saying it here rather than
	 * leaving the block out is the point: a refusal that exists only inside
	 * the TFLM branch is a build where `nn stream start` lights a camera and
	 * a panel for a stream that can never draw.
	 */
	if (require_draw) {
		nncam_guards_give();
		return NNCAM_ERR_NODRAW;
	}
#endif

	rc = nncam_create_objects();
	if (rc != NNCAM_OK) {
		nncam_guards_give();
		return rc;
	}

	nncam_model       = m;
	nncam_infers      = 0u;
	nncam_frames      = 0u;
	nncam_skipped     = 0u;
	nncam_errors      = 0u;
	nncam_model_errors   = 0u;
	nncam_decoder_errors = 0u;
	nncam_raced       = 0u;
	nncam_stale_posts = 0u;
	nncam_ingest_last = 0u;
	nncam_ingest_max  = 0u;
	nncam_infer_cyc   = 0u;
	/* Per start, like every counter here: measuring one model and then another
	 * would otherwise report the first one's high-water for both. */
	nncam_depth_decode = 0u;
	nncam_depth_draw   = 0u;
	/* [!] depth_shell is NOT reset here.  Its deepest site is `nn model load`,
	 * which happens before any stream and would otherwise be forgotten by the
	 * first start -- the one number a per-start reset would always erase. */
#if defined(CONFIG_NN_BACKEND_TFLM) && BSP_ENABLE_LCD
	/* Armed here and not reset on stop, so `nn stream stats` right after a
	 * stop still describes the run that just ended (issue #110). */
	cam_preview_plugin_draw_arm();
#endif
	nncam_start_tick  = HAL_GetTick();
	/*
	 * The worker wants nothing until its first step arms it (issue #130): a
	 * word left WANT or FILLING by the last session is joined here, as HEAD
	 * cleared want_frame and filling at this same point -- no more and no
	 * less.  A frame it left HANDED is dropped by the worker's first step.
	 *
	 * [!] THIS DOES NOT PROVE THE PRODUCER IS GONE.  The session lock says
	 * the last session's worker left its loop; it does not say its band was
	 * drained.  A stop whose band release failed (TEARING) leaves the guards
	 * to the worker, which gives them back on its way out whatever the band
	 * did -- so a callback that never returned may still be in flight here.
	 * That hazard is older than this word and is not closed by it (a
	 * separate issue); the JOIN only keeps the behaviour HEAD had.
	 */
	(void)nn_core_frame_join(&nncam_frame, &nncam_frame_ops);
	nn_core_frame_reset(&nncam_frame, &nncam_frame_ops, 1);
	nncam_record_boundary();
	while (tx_semaphore_get(&nncam_frame_sem, TX_NO_WAIT) == TX_SUCCESS)
		;

	nncam_rearmable = require_draw ? 1 : 0;   /* see the re-arm above */
	nncam_oneshot   = require_draw ? 0 : 1;   /* `nn run` is the one without */
	nncam_run = 1;
	rc = cam_band_claim(CAM_BAND_NN, colorbar, nncam_band);
	if (rc != CAM_BAND_OK) {
		nncam_run = 0;
		nncam_guards_give();
		return NNCAM_ERR_BAND;
	}
	(void)tx_semaphore_put(&nncam_start_sem);
	return NNCAM_OK;
}

int nn_camera_stop(void)
{
	int rc_band = CAM_BAND_OK;
	int i;

	if (!nncam_run && !nncam_holds_guards)
		return NNCAM_ERR_NOTRUN;

	nncam_run = 0;
	/*
	 * [!] MOVE THE GENERATION BEFORE WAITING FOR THE WORKER.  The wait below is
	 * bounded and the worker may be most of an inference away from noticing, so a
	 * decode that started under the old session can still complete after this
	 * returns.  Bumping here means its publish is dropped rather than replacing
	 * the stopped session's last result with a frame nobody asked for -- and
	 * the record then marks that the plugin has moved on (issue #118).
	 */
	nncam_record_boundary();
	/* Poke the worker out of its bounded wait so it notices immediately rather
	   than after the remainder of a 100 ms timeout. */
	(void)tx_semaphore_put(&nncam_frame_sem);

	/*
	 * Producer side first.  cam_band_release() drops the claim and then drains any
	 * callback already in flight -- and the claim test and the in-flight count are
	 * updated together under one PRIMASK section, so a producer that passed the
	 * test but had not yet entered cannot slip through behind us.  Until this
	 * returns OK, something may still be writing the input tensor.
	 *
	 * [!] Called UNCONDITIONALLY, not just when the claim is still set.  A previous
	 * stop that timed out already cleared the claim but left a callback in flight;
	 * skipping the drain on the retry because "we are not claimed any more" would
	 * release the arena to `nn bench` with that callback still able to write the
	 * input tensor -- which is the precise thing the first stop refused to do.
	 * cam_band_release() is idempotent, so calling it again is free.
	 */
	rc_band = cam_band_release(CAM_BAND_NN);
	/* The producer is confirmed out once the drain succeeded: a frame it was
	 * part way through filling will never be finished (JOIN, issue #130).  A
	 * frame handed over or running is the worker's, and is left to it. */
	if (rc_band == CAM_BAND_OK)
		(void)nn_core_frame_join(&nncam_frame, &nncam_frame_ops);

	/* Consumer side second: the worker may be up to one inference (~373 ms) away
	   from noticing.  It is below us in priority, so sleeping is what lets it run. */
	for (i = 0; nncam_worker_busy && i < NNCAM_STOP_TICKS; i++)
		tx_thread_sleep(1);

	if (rc_band != CAM_BAND_OK || nncam_worker_busy) {
		/* [!] Deliberately do NOT release the session or the OCTOSPI1 guard.  See
		   nn_camera.h: handing the arena to `nn bench` while a dead-but-not-
		   returned callback can still write the input tensor is unrecoverable,
		   whereas holding a session nobody can use is not.  The worker releases
		   on its way out, or a second `nn stream stop` completes it. */
		LOG_WRN("stop: still tearing down (band %d, worker busy %d)",
		        rc_band, nncam_worker_busy);
		return NNCAM_ERR_TEARING;
	}

	nncam_guards_give();
	return NNCAM_OK;
}

void nn_camera_stats_get(struct nn_camera_stats *out)
{
	uint32_t now;

	if (out == NULL)
		return;
	memset(out, 0, sizeof(*out));

	out->running      = (uint8_t)(nncam_run != 0);
	out->holds_guards = (uint8_t)(nncam_holds_guards != 0);
	out->stream_lost  = (uint8_t)(cam_band_stream_lost() != 0);
	out->norm_signed  = (uint8_t)(nncam_norm_signed != 0);
	out->overlay      = (uint8_t)(nncam_overlay != 0);
	out->infers       = nncam_infers;
	out->frames       = nncam_frames;
	out->skipped      = nncam_skipped;
	out->errors       = nncam_errors;
	out->model_errors   = nncam_model_errors;
	out->decoder_errors = nncam_decoder_errors;
	out->raced        = nncam_raced;
	out->stale_posts  = nncam_stale_posts;
	out->ingest_last_cyc = nncam_ingest_last;
	out->ingest_max_cyc  = nncam_ingest_max;
	out->infer_last_cyc  = nncam_infer_cyc;

	now = HAL_GetTick();
	out->elapsed_ms = nncam_start_tick ? (now - nncam_start_tick) : 0u;

	/* Read directly rather than under nncam_det_lock: a single int cannot tear on
	   this core, and taking the mutex here would make this function unusable before
	   the first start() has created it (`nn stream stats` on a cold boot). */
	/* Read without nncam_det_lock: a single int cannot tear on this core, and
	   taking the mutex here would make this function unusable before the first
	   start() has created it (`nn stream stats` on a cold boot). */
	out->ndet = nncam_rec.ndet;
	out->depth_decode = nncam_depth_decode;
	out->depth_draw   = nncam_depth_draw;
	out->depth_shell  = nncam_depth_shell;
}

int nn_camera_decode_get(struct nn_camera_decode *out,
                         struct nn_report_capture *rep,
                         struct nn_result_extra *ext)
{
	struct nn_det_snapshot snap;
#if defined(CONFIG_NN_BACKEND_TFLM)
	int leased = 0;
#endif

	if (out == NULL || !nncam_created)
		return 0;

#if defined(CONFIG_NN_BACKEND_TFLM)
	/*
	 * [!] THE LEASE FIRST, AND ONLY FOR A CAPTURE.  A plugin's account of its
	 * result has to be taken in the same breath as the snapshot that describes
	 * it -- the next decode overwrites the private state -- so both happen
	 * inside one hold.  The order is lease then record lock, the same way the
	 * worker takes them, and the panel asks for no capture precisely so it
	 * never waits here.
	 */
	if (rep != NULL && nn_active_is_plugin()) {
		leased = plugin_lease_take();
		if (!leased) {
			/* The result exists; nobody let go of it in time.  Saying so is
			 * better than printing a count with no account beside it and no
			 * sign that one was meant to be there. */
			nn_report_set(rep, NN_REPORT_STALE);
		}
	}
#endif

	if (tx_mutex_get(&nncam_det_lock, TX_WAIT_FOREVER) != TX_SUCCESS) {
#if defined(CONFIG_NN_BACKEND_TFLM)
		if (leased)
			plugin_lease_give();
#endif
		return 0;
	}
	/*
	 * [!] ONE LOCK FOR BOTH.  The boxes and the diagnostics that describe them
	 * are published together and have to be read together: taking them in two
	 * calls lets a frame land in between, and then a console prints this
	 * frame's boxes beside the next frame's peak score with nothing to show
	 * they disagree (issue #97).
	 *
	 * [!] AND NO BOX ARRAY IS OFFERED (issue #116).  Nothing on this board
	 * publishes into the caller's array any more -- a plugin keeps its result
	 * and a bare model has none -- so passing one would be offering a
	 * destination for boxes that no publisher here can produce.
	 */
	nn_det_record_snapshot(&nncam_rec, &snap, NULL, 0);
	/* In the same hold as the snapshot (issue #121). */
	if (ext != NULL)
		nn_det_record_extra(&nncam_rec, ext);
	(void)tx_mutex_put(&nncam_det_lock);
	out->valid      = snap.valid;
	out->ndet       = snap.ndet;
	out->res        = snap.res;
	out->kind       = snap.kind;
	out->reportable = snap.reportable;
	out->current    = snap.current;
	out->accepted   = snap.accepted;
	out->epoch      = snap.epoch;

#if defined(CONFIG_NN_BACKEND_TFLM)
	if (leased) {
		/* Still under the lease: the snapshot above and this account of it
		 * describe the same decode because nothing has run in between. */
		if (snap.valid && snap.kind == (uint8_t)NN_DET_PLUGIN_REPORT &&
		    !snap.reportable) {
			/* [!] A later decode ran and its publish was dropped (issue
			 * #118): the plugin's result is that frame's now, and asking it
			 * would describe a frame this record does not hold. */
			nn_report_set(rep, NN_REPORT_SUPERSEDED);
		} else if (snap.valid && snap.kind == (uint8_t)NN_DET_PLUGIN_REPORT) {
			int rc;

			nn_report_begin(rep);
			/* [!] Compared against 1 (issue #130): NN_ACTIVE_NOT_HELD is
			 * negative, so `!can_report` would read it as yes.  A refusal
			 * for want of the lease is REFUSED -- the account was not given
			 * -- and never UNSUPPORTED (the plugin has one) or STALE (nobody
			 * else was holding it).  Not reachable from here; counted where
			 * it is refused. */
			rc = nn_active_can_report();
			if (rc == NN_ACTIVE_NOT_HELD)
				nn_report_set(rep, NN_REPORT_REFUSED);
			else if (rc != 1)
				nn_report_set(rep, NN_REPORT_UNSUPPORTED);
			else
				nn_report_end(rep, nn_active_report(nn_report_write, rep));
		} else {
			/* A plugin is loaded but this record is not its work -- nothing
			 * has been decoded yet in this session, say. */
			nn_report_set(rep, NN_REPORT_NONE);
		}
		plugin_lease_give();
	}
#endif
	return 1;
}

int nn_camera_draw(const struct nn_core_panel *p, void *ctx)
{
	return (int)nn_core_draw(&nncam_frame, &nncam_frame_ops, p, ctx);
}

void nn_camera_present_done(void)
{
	nn_core_on_present_done(&nncam_frame, &nncam_frame_ops);
}

void nn_camera_set_norm(int signed_range) { nncam_norm_signed = signed_range ? 1 : 0; }
int  nn_camera_get_norm(void)             { return nncam_norm_signed; }
void nn_camera_set_overlay(int on)        { nncam_overlay = on ? 1 : 0; }
int  nn_camera_get_overlay(void)          { return nncam_overlay; }
