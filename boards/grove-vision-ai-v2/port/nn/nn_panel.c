/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    nn_panel.c
 * @brief   The panel thread's half of the camera -> inference glue (issue
 *          #140): the overlay's draw(), called by cam_lcd_sink.c inside the
 *          panel guard, the record snapshot and the plugin's paint.
 *
 * [!] THE PLUGIN LEASE IS ONLY TRIED HERE, NEVER WAITED FOR (nn_core_draw()):
 * the order is panel guard, then a non-blocking lease try, and no lease holder
 * waits for the panel guard, a camera API mutex or a pipeline lock.  The
 * shared state is nn_camera_priv.h's, defined in nn_camera.c.
 */
#include "nn_camera.h"
#include "nn_camera_priv.h"

#include <stddef.h>

#include "tx_api.h"

#include "nn_active.h"
#include "plugin_paint.h"
#include "nn_rec.h"
#include "nn_core_frame.h" /* the shared frame path (issue #130) */

/*
 * What one plugin draw() may spend, and why these numbers.
 *
 * The panel guard is held for the whole of draw(), and everything else that
 * wants the panel is failing its non-blocking acquire meanwhile, so the work the
 * base does on a plugin's behalf is capped at a quarter of a frame.
 *
 * [!] THE JUSTIFICATION USED TO QUOTE 775 us FOR THE STAGED BLIT, AND THAT
 * NUMBER IS SIX MONTHS STALE (issue #105).  The staging loops were compiled -O3
 * when issue #42 removed the MVE ban that had forced -Os on them, and `held`
 * dropped to 197 us -- the board README carries the before/after table.  The
 * cap did not need changing, but the reasoning behind it was being read off a
 * measurement that no longer described the machine, and a plan for this issue
 * copied 775 into a hardware acceptance criterion before anyone noticed: it
 * would have passed a threefold regression as "no change".  Take a fresh
 * baseline; do not transcribe a constant out of a comment.
 *
 * A full frame is 320 x 240 = 76,800 pixels, so a quarter of a frame is 19,200:
 * enough for a label bar (320 x 16 = 5,120), and enough for every box
 * BF_MAX_DET allows now that an outline is charged for the pixels it writes
 * rather than the area it encloses (issue #105 -- before that, one 200x200 box
 * was refused on its own).
 *
 * [!] A CAP THAT CANNOT BE EXCEEDED IS NOT A CAP.  The stack allowances of this
 * issue were first written at the size of the whole thread stack, which made
 * the check unable to fire for the case it existed to catch.  This one is
 * deliberately below what a plugin might plausibly want, and what a draw
 * actually spends is reported by `nn stream stats` so it can be judged rather
 * than argued about.
 */
#define NN_OV_DRAW_PIXELS  (320u * 240u / 4u)
#define NN_OV_DRAW_OPS     64u

/* ---- the panel ------------------------------------------------------------- */

/* The picture draw() was handed. */
struct nn_ov_canvas {
	uint16_t *fb;
	uint16_t  w, h;
};

static int nn_ov_record(void *ctx, struct nn_det_snapshot *snap)
{
	(void)ctx;
	nn_rec_snapshot(snap, NULL);
	return 1;
}

static void nn_ov_paint(void *ctx)
{
	TX_INTERRUPT_SAVE_AREA
	const struct nn_ov_canvas *cv = ctx;
	struct plugin_painter paint;
	struct plugin_paint_budget bud;
	uint32_t spent;

	bud.pixels  = NN_OV_DRAW_PIXELS;
	bud.ops     = NN_OV_DRAW_OPS;
	bud.refused = 0u;
	plugin_paint_bind(&paint, &bud, cv->fb, cv->w, cv->h);
	/* NN_ACTIVE_NOT_HELD paints nothing and is counted by the entry check
	 * itself; there is nothing more to do with it here. */
	(void)nn_active_draw(&paint);

	/*
	 * What it actually spent, so the cap can be judged against something
	 * rather than defended in the abstract.
	 *
	 * [!] BOTH IN ONE CRITICAL SECTION (issue #105).  A console reading
	 * these two is asking one question -- how close did a frame come to
	 * the cap, and did anything get refused -- and a preemption between
	 * the two stores answers it with this frame's spend beside the
	 * previous count.  The reader disabling interrupts cannot undo that,
	 * so the writer has to be atomic as well.  It is a handful of
	 * instructions on the panel thread, off the pixel path.
	 */
	spent = NN_OV_DRAW_PIXELS - bud.pixels;
	TX_DISABLE
	if (spent > nn_ov_draw_spent)
		nn_ov_draw_spent = spent;
	nn_ov_draw_refused += bud.refused;
	TX_RESTORE
}

static const struct nn_core_panel nn_ov_panel = {
	.frame_lock = NULL,   /* the panel guard is the caller's, held already */
	.may_draw   = NULL,
	.record     = nn_ov_record,
	.paint      = nn_ov_paint,
};

void nn_camera_draw(void *ctx, uint16_t *fb, uint16_t fb_w,
                    uint16_t fb_h)
{
	struct nn_ov_canvas cv;

	(void)ctx;

	/*
	 * The plugin paints its own result (issue #103), because the firmware does
	 * not know what shape that result has -- which is the whole point of
	 * issue #78.
	 *
	 * [!] AND THERE IS NO OTHER BRANCH SINCE ISSUE #104.  A resident path used
	 * to follow this one, mapping bf_det boxes through nn_preproc_box() and
	 * drawing them here.  With no decoder in the firmware nothing can reach it:
	 * nn_detector_ready() refuses to start a stream unless a plugin is loaded
	 * AND draws, so by the time the panel thread is calling this, both are true.
	 *
	 * [!] THE PLUGIN LEASE, TRIED ONCE INSIDE THE PANEL GUARD (issue #127),
	 * by the shared frame path (issue #130).  Nothing a console or the worker
	 * does can be inside this plugin while it paints.  A refusal shows the
	 * frame without an overlay and is counted as this frame's miss -- the only
	 * one it can have, since the producer no longer asks (issue #129).
	 *
	 * [!] THE ORDER IS PANEL GUARD, THEN LEASE -- wio's is the other way
	 * round (lease, then frame lock), because here draw() is called from
	 * inside the guard and there is no earlier point to ask.  It cannot
	 * close a cycle because this thread only TRIES: it never waits while
	 * holding the guard.  What would close one is a lease holder that
	 * waits for the panel guard, a camera API mutex or a pipeline lock;
	 * no holder does, and none touches the LCD.  Released before the
	 * callback returns, and nothing else is waited for in between.
	 */
	cv.fb = fb;
	cv.w  = fb_w;
	cv.h  = fb_h;
	(void)nn_core_draw(&nn_ov_core, &nn_ov_ops, &nn_ov_panel, &cv);
	/* The lag of what was painted (issue #129), counted now: the staged
	 * frame is this draw's, and nothing later on this board says more. */
	nn_core_on_present_done(&nn_ov_core, &nn_ov_ops);
}
