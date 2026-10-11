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
 * frame that the next one replaces.
 *
 * [!] AND THE PRODUCER NEVER WAITS FOR IT (issue #142).  The lock is TRIED per
 * band (ltdc_blit_try()); a band that finds it held drops the rest of that frame
 * from the display -- counted as `dropped` -- and the band still goes on to the
 * NN step.  A drawing `lcd` command holds the lock for a DMA2D fill plus a
 * blanking wait, close to a band period, and waiting for it used to tick
 * `band late` / `band torn` for the NN too.
 *
 * WHEN IT SHOWS ANYTHING (issue #142).  There is no `camera preview on` any
 * more: the preview is what you see while the camera streams AND the display is
 * on.  Its step of the band sink is armed for the board's whole life and decides
 * at each frame's first band from the display's state (ltdc_display_on()).
 */
#include "cam_preview.h"

#include "cam_band.h"    /* issue #9 P3: the band stream is shared with the NN now */
#include "camera.h"
#include "ltdc_display.h"
#include "nn_panel.h"     /* issue #140: the nn half of this thread */
#if defined(CONFIG_NN_BACKEND_TFLM)
#include "nn_active.h"   /* NN_ACTIVE_FRAME_W / _H, checked against the surface */
#endif
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
 * This is the PREVIEW step of the camera's band sink (port/camera/cam_band.c):
 * it runs first, the NN ingest second.  The frame-level decision below stays
 * entirely ours: a frame this thread skips -- the display off, a flip still
 * pending, the frame lock busy -- is NOT a frame the NN skips.
 */
static void preview_band(unsigned band, const uint16_t *px, unsigned rows)
{
	uint32_t t0;

	if (band == 0u) {
		/* Displayed only while the display is on (issue #142): nothing to
		   transpose for a dark panel, and no flip to ask for.  Not counted --
		   this is the preview being off, not a frame lost. */
		if (!ltdc_display_on()) {
			preview_in_frame = 0;
			return;
		}
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
	   ltdc_blit_try() TRIES the display lock, clips, and transposes (svc/gfx_rot).
	   [!] A lock somebody else holds drops the rest of this frame from the
	   display rather than waiting: the producer has one band period, and the
	   NN step comes after this one. */
	t0 = DWT->CYCCNT;
	if (!ltdc_blit_try(px, 0, (uint16_t)(band * rows),
	                   (uint16_t)CAMERA_FRAME_WIDTH, (uint16_t)rows)) {
		preview_in_frame = 0;
		preview_dropped++;
		return;
	}
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
 * [!] THE NN HALF OF THIS THREAD IS port/nn/nn_panel.c (issue #140): whether to
 * paint, the record, the plugin's draw() and its spent / refused figures.  What
 * stays here is the display, handed over as the LTDC driver's own functions --
 * port/nn may not include port/ltdc.  The back buffer and the surface size are
 * read only once the frame lock is held (nn_panel.h).
 */
static const struct nn_panel_display preview_display = {
	.frame_lock  = ltdc_lock_frame,
	.back_buffer = ltdc_back_buffer,
	.surface_w   = ltdc_surface_w,
	.surface_h   = ltdc_surface_h,
};

#endif /* CONFIG_NN_BACKEND_TFLM */

static void preview_entry(ULONG arg)
{
	(void)arg;
	for (;;) {
		if (tx_semaphore_get(&preview_flip_sem, TX_WAIT_FOREVER) != TX_SUCCESS)
			continue;
		{
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
			   bare.  The shared frame path asks whether a plugin is there once,
			   tries the lease, and only then takes the frame lock (issue #130).

			   Where a plugin's draw() stands (issue #108 placed the probe,
			   #110 put the call beside it): inside the frame lock.  The depth
			   is sampled at the indirect call itself (svc/nn_active_core.c,
			   issue #126).
			   [!] AND THIS IS THE ONLY WAY A FRAME GETS ANNOTATED (issue
			   #116).  With no plugin -- or a container whose plugin was
			   refused -- the picture is presented exactly as the bands built
			   it; there is no second painter to fall back to.
			   [!] The lease is released BEFORE the flip, inside the frame
			   path: holding it across the VBR wait would stop the worker
			   decoding while this thread sleeps, for nothing -- the drawing is
			   already done. */
			int drew = nn_panel_draw(&preview_display);

			/* One outer lock around the boxes AND the flip.  ltdc_lock_frame()
			   is recursive, and ltdc_flip() already holds it across its entire
			   VBR wait, so this adds only the fills to the held time while
			   removing up to 32 separate acquisitions from the window between
			   the last band and the flip.  The frame path left it held if it
			   won the lease; otherwise it is taken here. */
			if (drew != (int)NN_CORE_DRAW_DECLINED &&
			    drew != (int)NN_CORE_DRAW_PAINTED)
				ltdc_lock_frame();
#else
			ltdc_lock_frame();
#endif
			if (ltdc_flip() == LTDC_OK)
				preview_shown++;
			else
				preview_dropped++;
			ltdc_unlock_frame();
#if defined(CONFIG_NN_BACKEND_TFLM)
			/* The painted frame is out (svc/nn_core_frame.h). */
			nn_camera_present_done();
#endif
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
	/* The preview's step of the band sink, armed for good (issue #142): it is
	   fed whenever the camera streams, and decides per frame from the display
	   whether to draw.  Nothing has been delivered yet, so it cannot refuse. */
	if (cam_band_step_enable(CAM_BAND_PREVIEW, preview_band) != CAM_BAND_OK) {
		LOG_ERR("preview step enable failed");
		return -1;
	}
	return 0;
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
