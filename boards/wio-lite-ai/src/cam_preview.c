/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Wio Lite AI ThreadX Shell Project
 */
/**
 * @file    cam_preview.c
 * @brief   Live camera preview on the LCD (owhinata/wio-lite-ai#8 phase 3c,
 * owhinata/wio-lite-ai#35).
 *
 * The pipeline, end to end:
 *
 *     DCMI -> DMA2_Stream1 (double buffer, two FIXED 60-row bands in AXI-SRAM)
 *       -> camera producer thread: invalidate, then transpose the band into the
 *          LTDC back buffer (PSRAM)
 *       -> preview thread: ltdc_flip() once the fourth band is in
 *
 * WHY BANDS.  The camera is 320x240 landscape and the panel 240x320 portrait, and
 * nothing on this board can rotate for free: no GFXMMU, no GPU2D, and the panel's
 * own MADCTL/MV and RAMCTRL/RM do not apply to RGB-interface pixels (the issue
 * owhinata/wio-lite-ai#35/#38 spike; port/ltdc/st7789_rgb.c).  So every displayed
 * frame is transposed in software, and a transpose reads one side with a stride.
 * owhinata/wio-lite-ai#38 shipped the honest slow version -- whole frames pinned out
 * of the PSRAM ring -- and it measured 25.0 ms of CPU per frame, 35% of a core, plus
 * ~10 DCMI FIFO errors per 30 s from the DCMI and the LTDC both wanting OCTOSPI1.
 * Staging the DCMI in AXI-SRAM instead puts the strided reads on internal RAM and
 * takes the camera off that bus altogether, which is why both numbers move together.
 *
 * WHY THE FLIP IS ON ITS OWN THREAD.  ltdc_flip() waits for vertical blanking --
 * up to ~16 ms.  The band transposes run on the CAMERA's producer thread, and
 * whatever runs there delays the next band, which has to be consumed inside one
 * band period (~18.5 ms at ~13.5 fps -- the derivation is on cam_band_service()
 * in port/camera/camera.c).  Spending 16 of those 18 on a blanking wait once a
 * frame would trade nearly the whole margin for nothing.  The producer therefore
 * asks for a flip and returns, and this thread waits.
 *
 * WHICH MAKES THE HANDSHAKE THE INTERESTING PART.  ltdc_flip() swaps front and
 * back, so a flip that lands while the producer is part way through the next
 * frame would split that frame across both buffers.  The producer will not begin a
 * frame while a flip it asked for is still outstanding -- it drops the frame
 * instead, counted as `dropped`.  At ~13.5 fps the flip has ~74 ms to complete, so
 * this is a guard rather than a regular event.
 *
 * WHAT IS NOT DEFENDED, on purpose: the display lock is taken per band rather than
 * held across the whole frame, so an `lcd` command from the other console can draw
 * between two bands.  Holding it across a frame would mean holding it ~95% of the
 * time at 13.5 fps -- any contention would then land squarely on the band deadline
 * and cost real pixels, whereas an interleaved draw costs one cosmetically mixed
 * frame that the next one replaces.  A drawing `lcd` command holds the lock for a
 * DMA2D fill plus a blanking wait, which is close enough to one band period that
 * `band late` / `band torn` can tick; `lcd reset` holds it for ~0.3 s and will.
 * Both recover on the next frame, which is what those counters are for.
 */
#include "cam_preview.h"

#include "cam_band.h"    /* issue #9 P3: the band stream is shared with the NN now */
#include "camera.h"
#include "ltdc_display.h"
#include "nn_camera.h"
#if defined(CONFIG_NN_BACKEND_TFLM)
#include "nn_active.h"
#include "plugin_lease.h"
#include "plugin_paint.h"
#endif   /* issue #9 P4: the face boxes this thread draws */
#include "stm32h7xx.h"   /* DWT->CYCCNT: time the rotating blit */
#include "tx_api.h"

#define LOG_TAG "campv"
#include "log.h"
#include "mem_sections.h"  /* DTCM_BSS: CPU-only data out of AXI-SRAM (issue #46) */

/* Below the camera producer (10) -- a late flip costs a frame of display, a late
   producer costs a band.  Above the shell (16) so a busy console cannot stall the
   display. */
#define PREVIEW_PRIO   12u
#define PREVIEW_STACK  CAM_PREVIEW_STACK_BYTES   /* cam_preview.h (#108) */

/* The camera frame and the landscape drawing surface are the same size, so the
 * preview is a full-surface blit at the origin -- no crop, no offsets, and each
 * band is a full-width strip of it. */
_Static_assert(CAMERA_FRAME_WIDTH == 320u && CAMERA_FRAME_HEIGHT == 240u,
               "preview assumes the camera matches the 320x240 landscape surface");

static TX_THREAD    preview_thread;
static UCHAR        preview_stack[PREVIEW_STACK] DTCM_BSS __attribute__((aligned(8)));
static TX_SEMAPHORE preview_flip_sem;

static volatile int preview_on;
/* Set by the producer thread when it has handed over a finished back buffer,
   cleared by the preview thread once the flip is done (or has failed).  One
   writer each, and the producer only ever tests it. */
static volatile int preview_flip_pending;
/* Producer-thread only: we are part way through a frame we intend to present. */
static int          preview_in_frame;

static uint32_t     preview_shown;
static uint32_t     preview_dropped;
/* Cycles this frame's four band transposes have taken so far, and the completed
   total from the most recent frame (DWT, 550 MHz).  The total is the figure of
   merit for owhinata/wio-lite-ai#35 -- 25.0 ms was the whole-frame-from-PSRAM number
   it had to beat. */
static uint32_t     preview_blit_acc;
static uint32_t     preview_blit_cyc;

/*
 * One band, on the camera's producer thread.
 *
 * @px points into the AXI-SRAM band the DMA just finished; the driver has already
 * invalidated it and guarantees bands arrive 0,1,2,3 with no gaps inside a frame,
 * so the only frame-level decision left here is whether to start one.
 *
 * Since owhinata/wio-lite-ai#9 phase 3 this is a cam_band client rather than the
 * camera's one registered callback -- app/cam_band.c fans out to it first and to the
 * NN ingest second.  The frame-level decision below stays entirely ours: a frame this
 * thread skips because a flip is still pending is NOT a frame the NN skips.
 */
static void preview_band(unsigned band, const uint16_t *px, unsigned rows)
{
	uint32_t t0;

	if (!preview_on)
		return;

	if (band == 0u) {
		if (preview_flip_pending) {
			/* The previous frame is still on its way to the panel.  Drawing now
			   would land in a buffer that is about to become the front one --
			   skip the whole frame rather than tear it across the swap. */
			preview_in_frame = 0;
			preview_dropped++;
			return;
		}
		preview_in_frame = 1;
		preview_blit_acc = 0u;
	}
	if (!preview_in_frame)
		return;                   /* mid-frame remainder of a frame we skipped */

	/* The band IS the surface here: full width, at its own landscape row offset.
	   ltdc_blit() takes the display lock, clips, and transposes (svc/gfx_rot). */
	t0 = DWT->CYCCNT;
	ltdc_blit(px, 0, (uint16_t)(band * rows),
	          (uint16_t)CAMERA_FRAME_WIDTH, (uint16_t)rows);
	preview_blit_acc += DWT->CYCCNT - t0;

	if (band + 1u >= CAMERA_BANDS_PER_FRAME) {
		preview_in_frame = 0;
		preview_blit_cyc = preview_blit_acc;
		/* Order matters: the flag is what the next band 0 tests, so it has to be
		   set before the thread that clears it can possibly run. */
		preview_flip_pending = 1;
		(void)tx_semaphore_put(&preview_flip_sem);
	}
}

/*
 * One detection box, in landscape surface coordinates (owhinata/wio-lite-ai#9 phase
 * 4).
 *
 * The boxes are normalized to the model's square input, which the downsample maps
 * onto the WHOLE 320x240 frame (it squashes rather than crops), and the landscape
 * surface is that same 320x240 -- so the mapping is a straight multiply.  A 90
 * degree rotation takes an axis-aligned rectangle to an axis-aligned rectangle, so
 * four thin ltdc_fill_rect() calls need no new drawing primitive: each one clips and
 * transposes itself.
 *
 * [!] THE CLAMP IS IN SIGNED ARITHMETIC, BEFORE THE uint16_t CAST.  The decoder
 * computes x = cx - w/2, which is routinely negative for a face at the edge of the
 * frame, and surf_clip() only clips the FAR edges -- it takes uint16_t and assumes
 * non-negative input.  A negative value cast to uint16_t becomes ~65535 and sails
 * straight through the clip.
 */
#define PREVIEW_BOX_RGB565 0x07E0u   /* green: no red bits at all (see issue #43) */
#define PREVIEW_BOX_THICK  2u

static void preview_box(const struct bf_det *d)
{
	/* The LTDC_SURFACE_* macros expand to constants private to ltdc_display.c;
	   the accessors are the public way to ask, and they answer the same. */
	const int sw = (int)ltdc_surface_w();
	const int sh = (int)ltdc_surface_h();
	int x0, y0, x1, y1, w, h, t;

	x0 = (int)(d->x * (float)sw);
	y0 = (int)(d->y * (float)sh);
	x1 = (int)((d->x + d->w) * (float)sw);
	y1 = (int)((d->y + d->h) * (float)sh);

	if (x0 < 0)
		x0 = 0;
	if (y0 < 0)
		y0 = 0;
	if (x1 > sw)
		x1 = sw;
	if (y1 > sh)
		y1 = sh;
	if (x1 <= x0 || y1 <= y0)
		return;                   /* entirely off the surface */

	w = x1 - x0;
	h = y1 - y0;
	t = (int)PREVIEW_BOX_THICK;
	if (t > w)
		t = w;                    /* a box thinner than the stroke becomes solid */
	if (t > h)
		t = h;

	ltdc_fill_rect((uint16_t)x0, (uint16_t)y0, (uint16_t)w, (uint16_t)t,
	               PREVIEW_BOX_RGB565);
	ltdc_fill_rect((uint16_t)x0, (uint16_t)(y1 - t), (uint16_t)w, (uint16_t)t,
	               PREVIEW_BOX_RGB565);
	ltdc_fill_rect((uint16_t)x0, (uint16_t)y0, (uint16_t)t, (uint16_t)h,
	               PREVIEW_BOX_RGB565);
	ltdc_fill_rect((uint16_t)(x1 - t), (uint16_t)y0, (uint16_t)t, (uint16_t)h,
	               PREVIEW_BOX_RGB565);
}

/*
 * Drawn HERE, on the flip thread, and not in the band callback: this thread has
 * ~74 ms of slack per frame while the band callback has ~18.5 ms, and every
 * microsecond spent there costs real pixels.  The boxes go into the back buffer
 * after the fourth band and before the flip, and the next frame's bands overwrite
 * the whole surface, so nothing has to erase them.
 *
 * If `band late` ever climbs with the overlay on, the lever is to cap the number of
 * boxes drawn (the top few by score are the ones worth seeing) -- NOT a frame-wide
 * display lock, which cannot be written at all: a TX_MUTEX may only be released by
 * the thread that took it, and the bands and the flip are on different threads.
 */
/*
 * Static rather than a local, and that is a stack decision, not a style one: this
 * thread's stack is 1,024 B and BF_MAX_DET detections are 160 B of it, on top of a
 * call chain (ltdc_fill_rect -> fb_fill_rect -> ltdc_dma2d_fill) that did not exist
 * on this thread before -- preview_entry() used to do nothing but ltdc_flip().  Float
 * arithmetic here also means an interrupt now stacks the extended VFP frame (the
 * ThreadX M7 port adds s16-s31 on top of the hardware's s0-s15 + FPSCR), which is a
 * further ~136 B whenever this thread is preempted.  Safe as a static because
 * preview_entry() is its only caller and there is exactly one of it.
 */
static struct bf_det preview_dets[BF_MAX_DET];

static void preview_draw_overlay(void)
{
	int n, i;

	if (!nn_camera_get_overlay())
		return;
	n = nn_camera_dets_get(preview_dets, BF_MAX_DET);
	for (i = 0; i < n; i++)
		preview_box(&preview_dets[i]);
}

#if defined(CONFIG_NN_BACKEND_TFLM)
/*
 * The surface this panel draws on and the frame nn_active maps boxes into are
 * the same rectangle, stated in two places because port/nn may not include
 * port/ltdc.  This file sees both -- at RUN TIME, because the panel's own
 * dimensions are private to ltdc_display.c and reachable only through its
 * accessors, which is a deliberate property of that driver and not one to
 * unpick for an assertion.  Checked once at init, where a mismatch is a line
 * on the console rather than a plugin quietly drawing in the wrong place.
 */
static void preview_check_plugin_frame(void)
{
	if (ltdc_surface_w() != (uint16_t)NN_ACTIVE_FRAME_W ||
	    ltdc_surface_h() != (uint16_t)NN_ACTIVE_FRAME_H)
		LOG_ERR("the plugin's frame (%ux%u) is not the drawing surface "
		        "(%ux%u); boxes will land in the wrong place",
		        (unsigned)NN_ACTIVE_FRAME_W, (unsigned)NN_ACTIVE_FRAME_H,
		        (unsigned)ltdc_surface_w(), (unsigned)ltdc_surface_h());
}

/*
 * What one plugin draw() may spend.
 *
 * A quarter of the surface, which is where the other board started, taken here
 * as an INITIAL TEST CEILING rather than a shipping one: what the budget bounds
 * is time spent with the frame lock held, and on this board those pixels are
 * CPU stores into non-cacheable external PSRAM.  The board README carries the
 * measurement and the acceptance criteria; if they are not met this comes down.
 */
#define PREVIEW_PLUGIN_DRAW_PIXELS  (NN_ACTIVE_FRAME_W * NN_ACTIVE_FRAME_H / 4u)
#define PREVIEW_PLUGIN_DRAW_OPS     64u

/*
 * [!] WRITER, READER AND ARM UNDER ONE RULE.  The writer below sets two words;
 * a reader that masked interrupts only on its own side could still see the
 * high-water from after one assignment beside the refusals from before the
 * other.  Masking cannot reach backwards, so all three sites mask.
 */
static uint32_t preview_draw_spent;
static uint32_t preview_draw_refused;

void cam_preview_plugin_draw_stats(uint32_t *spent, uint32_t *refused)
{
	TX_INTERRUPT_SAVE_AREA

	TX_DISABLE
	if (spent != NULL)
		*spent = preview_draw_spent;
	if (refused != NULL)
		*refused = preview_draw_refused;
	TX_RESTORE
}

void cam_preview_plugin_draw_arm(void)
{
	TX_INTERRUPT_SAVE_AREA

	TX_DISABLE
	preview_draw_spent   = 0u;
	preview_draw_refused = 0u;
	TX_RESTORE
	plugin_lease_misses_reset();
}

/*
 * Let the loaded plugin paint.
 *
 * [!] THE LEASE IS TAKEN BY THE CALLER, BEFORE THE FRAME LOCK.  It used to be
 * taken in here, which is AFTER ltdc_lock_frame() -- the exact inversion of
 * the order this port documents, with the comment beside it asserting the
 * opposite.  A no-wait acquire meant no deadlock could follow, so the code
 * worked and the rule it was written to obey did not exist.
 *
 * [!] AND A HELD LEASE IS NOT A REASON TO DRAW.  The lease says the plugin's
 * result is not being rewritten; it says nothing about whether there IS one
 * that belongs to now.  Three reachable cases need the record as well:
 * `nn overlay off` (which this path ignored entirely), a preview still running
 * after inference stopped, and a decode whose publication the generation check
 * REJECTED -- that one leaves fresh private state in the plugin that no
 * accepted record describes, and drawing it puts a retired session's
 * detections on a live picture.
 */
static void preview_draw_plugin(void)
{
	struct plugin_painter paint;
	struct plugin_paint_budget bud;
	struct nn_camera_decode dec;
	TX_INTERRUPT_SAVE_AREA

	if (!nn_camera_get_overlay())
		return;
	/* No capture and no boxes: this is the panel asking "is there a current
	 * result, and is it the plugin's". */
	memset(&dec, 0, sizeof dec);
	if (!nn_camera_decode_get(&dec, NULL, 0, NULL))
		return;
	if (!dec.valid || dec.kind != (uint8_t)NN_DET_PLUGIN_REPORT)
		return;

	bud.pixels  = PREVIEW_PLUGIN_DRAW_PIXELS;
	bud.ops     = PREVIEW_PLUGIN_DRAW_OPS;
	bud.refused = 0u;
	plugin_paint_bind(&paint, &bud, ltdc_back_buffer(),
	                  ltdc_surface_w(), ltdc_surface_h());
	/* [!] THE PROBE SITS AT THE CALL, not in the caller.  Both are the same
	 * frame while this function is inlined, and "while it is inlined" is not
	 * something the number should depend on. */
	nn_camera_note_depth(NNCAM_SITE_DRAW);
	nn_active_draw(&paint);

	TX_DISABLE
	if (PREVIEW_PLUGIN_DRAW_PIXELS - bud.pixels > preview_draw_spent)
		preview_draw_spent = PREVIEW_PLUGIN_DRAW_PIXELS - bud.pixels;
	preview_draw_refused += bud.refused;
	TX_RESTORE
}

#endif /* CONFIG_NN_BACKEND_TFLM */

static void preview_entry(ULONG arg)
{
	(void)arg;
	for (;;) {
		if (tx_semaphore_get(&preview_flip_sem, TX_WAIT_FOREVER) != TX_SUCCESS)
			continue;
		if (preview_on) {
#if defined(CONFIG_NN_BACKEND_TFLM)
			/* [!] THE LEASE COMES BEFORE THE FRAME LOCK (issue #110), because
			   that is the order every other holder uses and an inversion is a
			   deadlock waiting for somebody to make one of these waits
			   blocking.  It is a TRY: this thread outranks the worker, and
			   blocking here would hold a lock wider than what it protects for
			   as long as a decode takes.  A refusal presents the picture
			   unannotated -- the failure this pipeline already has for a
			   process() that declines -- and is counted, because the preview
			   counters below see a frame that was PRESENTED, not one presented
			   bare.
			   [!] And `plug` is sampled once: taking it twice could light the
			   plugin path without the lease, or leak the lease. */
			int plug   = nn_active_is_plugin();
			int leased = plug ? plugin_lease_try() : 0;
#endif
			/* One outer lock around the boxes AND the flip.  ltdc_lock_frame()
			   is recursive, and ltdc_flip() already holds it across its entire
			   VBR wait, so this adds only the fills to the held time while
			   removing up to 32 separate acquisitions from the window between
			   the last band and the flip. */
			ltdc_lock_frame();
			/* Where a plugin's draw() stands (issue #108 placed the probe,
			   #110 put the call beside it): here, inside the frame lock and
			   INSTEAD OF the resident overlay -- not in preview_box(), which
			   is deeper than a plugin is ever called from and would
			   over-report. */
#if defined(CONFIG_NN_BACKEND_TFLM)
			/* One decoder annotates a frame, not two: the plugin's boxes and
			   the resident decoder's would be different readings of different
			   models.  The depth probe moved into preview_draw_plugin(), at
			   the call it describes.
			   [!] The lease is released BEFORE the flip: holding it across the
			   VBR wait would stop the worker decoding while this thread
			   sleeps, for nothing -- the drawing is already done. */
			if (plug) {
				if (leased) {
					preview_draw_plugin();
					plugin_lease_give();
				}
			} else {
				nn_camera_note_depth(NNCAM_SITE_DRAW);
				preview_draw_overlay();
			}
#else
			nn_camera_note_depth(NNCAM_SITE_DRAW);
			preview_draw_overlay();
#endif
			if (ltdc_flip() == LTDC_OK)
				preview_shown++;
			else
				preview_dropped++;
			ltdc_unlock_frame();
		}
		/* Unconditionally, including after a failed flip: leaving it set would
		   stop the producer from ever starting another frame. */
		preview_flip_pending = 0;
	}
}

int cam_preview_init(void)
{
	if (tx_semaphore_create(&preview_flip_sem, "campv_fl", 0) != TX_SUCCESS) {
		LOG_ERR("preview semaphore create failed");
		return -1;
	}
#if defined(CONFIG_NN_BACKEND_TFLM)
	preview_check_plugin_frame();
#endif
	if (tx_thread_create(&preview_thread, "cam_prev", preview_entry, 0,
	                     preview_stack, sizeof preview_stack,
	                     PREVIEW_PRIO, PREVIEW_PRIO,
	                     TX_NO_TIME_SLICE, TX_AUTO_START) != TX_SUCCESS) {
		LOG_ERR("preview thread create failed");
		return -1;
	}
	return 0;
}

int cam_preview_enable(int on, int colorbar)
{
	int rc;

	if (!on) {
		preview_on = 0;
		rc = cam_band_release(CAM_BAND_PREVIEW);
		if (rc == CAM_BAND_ERR_LOCK) {
			/* The claim was never dropped -- the other console held the band API
			   too long -- so the preview really is still on.  Put the flag back
			   rather than leaving `camera info` reporting "off" over a claim that
			   is still in the fan-out set; a retry works. */
			preview_on = 1;
			return -3;
		}
		/* Safe to clear regardless of rc: cam_band_release() has already dropped
		   our claim, so the fan-out no longer reaches preview_band() at all, and
		   on success its drain has also waited out any call still in flight.
		   (Before owhinata/wio-lite-ai#9 phase 3 the equivalent guarantee came from the
		   stream itself having stopped -- but the stream now outlives us when the NN is
		   still claiming it, so the drain is what provides it.) */
		preview_flip_pending = 0;
		preview_in_frame     = 0;
		return (rc == CAM_BAND_OK) ? 0 : -1;
	}

	if (!ltdc_is_up() || ltdc_scanout_off())
		return -1;                 /* nothing would appear; say so instead */
	/* Re-issuing `preview on` used to restart unconditionally so that `preview on
	   test` could switch a running preview to the colour bars -- the test-pattern
	   bit lives in the sensor's COM7 and only a restart rewrites it.  With the
	   stream now shared, a restart is no longer ours alone to do: it would tear the
	   stream out from under a running `nn stream`.  So restart only when the
	   setting actually changes, and refuse when somebody else is on the stream. */
	if (camera_band_streaming() && colorbar != cam_band_colorbar()) {
		if (cam_band_claimed(CAM_BAND_NN))
			return -2;
		preview_on = 0;
		/* [!] The result matters here.  If the release fails, the stream is still
		   the OLD one, and the claim below would quietly JOIN it -- reporting a
		   successful switch while the sensor's test-pattern bit never changed.
		   Fail instead; the caller retries. */
		rc = cam_band_release(CAM_BAND_PREVIEW);
		if (rc != CAM_BAND_OK) {
			if (cam_band_claimed(CAM_BAND_PREVIEW))
				preview_on = 1;   /* still ours; keep the reported state true */
			return (rc == CAM_BAND_ERR_LOCK) ? -3 : -1;
		}
	}

	preview_shown        = 0u;
	preview_dropped      = 0u;
	preview_blit_acc     = 0u;
	preview_blit_cyc     = 0u;
	preview_flip_pending = 0;
	preview_in_frame     = 0;
	while (tx_semaphore_get(&preview_flip_sem, TX_NO_WAIT) == TX_SUCCESS)
		;
	/* Before the claim, not after: the first band can arrive the moment
	   cam_band_claim() returns, and preview_band() drops what it gets while
	   this is clear. */
	preview_on = 1;

	rc = cam_band_claim(CAM_BAND_PREVIEW, colorbar, preview_band);
	if (rc != CAM_BAND_OK) {
		preview_on = 0;
		/* Distinguished from "the camera would not start": nothing is wrong, the
		   other console is simply mid claim/release and a retry will work.  A
		   catch-all here would send someone to `dmesg` for a healthy board. */
		return (rc == CAM_BAND_ERR_LOCK) ? -3 : -1;
	}
	return 0;
}

int cam_preview_enabled(void)
{
	return preview_on;
}

void cam_preview_stats(uint32_t *shown, uint32_t *dropped, uint32_t *blit_us)
{
	if (shown != NULL)
		*shown = preview_shown;
	if (dropped != NULL)
		*dropped = preview_dropped;
	/* DWT counts CPU cycles and the app inherits a 550 MHz core (SystemCoreClock),
	   which is where the divisor comes from -- not a hardcoded 550. */
	if (blit_us != NULL)
		*blit_us = preview_blit_cyc / (SystemCoreClock / 1000000u);
}
