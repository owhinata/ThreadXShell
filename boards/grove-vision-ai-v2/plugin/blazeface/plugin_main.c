/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    plugin_main.c
 * @brief   BlazeFace as a loadable plugin (issue #101 = #78 Step 1a).
 *
 * The ABI face of svc/blazeface.c.  The decoder itself is NOT copied here: the
 * same source the firmware links is linked again into this image, which is what
 * keeps issue #97's "one decoder for three boards" true after a second way of
 * shipping it exists.  Everything in this file is the wrapper.
 *
 * [!] STEP 1a NEVER RUNS THIS.  It is built and gated, not loaded: the device
 * validates the container it arrives in and stops there.  It exists now because
 * a gate with nothing to gate proves nothing, and because the numbers Step 1b
 * needs -- the real image size, the real stack depth of each callback -- are
 * measurements of this artifact, not estimates.
 *
 * [!] AND IT OWNS ITS STORAGE, WHICH THE SHARED DECODER MAY NOT.  cmake/
 * check_no_mutable_storage.py forbids svc/blazeface.c from holding a single
 * mutable byte, because each board places the decoder's scratch itself -- wio in
 * .psram_ai, f746 in .sdram.ai, Grove in plain .bss -- and a static in the
 * shared file would land in memory no board's residency gate names.  A plugin is
 * the opposite case: it has exactly one placement, the reservation it is
 * prelinked for, so its state lives here in ordinary .bss and .data.  The
 * plugin's own gate checks that its storage stays inside its declared segments,
 * which is a different question from the shared file's.
 */
#include "plugin_abi.h"
#include "plugin_base.h"
#include "plugin_fmt.h"
#include "plugin_text.h"
#include "blazeface.h"
#include "tensor.h"

#include <stddef.h>
#include <stdint.h>

/* ---- state ---------------------------------------------------------------- */

/*
 * The candidate scratch the decoder evicts into.  BF_MAX_CAND entries is the
 * decoder's own cap; giving it fewer would silently narrow the "best N" the NMS
 * sees, which is the bug issue #47 fixed and not one to reintroduce by being
 * frugal with a buffer that costs a few hundred bytes.
 */
static struct bf_cand   pl_cand[BF_MAX_CAND];
static struct blazeface pl_bf;

/* The last decode, kept for draw() and report(). */
static struct bf_det    pl_det[BF_MAX_DET];
static struct bf_result pl_res;
static int              pl_ndet;

static const struct plugin_base_api *pl_base;

/* Box colour and stroke, matching what the resident overlay draws so that the
 * two paths can be compared on the panel as well as in a test. */
#define PL_RGB565   0x07E0u   /* green */
#define PL_STROKE   2u

/* ---- the score beside each box (issue #105) -------------------------------- */

/*
 * [!] THE SAME RASTERISER THE CLASSIFIER USES, AND THAT IS WHY IT IS SHARED.
 * plugin_text.c was written for the classifier's label; a shared file with one
 * caller is not shared, it is just misfiled.  Putting the score on the box is
 * what makes the escape hatch general -- glyphs into a plugin-owned buffer,
 * spans through blit -- rather than a one-off for one container.
 *
 * SCALE 1, four characters: the milli score is at most "1000", and the chip has
 * to sit inside a face box without covering the face.  Opaque, because a
 * colour-keyed blit is charged for every source pixel it READS anyway, so
 * transparency would buy nothing from the budget and would leave green digits
 * over a bright cheek.
 */
#define PL_LBL_SCALE  1u
#define PL_LBL_CHARS  4u
#define PL_LBL_W      (PL_TEXT_CELL_W * PL_LBL_CHARS * PL_LBL_SCALE)
#define PL_LBL_H      (PL_TEXT_CELL_H * PL_LBL_SCALE)
#define PL_LBL_BG     0x0000u   /* black */

/*
 * One cell per detection, stacked into a single column so a cell is a
 * contiguous run at the atlas stride -- which is exactly what blit wants.
 *
 * The _Static_asserts are the bounds check: blit reads the rectangle's width and
 * height through the stride it is handed and proves nothing about the buffer
 * behind the pointer, and neither does the image gate.
 */
static uint16_t pl_atlas[PL_LBL_W * PL_LBL_H * BF_MAX_DET];
static int      pl_nlbl;

_Static_assert(sizeof pl_atlas / sizeof pl_atlas[0] ==
               PL_LBL_W * PL_LBL_H * BF_MAX_DET,
               "the atlas is not the rectangle its extents describe");
_Static_assert(PL_LBL_W >= PL_TEXT_CELL_W * PL_LBL_CHARS * PL_LBL_SCALE,
               "a cell is narrower than the longest score it must hold");

/* ---- entry ---------------------------------------------------------------- */

/*
 * [!] THE BASE IS CHECKED BEFORE ANY MEMBER IS USED.  version and size come
 * first in the struct precisely so that a base older than this plugin is refused
 * rather than called through a struct that is shorter than this file thinks.
 */
static int pl_entry(const struct plugin_base_api *base)
{
	if (base == NULL)
		return -1;
	if (base->version != PLUGIN_ABI_VERSION)
		return -1;
	if (base->size != (uint32_t)sizeof(struct plugin_base_api))
		return -1;
	if (base->log == NULL || base->to_frame == NULL)
		return -1;

	pl_base = base;
	pl_ndet = 0;
	return blazeface_init(&pl_bf, pl_cand, sizeof pl_cand) == 0 ? 0 : -1;
}

/* ---- the model ------------------------------------------------------------ */

static int pl_shapes_ok(const struct tensor_desc *outs, unsigned n)
{
	return blazeface_shapes_ok(outs, n);
}

/*
 * Decode, and keep the result for the two reporters.
 *
 * [!] THE COUNT IS STORED EVEN WHEN IT IS NEGATIVE.  blazeface_decode() returns
 * a face count or a negative BF_ERR_*, and the three failures are not one thing:
 * BF_ERR_MODEL means "open a different model" while a wiring fault means "look
 * at the firmware".  Folding them into 0 faces -- which one of the three board
 * copies used to do -- turns a question about the model into a picture with no
 * boxes and no explanation.
 */
/** Milli, on the same scale `report` and the threshold use. */
static uint32_t pl_score_milli(const struct bf_det *d)
{
	return (uint32_t)(d->score * 1000.0f);
}

/*
 * Rasterise one chip per detection.
 *
 * On the producer thread with no panel guard held -- the split cam_lcd_sink.h
 * documents, and the reason draw() below can stay a run of blits.
 */
static void pl_stage_labels(void)
{
	struct pl_text_target t;
	int i;

	t.px     = pl_atlas;
	t.w      = PL_LBL_W;
	t.h      = PL_LBL_H * BF_MAX_DET;
	t.stride = PL_LBL_W;

	for (i = 0; i < pl_ndet && i < BF_MAX_DET; i++) {
		struct plugin_printer out;
		struct pl_sbuf sb;
		char line[PL_LBL_CHARS + 1u];
		int32_t y = (int32_t)((uint32_t)i * PL_LBL_H);

		pl_sbuf_init(&sb, line, sizeof line);
		pl_sbuf_printer(&out, &sb);
		(void)pl_fmt_u32(&out, pl_score_milli(&pl_det[i]));

		pl_text_fill(&t, 0, y, (int32_t)PL_LBL_W, (int32_t)PL_LBL_H,
		             PL_LBL_BG);
		(void)pl_text_draw(&t, 0, y, line, PL_LBL_SCALE, PL_RGB565);
		pl_nlbl = i + 1;
	}
}

static int pl_decode(const struct tensor_desc *outs, unsigned n)
{
	/* Before anything can return: a chip must never outlive the decode that
	 * produced it, however this function later grows an early exit. */
	pl_nlbl = 0;

	pl_ndet = blazeface_decode(&pl_bf, outs, n, pl_det, BF_MAX_DET, &pl_res);
	if (pl_ndet > 0)
		pl_stage_labels();
	return pl_ndet;
}

/* ---- the panel ------------------------------------------------------------ */

/*
 * Paint the last decode.
 *
 * Runs on the panel thread with the panel guard held, so it does the least it
 * can: no arithmetic beyond the transform the base supplies, no allocation, no
 * blocking.  Everything expensive already happened in pl_decode() on the
 * producer thread.
 *
 * The boxes come out of the decoder in MODEL INPUT coordinates, and to_frame()
 * is the inverse of the transform the input was built with -- a call rather than
 * four multiplications here because it also rejects the non-finite box a
 * degenerate model can produce, before anything is cast to an integer.
 */
static void pl_draw(const struct plugin_painter *paint)
{
	int i;

	if (paint == NULL || paint->rect == NULL || pl_base == NULL)
		return;
	if (pl_ndet <= 0)
		return;   /* a negative count is a diagnostic, not a box to draw */

	for (i = 0; i < pl_ndet && i < BF_MAX_DET; i++) {
		struct plugin_rect r, lr;

		/* Through the veneer, never the pointer: see plugin_base.c. */
		if (pl_base_to_frame(pl_base, pl_det[i].x, pl_det[i].y,
		                     pl_det[i].w, pl_det[i].h, &r) != 0)
			continue;
		pl_paint_rect(paint, &r, PL_RGB565, PL_STROKE);

		/*
		 * The score chip, inside the box's top-left corner so it never
		 * hides the edge the box is there to show.  The painter clips it,
		 * which is what makes a box running off the frame -- the ordinary
		 * case for a face at the edge -- need no arithmetic here.
		 */
		if (i >= pl_nlbl || paint->blit == NULL)
			continue;
		lr.x0 = r.x0 + (int32_t)PL_STROKE;
		lr.y0 = r.y0 + (int32_t)PL_STROKE;
		lr.x1 = lr.x0 + (int32_t)PL_LBL_W;
		lr.y1 = lr.y0 + (int32_t)PL_LBL_H;
		pl_paint_blit(paint, &lr,
		              pl_atlas + (size_t)i * PL_LBL_W * PL_LBL_H,
		              PL_LBL_W, -1);
	}
}

/* ---- the console ---------------------------------------------------------- */

/*
 * Describe the last decode.
 *
 * [!] EVERY FAILURE PROPAGATES.  A sink that has said no has said no; continuing
 * to write into it would turn one refused line into a run of them, and the
 * caller would see only the last.
 */
static int pl_report(const struct plugin_printer *out)
{
	int i, rc;

	if (out == NULL || out->write == NULL)
		return -1;

	if (pl_ndet < 0) {
		/* The diagnostic IS the result here -- see pl_decode(). */
		rc = pl_fmt_cstr(out, "blazeface: decode refused (");
		if (rc == 0)
			rc = pl_fmt_u32(out, (uint32_t)(-pl_ndet));
		if (rc == 0)
			rc = pl_fmt_cstr(out, ")\r\n");
		return rc;
	}

	/* The header says what the numbers are, because this report and the
	 * resident one do not use the same units -- see the box loop below. */
	rc = pl_fmt_cstr(out, "faces ");
	if (rc == 0)
		rc = pl_fmt_u32(out, (uint32_t)pl_ndet);
	if (rc == 0)
		rc = pl_fmt_cstr(out, "  thresh ");
	if (rc == 0)
		rc = pl_fmt_u32(out, pl_res.thresh_milli);
	if (rc == 0)
		rc = pl_fmt_cstr(out, "/1000\r\n");

	for (i = 0; i < pl_ndet && i < BF_MAX_DET && rc == 0; i++) {
		struct plugin_rect r;
		int have_box = pl_base != NULL &&
		               pl_base_to_frame(pl_base, pl_det[i].x, pl_det[i].y,
		                                pl_det[i].w, pl_det[i].h, &r) == 0;

		/* The trailing space is part of the literal.  It used to be a
		 * hand-counted length beside it -- "7, not 6" -- which is a comment that
		 * exists because the count is a second declaration of the string. */
		rc = pl_fmt_cstr(out, "  face ");
		if (rc == 0)
			rc = pl_fmt_u32(out, (uint32_t)i);
		/*
		 * [!] THE BOX, NOT JUST THE SCORE.  The first version of this printed
		 * a score and nothing else, and on hardware that showed up as `nn run`
		 * losing the coordinates it had always printed -- the resident path
		 * reports x/y/w/h and this did not.  No gate compares what a command
		 * PRINTS; it took a session next to the old output to see.
		 *
		 * In FRAME PIXELS, through the base's transform, because that is what
		 * the base offers.  The resident path prints percentages, which it can
		 * because the shell knows the frame size; a plugin does not, and
		 * hardcoding 320x240 here would be board knowledge in a plugin.  The
		 * unit is in the line so the two are not mistaken for each other.
		 */
		if (rc == 0 && have_box) {
			rc = pl_fmt_cstr(out, "  x ");
			if (rc == 0)
				rc = pl_fmt_u32(out, (uint32_t)(r.x0 < 0 ? 0 : r.x0));
			if (rc == 0)
				rc = pl_fmt_cstr(out, " y ");
			if (rc == 0)
				rc = pl_fmt_u32(out, (uint32_t)(r.y0 < 0 ? 0 : r.y0));
			if (rc == 0)
				rc = pl_fmt_cstr(out, " w ");
			if (rc == 0)
				rc = pl_fmt_u32(out, (uint32_t)(r.x1 > r.x0 ? r.x1 - r.x0 : 0));
			if (rc == 0)
				rc = pl_fmt_cstr(out, " h ");
			if (rc == 0)
				rc = pl_fmt_u32(out, (uint32_t)(r.y1 > r.y0 ? r.y1 - r.y0 : 0));
			if (rc == 0)
				rc = pl_fmt_cstr(out, " px");
		} else if (rc == 0) {
			rc = pl_fmt_cstr(out, "  outside the frame");
		}
		if (rc == 0)
			rc = pl_fmt_cstr(out, "  score ");
		/* milli, on the same scale the threshold is printed in: one command
		 * carrying two spellings of "score" is the divergence issue #50 spent
		 * itself removing. */
		if (rc == 0)
			rc = pl_fmt_u32(out, (uint32_t)(pl_det[i].score * 1000.0f));
		if (rc == 0)
			rc = pl_fmt_cstr(out, "\r\n");
	}
	return rc;
}

/* ---- parameters ----------------------------------------------------------- */

/** The only parameter this plugin has: the score threshold, in milli. */
#define PL_PARAM_THRESH_MILLI  0u

static int pl_param_set(uint32_t id, uint32_t value)
{
	if (id != PL_PARAM_THRESH_MILLI)
		return -1;
	return blazeface_set_thresh_milli(&pl_bf, (unsigned)value) == 0 ? 0 : -1;
}

static int pl_param_get(uint32_t id, uint32_t *value)
{
	if (id != PL_PARAM_THRESH_MILLI || value == NULL)
		return -1;
	*value = blazeface_get_thresh_milli(&pl_bf);
	return 0;
}

/* ---- the slot table ------------------------------------------------------- */

/*
 * [!] KEPT ALIVE BY THE LINKER SCRIPT, NOT BY A REFERENCE.  Nothing in this
 * image calls these; the loader reaches them through the manifest's offsets, so
 * --gc-sections would drop every one of them if the script did not KEEP the
 * section this table lives in.  The table is also what the packer reads to fill
 * those offsets in, which is why the order is enum plugin_slot's order and not
 * a convenient one.
 */
__attribute__((used, section(".plugin_slots")))
const void *const plugin_slot_table[PLUGIN_SLOT_COUNT] = {
	[PLUGIN_SLOT_ENTRY]     = (const void *)(uintptr_t)&pl_entry,
	[PLUGIN_SLOT_SHAPES_OK] = (const void *)(uintptr_t)&pl_shapes_ok,
	[PLUGIN_SLOT_DECODE]    = (const void *)(uintptr_t)&pl_decode,
	[PLUGIN_SLOT_DRAW]      = (const void *)(uintptr_t)&pl_draw,
	[PLUGIN_SLOT_REPORT]    = (const void *)(uintptr_t)&pl_report,
	[PLUGIN_SLOT_PARAM_SET] = (const void *)(uintptr_t)&pl_param_set,
	[PLUGIN_SLOT_PARAM_GET] = (const void *)(uintptr_t)&pl_param_get,
};

/*
 * The signatures are pinned here rather than trusted to review: an entry whose
 * type drifted from the ABI would still compile into the void* table above and
 * would fail on the board, in a callback, on the producer thread.
 */
_Static_assert(sizeof((plugin_entry_fn)pl_entry) == sizeof(void *), "");
_Static_assert(sizeof((plugin_shapes_ok_fn)pl_shapes_ok) == sizeof(void *), "");
_Static_assert(sizeof((plugin_decode_fn)pl_decode) == sizeof(void *), "");
_Static_assert(sizeof((plugin_draw_fn)pl_draw) == sizeof(void *), "");
_Static_assert(sizeof((plugin_report_fn)pl_report) == sizeof(void *), "");
_Static_assert(sizeof((plugin_param_set_fn)pl_param_set) == sizeof(void *), "");
_Static_assert(sizeof((plugin_param_get_fn)pl_param_get) == sizeof(void *), "");
