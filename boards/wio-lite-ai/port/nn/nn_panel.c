/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Wio Lite AI ThreadX Shell Project
 */
/**
 * @file    nn_panel.c
 * @brief   The nn half of the preview thread (issue #140; see nn_panel.h for the
 * boundary with src/cam_preview.c and nn_camera_priv.h for the state).
 *
 * Everything here runs on the preview thread (priority 12), which outranks the
 * worker: the may-draw question, the record snapshot (no capture, so it never
 * waits on the lease), the plugin's draw() and its spent / refused figures.
 * The lease is only ever TRIED (svc/nn_core_frame.c, nn_core_draw()).  Built
 * only with the LCD, like the preview itself.
 */
#include "nn_panel.h"
#include "nn_camera.h"       /* nn_camera_decode_get(), nn_camera_get_overlay() */
#include "nn_camera_priv.h"  /* nncam_frame, nncam_frame_ops */

#if defined(CONFIG_NN_BACKEND_TFLM)
#include "nn_active.h"
#include "plugin_lease.h"
#include "plugin_paint.h"
#endif
#include "tx_api.h"

#include <string.h>

#if defined(CONFIG_NN_BACKEND_TFLM)
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
 * The panel's half of the shared frame path (svc/nn_core_frame.h, issue #130).
 * nn_core_draw() tries the lease, then takes the frame lock below, then asks
 * the hooks below whether to paint, then paints, then gives the lease back --
 * the order and the questions this file used to spell out itself.
 *
 * [!] THE LEASE IS TAKEN BEFORE THE FRAME LOCK.  It used to be taken in here,
 * AFTER ltdc_lock_frame() -- the exact inversion of the order this port
 * documents, with the comment beside it asserting the opposite.  A no-wait
 * acquire meant no deadlock could follow, so the code worked and the rule it was
 * written to obey did not exist.  The frame path takes the frame lock through
 * preview_frame_lock() only once the lease is won, and LEAVES IT HELD for the
 * flip.
 *
 * [!] AND A HELD LEASE IS NOT A REASON TO DRAW.  The lease says the plugin's
 * result is not being rewritten; it says nothing about whether there IS one
 * that belongs to now.  Three reachable cases need the record as well:
 * `nn overlay off` (preview_may_draw()), a preview still running after
 * inference stopped, and a decode whose publication the generation check
 * REJECTED -- that one leaves fresh private state in the plugin that no
 * accepted record describes, and drawing it puts a retired session's
 * detections on a live picture.  The frame path checks the record it gets
 * from preview_record(): valid, of THIS session (issue #118), and the
 * plugin's.
 */
static void preview_frame_lock(void *ctx)
{
	const struct nn_panel_display *d = ctx;

	d->frame_lock();
}

static int preview_may_draw(void *ctx)
{
	(void)ctx;
	return nn_camera_get_overlay();
}

/* No capture: this is the panel asking "is there a current result, and is it
 * the plugin's".  A RAW_TENSORS record answers the first and not the second --
 * an inference ran that nothing decoded, and there is nothing to put on the
 * picture (issue #116). */
static int preview_record(void *ctx, struct nn_det_snapshot *snap)
{
	struct nn_camera_decode dec;

	(void)ctx;
	memset(&dec, 0, sizeof dec);
	if (!nn_camera_decode_get(&dec, NULL, NULL))
		return 0;
	snap->valid      = dec.valid;
	snap->ndet       = dec.ndet;
	snap->kind       = dec.kind;
	snap->reportable = dec.reportable;
	snap->current    = dec.current;
	snap->accepted   = dec.accepted;
	snap->epoch      = dec.epoch;
	return 1;
}

static void preview_paint(void *ctx)
{
	const struct nn_panel_display *d = ctx;
	struct plugin_painter paint;
	struct plugin_paint_budget bud;
	TX_INTERRUPT_SAVE_AREA

	bud.pixels  = PREVIEW_PLUGIN_DRAW_PIXELS;
	bud.ops     = PREVIEW_PLUGIN_DRAW_OPS;
	bud.refused = 0u;
	/* [!] Under the frame lock: nn_core_draw() calls this only after
	 * preview_frame_lock(), and the back buffer is valid only while it is
	 * held (nn_panel.h). */
	plugin_paint_bind(&paint, &bud, d->back_buffer(),
	                  d->surface_w(), d->surface_h());
	/* The depth is sampled inside, at the indirect call (issue #126).
	 * NN_ACTIVE_NOT_HELD paints nothing and is counted by the entry check
	 * itself (issue #130); there is nothing more to do with it here. */
	(void)nn_active_draw(&paint);

	TX_DISABLE
	if (PREVIEW_PLUGIN_DRAW_PIXELS - bud.pixels > preview_draw_spent)
		preview_draw_spent = PREVIEW_PLUGIN_DRAW_PIXELS - bud.pixels;
	preview_draw_refused += bud.refused;
	TX_RESTORE
}

static const struct nn_core_panel preview_panel = {
	.frame_lock = preview_frame_lock,
	.may_draw   = preview_may_draw,
	.record     = preview_record,
	.paint      = preview_paint,
};

int nn_panel_draw(const struct nn_panel_display *d)
{
	return nn_camera_draw(&preview_panel, (void *)d);
}

#endif /* CONFIG_NN_BACKEND_TFLM */

int nn_camera_draw(const struct nn_core_panel *p, void *ctx)
{
	return (int)nn_core_draw(&nncam_frame, &nncam_frame_ops, p, ctx);
}

void nn_camera_present_done(void)
{
	nn_core_on_present_done(&nncam_frame, &nncam_frame_ops);
}
