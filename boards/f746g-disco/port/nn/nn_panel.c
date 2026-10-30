/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    nn_panel.c
 * @brief   The nn half of the GUIX camera preview.  See nn_panel.h.
 *
 * Everything here runs on the GUIX thread (priority 14), which outranks the
 * inference worker: the may-draw question, the record snapshot (no capture, so
 * it never waits on the lease), the plugin's draw() and its figures.  The lease
 * is only ever TRIED (svc/nn/nn_core_frame.c, nn_core_draw()).  tflm only.
 */
#include "nn_panel.h"

#include "nn_active.h"
#include "nn_camera.h"       /* nn_camera_decode_get(), nn_camera_overlay() */
#include "nn_camera_priv.h"  /* nncam_frame, nncam_frame_ops */
#include "plugin_lease.h"
#include "plugin_paint.h"

#include "stm32f7xx_hal.h"   /* DWT->CYCCNT, SystemCoreClock: the hold time */
#include "tx_api.h"

#include <string.h>

/*
 * What one plugin draw() may spend.
 *
 * [!] PROVISIONAL, AND NOT wio-lite-ai's NUMBER (plan #159 D6).  What the budget
 * bounds is how long the GUIX thread holds the frame lock, and the producer
 * needs that same lock to store the next frame into the view buffer -- so the
 * figure that decides it is how long the producer can wait on the lock before
 * the DCMI ring runs out of free slots, measured on this board (the README's
 * "draw budget" section has the procedure).  Until that measurement is taken an
 * eighth of the frame is the ceiling: CPU stores into non-cacheable SDRAM, a
 * few tens of microseconds for a full-budget draw at QVGA, against frame
 * periods of tens of milliseconds.  `nn stream stats` prints the longest hold
 * a plugin draw caused, which is the number the acceptance compares.
 */
#define NN_PANEL_DRAW_DIV   8u
#define NN_PANEL_DRAW_OPS   64u

uint32_t nn_panel_budget(uint16_t w, uint16_t h)
{
	return (uint32_t)w * (uint32_t)h / NN_PANEL_DRAW_DIV;
}

/*
 * [!] WRITER, READER AND ARM UNDER ONE RULE.  The writer below sets several
 * words; a reader that masked interrupts only on its own side could still see
 * one from after an update beside another from before it.  All three sites
 * mask.
 */
static uint32_t panel_spent, panel_refused, panel_hold_cyc;

/* When this draw's frame lock was taken: the GUIX thread's only. */
static uint32_t panel_lock_at;

void nn_panel_stats(uint32_t *spent, uint32_t *refused, uint32_t *hold_us)
{
	uint32_t mhz = SystemCoreClock / 1000000u;
	uint32_t hold;
	TX_INTERRUPT_SAVE_AREA

	TX_DISABLE
	if (spent != NULL)
		*spent = panel_spent;
	if (refused != NULL)
		*refused = panel_refused;
	hold = panel_hold_cyc;
	TX_RESTORE
	if (hold_us != NULL)
		*hold_us = mhz != 0u ? hold / mhz : 0u;
}

void nn_panel_arm(void)
{
	TX_INTERRUPT_SAVE_AREA

	TX_DISABLE
	panel_spent    = 0u;
	panel_refused  = 0u;
	panel_hold_cyc = 0u;
	TX_RESTORE
	plugin_lease_misses_reset();
}

/*
 * The frame lock, AFTER the lease: nn_core_draw() calls this only once it won
 * the lease (the order this board's plugin_lease.h states), and returns with it
 * held whenever it did.
 */
static void panel_frame_lock(void *ctx)
{
	const struct nn_panel_display *d = ctx;

	d->frame_lock();
	panel_lock_at = DWT->CYCCNT;
}

/* The overlay is DERIVED (issue #142): drawn while an `nn stream` runs, not for
 * a `nn run`. */
static int panel_may_draw(void *ctx)
{
	(void)ctx;
	return nn_camera_overlay();
}

/* No capture: the panel asks "is there a current result, and is it the
 * plugin's" -- the shared draw checks valid, current and the kind. */
static int panel_record(void *ctx, struct nn_det_snapshot *snap)
{
	struct nn_camera_decode dec;

	(void)ctx;
	memset(&dec, 0, sizeof dec);
	if (!nn_camera_decode_get(&dec, NULL, 0, NULL, NULL))
		return 0;
	snap->valid      = dec.valid;
	snap->ndet       = dec.ndet;
	snap->res        = dec.res;
	snap->kind       = dec.kind;
	snap->reportable = dec.reportable;
	snap->current    = dec.current;
	snap->accepted   = dec.accepted;
	snap->epoch      = dec.epoch;
	return 1;
}

static void panel_paint(void *ctx)
{
	const struct nn_panel_display *d = ctx;
	struct plugin_painter paint;
	struct plugin_paint_budget bud;
	uint16_t w = 0u, h = 0u;
	uint16_t *fb;
	uint32_t full;
	TX_INTERRUPT_SAVE_AREA

	/* [!] Under the frame lock: the view buffer is valid only while it is
	 * held.  No preview armed, nothing to paint on. */
	fb = d->view(&w, &h);
	if (fb == NULL || w == 0u || h == 0u)
		return;
	full = nn_panel_budget(w, h);
	bud.pixels  = full;
	bud.ops     = NN_PANEL_DRAW_OPS;
	bud.refused = 0u;
	plugin_paint_bind(&paint, &bud, fb, w, h);
	/* The depth is sampled inside, at the indirect call.  NN_ACTIVE_NOT_HELD
	 * paints nothing and is counted by the entry check itself. */
	(void)nn_active_draw(&paint);
	if (bud.pixels != full)
		d->touched();               /* the strokes ride the view buffer */

	TX_DISABLE
	if (full - bud.pixels > panel_spent)
		panel_spent = full - bud.pixels;
	panel_refused += bud.refused;
	TX_RESTORE
}

static const struct nn_core_panel panel = {
	.frame_lock = panel_frame_lock,
	.may_draw   = panel_may_draw,
	.record     = panel_record,
	.paint      = panel_paint,
};

enum nn_core_draw nn_panel_draw(const struct nn_panel_display *d)
{
	enum nn_core_draw r;

	if (d == NULL)
		return NN_CORE_DRAW_NONE;
	/*
	 * [!] NO STREAM, NO TRY (issue #159 Step 4b review).  The preview keeps
	 * running after `nn stream stop`, and nn_core_draw() tries the lease before
	 * it asks may_draw(); every one of those tries would be counted as a miss
	 * -- for a frame nobody meant to annotate -- and the count only goes back
	 * to zero at the next start.  Asked here first, without a lock (the
	 * overlay is two words the start and the stop write); the same question
	 * under the frame lock (panel_may_draw) stays, for a stop landing in
	 * between.  DECLINED, not NONE: NONE tells the caller to draw the resident
	 * decoder's boxes, and with no stream there are none to draw either.
	 */
	if (!nn_camera_overlay())
		return NN_CORE_DRAW_DECLINED;
	r = nn_core_draw(&nncam_frame, &nncam_frame_ops, &panel, (void *)d);
	if (r == NN_CORE_DRAW_DECLINED || r == NN_CORE_DRAW_PAINTED) {
		uint32_t held = DWT->CYCCNT - panel_lock_at;
		TX_INTERRUPT_SAVE_AREA

		d->frame_unlock();
		if (r == NN_CORE_DRAW_PAINTED) {
			TX_DISABLE
			if (held > panel_hold_cyc)
				panel_hold_cyc = held;
			TX_RESTORE
			/* The painted frame is out (svc/nn/nn_core_frame.h). */
			nn_core_on_present_done(&nncam_frame, &nncam_frame_ops);
		}
	}
	return r;
}
