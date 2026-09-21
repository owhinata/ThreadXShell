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
 * [!] THE RESIDENT OVERLAY IS GONE (issue #116 = #78 Step 3c).  This file used
 * to hold a box painter of its own, drawing what the firmware's BlazeFace
 * decoder had published.  There is no such decoder now: the only thing that can
 * annotate a frame is a LOADED PLUGIN, through the one path below.
 *
 * What was deleted with it, and is therefore not a thing to look for: a static
 * box array on this thread's 1,024 B stack budget, and a
 * ltdc_fill_rect -> fb_fill_rect -> ltdc_dma2d_fill chain that only the overlay
 * reached from here.  The plugin path paints on the CPU (see plugin_paint.c),
 * which is a different decision made for a different reason.
 */

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
	/* No capture: this is the panel asking "is there a current result, and is
	 * it the plugin's".  A RAW_TENSORS record answers the first and not the
	 * second -- an inference ran that nothing decoded, and there is nothing to
	 * put on the picture (issue #116). */
	memset(&dec, 0, sizeof dec);
	if (!nn_camera_decode_get(&dec, NULL))
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
			   #110 put the call beside it): here, inside the frame lock.  The
			   depth probe is inside preview_draw_plugin(), at the call it
			   describes.
			   [!] AND THIS IS THE ONLY WAY A FRAME GETS ANNOTATED (issue
			   #116).  With no plugin -- or a container whose plugin was
			   refused -- the picture is presented exactly as the bands built
			   it; there is no second painter to fall back to.
			   [!] The lease is released BEFORE the flip: holding it across the
			   VBR wait would stop the worker decoding while this thread
			   sleeps, for nothing -- the drawing is already done. */
#if defined(CONFIG_NN_BACKEND_TFLM)
			if (plug && leased) {
				preview_draw_plugin();
				plugin_lease_give();
			}
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
