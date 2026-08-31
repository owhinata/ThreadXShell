/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    plugin_main.c
 * @brief   CIFAR-10 classification as a loadable plugin (issue #103 = #78
 *          Step 1b).
 *
 * [!] THIS FILE IS THE POINT OF ISSUE #78.  The firmware holds one decoder, and
 * it decodes BlazeFace.  A classifier's output -- a vector of class scores and
 * the NAMES those classes have -- is something no amount of firmware
 * generalisation can supply, because the names come with the model and nowhere
 * else.  Before containers, `nn run` on the classification model printed
 * `class 3  score 812/1000` and a human looked the number up in a table that
 * lived on the PC.  A container carries the labels with the weights, and this is
 * the code that reads them out.
 *
 * THE MODEL is the Himax SDK's `tflm_mb_cls` MobileNet, which the store holds
 * under the name `cls`: input INT8 [1x224x224x3] scale 0.0203246 zp -8, output
 * INT8 [1x10] scale 0.0652715 zp 4, trained on CIFAR-10.  The ten labels are
 * the vendor scenario app's own
 * (sdk/EPII_CM55M_APP_S/app/scenario_app/tflm_mb_cls/cvapp_mb_cls.cpp), so they
 * are the model's, not a guess made here.
 *
 * [!] AND THE INPUT IS FED THE WAY THE VENDOR FEEDS IT.  That app writes
 * `pixel - 128` into the input tensor and ignores the recorded scale and zero
 * point, exactly as this port's nn_preproc_fill() does.  The firmware used to
 * refuse that pair -- rightly, for the RESIDENT BlazeFace decoder, whose
 * arithmetic assumed the fill convention matched the quantisation -- and issue
 * #103 had to waive the check for plugins so that this container's labels could
 * be read at all.  Issue #104 removed that decoder and the check went with it:
 * a plugin ships WITH its model, and shipping it is the statement that the two
 * agree.  The fill convention is now a board property, documented rather than
 * enforced (nn_preproc.h).
 *
 * [!] AND IT DRAWS, SINCE ISSUE #105 (#78 Step 2).  Putting a label on the panel
 * needs a font, and the font is HERE rather than in the firmware for the same
 * reason the labels are: see plugin_text.h.  Until this existed `nn stream`
 * refused the classifier outright -- a live preview that annotates nothing is
 * indistinguishable from a broken one -- so the classifier could only ever be
 * read one frame at a time through `nn run`.
 *
 * [!] NO PARAMETERS STILL.  A threshold is a detector's idea; the top of a
 * class vector is always reported.  With PARAM_GET absent, `nn thresh` reads
 * `none` -- which is the honest answer, and is now literally what the shim
 * returns.  Before issue #104 it borrowed the resident decoder's number, so a
 * console showed `644/1000` while this container was loaded and nothing on the
 * board would ever read it.
 */
#include "plugin_abi.h"
#include "plugin_base.h"
#include "plugin_fmt.h"
#include "plugin_text.h"
#include "tensor.h"

#include <stddef.h>
#include <stdint.h>

/* ---- the model's classes -------------------------------------------------- */

/** Classes the model scores.  Fixed: the labels below are these ten. */
#define PL_CLASSES 10

/** Rows the report prints.  Ten would be a wall of near-zero scores. */
#define PL_TOP     3

/*
 * [!] THE LABELS TRAVEL WITH THE CODE THAT READS THEM.  They are the model's
 * property, so they belong in the container and not in the firmware; a base that
 * carried them would have to be rebuilt and reflashed for the next model, which
 * is the errand issue #78 exists to remove.
 */
static const char *const pl_label[PL_CLASSES] = {
	"airplane", "automobile", "bird", "cat", "deer",
	"dog", "frog", "horse", "ship", "truck",
};

/** Widest label above, so the score column lines up. */
#define PL_LABEL_W 10u

/* ---- state ---------------------------------------------------------------- */

struct pl_entry_row {
	int32_t milli;   /**< dequantised score x 1000                          */
	int32_t raw;     /**< the quantised byte, before the scale was applied  */
	int     cls;
};

static struct pl_entry_row pl_top[PL_TOP];
static int                 pl_ntop;
static int                 pl_status;   /**< 0, or a negative refusal         */

static const struct plugin_base_api *pl_base;

/* ---- the label on the panel (issue #105) ---------------------------------- */

/*
 * The strip, and every extent derived from one place.
 *
 * [!] THE _Static_asserts BELOW ARE THE BOUNDS CHECK.  plugin_painter's blit
 * takes the rectangle's width and height as the SOURCE extent and reads through
 * the stride it is given; nothing on the device or in the gate proves that the
 * buffer behind that pointer is big enough -- check_plugin_image.py says as much
 * in its own header.  So the relations are asserted where the array is declared,
 * which is the only place they can be checked at all.
 *
 * PL_CHARS is what the widest line needs: the label column, two spaces, and a
 * signed value.  A line longer than this is REFUSED by the string sink rather
 * than truncated, so the failure is a missing field and not a wrong number.
 */
#define PL_SCALE  2u
#define PL_PAD    2u
#define PL_CHARS  (PL_LABEL_W + 2u + 6u)

#define PL_STRIP_W (2u * PL_PAD + PL_TEXT_CELL_W * PL_CHARS * PL_SCALE)
#define PL_STRIP_H (2u * PL_PAD + PL_TEXT_CELL_H * PL_SCALE)

/* Where it sits, in FRAME pixels.  The origin, and not the bottom edge, because
 * a plugin is not told the frame's geometry -- the base publishes only the
 * model-input transform, and inventing a way to ask would be an ABI change for
 * a cosmetic gain.  See the board README. */
#define PL_STRIP_X 0
#define PL_STRIP_Y 0

/* Opaque, so the label stays legible over whatever the camera is looking at. */
#define PL_BG 0x0000u   /**< black    */
#define PL_FG 0xFFFFu   /**< white    */

static uint16_t pl_strip[PL_STRIP_W * PL_STRIP_H];

/* The stride this buffer is described with is its width, and the array holds a
 * whole rectangle of that stride.  Both are what plugin_text.h asks a caller to
 * assert, and what blit assumes without being able to check. */
_Static_assert(sizeof pl_strip / sizeof pl_strip[0] == PL_STRIP_W * PL_STRIP_H,
               "the strip is not the rectangle its extents describe");
_Static_assert(PL_STRIP_W >= 2u * PL_PAD + PL_TEXT_CELL_W * PL_CHARS * PL_SCALE,
               "the strip is narrower than the longest line it must hold");
_Static_assert(PL_STRIP_H >= 2u * PL_PAD + PL_TEXT_CELL_H * PL_SCALE,
               "the strip is shorter than one line of text");

/*
 * [!] SEPARATE FROM pl_status, AND CLEARED FIRST.  The firmware already
 * declines to call draw() for a frame whose decode failed -- nn_overlay.c
 * returns non-zero and the sink never sets its overlay hook -- so this is
 * defence in depth.  It is worth having anyway: it is the flag that makes "an
 * early return added later" show as a missing label rather than as LAST FRAME'S
 * label, painted over a live picture, indefinitely.
 */
static int pl_draw_valid;

/*
 * Refusals.
 *
 * [!] NOT ONE CODE.  "Nothing has decoded yet" and "the open model is not a
 * classifier" call for opposite actions -- run something, or load something
 * else -- and folding either into "no classes" would report a measurement that
 * was never made.  The same rule svc/blazeface.h states for its own codes.
 */
#define PL_ERR_MODEL   (-1)   /**< the outputs are not this model's shape     */
#define PL_ERR_UNINIT  (-2)   /**< entry() has not run                        */
#define PL_ERR_ARG     (-3)   /**< a null argument                            */

/* ---- entry ---------------------------------------------------------------- */

/*
 * [!] THE BASE IS CHECKED BEFORE ANY MEMBER IS USED.  version and size come
 * first in the struct precisely so that a base older than this plugin is refused
 * rather than called through a struct shorter than this file thinks.
 *
 * to_frame is required even though nothing here maps a box: the base publishes
 * one vtable for every plugin, and a base that could not supply it is a base
 * this image was not built against.
 */
static int pl_entry(const struct plugin_base_api *base)
{
	int i;

	if (base == NULL)
		return -1;
	if (base->version != PLUGIN_ABI_VERSION)
		return -1;
	if (base->size != (uint32_t)sizeof(struct plugin_base_api))
		return -1;
	if (base->log == NULL || base->to_frame == NULL)
		return -1;

	pl_base       = base;
	pl_ntop       = 0;
	pl_status     = PL_ERR_UNINIT;
	pl_draw_valid = 0;
	for (i = 0; i < PL_TOP; i++) {
		pl_top[i].cls   = -1;
		pl_top[i].milli = 0;
		pl_top[i].raw   = 0;
	}
	return 0;
}

/* ---- the model ------------------------------------------------------------ */

/*
 * Is this the one output this model has?
 *
 * The shape is checked as "PL_CLASSES elements, everything else a 1", not as a
 * literal rank-2 [1x10]: the same graph reaches the two paths through different
 * converters and a classifier that emitted [1x1x1x10] would be the same tensor.
 * What is NOT relaxed is the element count -- a vector of a different length is
 * a different model, and reading its first ten entries would produce a confident
 * answer about nothing.
 */
static const struct tensor_desc *pl_find(const struct tensor_desc *outs,
                                         unsigned n)
{
	const struct tensor_desc *t;
	int32_t elems = 1;
	unsigned i;

	if (outs == NULL || n != 1u)
		return NULL;
	t = &outs[0];
	if (t->data == NULL || t->rank == 0u || t->rank > TENSOR_MAX_DIMS)
		return NULL;
	/* [!] int8 only, and said so rather than assumed.  This board's models are
	 * int8 throughout -- the op resolver carries AddEthosU() and nothing else --
	 * and reading some other buffer as bytes would score classes from noise. */
	if (t->dtype != (uint8_t)TENSOR_DTYPE_INT8)
		return NULL;
	for (i = 0u; i < t->rank; i++) {
		if (t->dims[i] <= 0)
			return NULL;
		elems *= t->dims[i];
	}
	if (elems != PL_CLASSES)
		return NULL;
	if (t->bytes < (size_t)PL_CLASSES)
		return NULL;
	/* A zero or non-finite scale dequantises every class to the same number,
	 * which would come out as a confident answer for class 0. */
	if (!(t->scale > 0.0f) || t->scale != t->scale)
		return NULL;
	return t;
}

static int pl_shapes_ok(const struct tensor_desc *outs, unsigned n)
{
	return pl_find(outs, n) != NULL;
}

/*
 * Decode: dequantise the ten classes and keep the best PL_TOP.
 *
 * An insertion sort over three entries: the vector is ten long, so anything
 * cleverer would cost more code than it saves work, and this runs on the camera
 * producer thread where the budget is the whole frame period.
 */
/*
 * Rasterise the winning class into the strip.
 *
 * [!] ON THE PRODUCER THREAD, WITH NO PANEL GUARD HELD, and that is the whole
 * reason it is here rather than in draw().  draw() runs on the panel thread
 * inside the guard, where everything else that wants the panel is failing its
 * non-blocking acquire; the frame pipeline pre-pins one delivery per sink, so
 * this and draw() strictly alternate and the hand-off needs no lock.  This is
 * the split cam_lcd_sink.h already documents -- no new discipline is invented
 * for loaded code.
 *
 * The number is formatted through the EXISTING pl_fmt_* helpers, pointed at a
 * char buffer: a second integer formatter in plugin_text.c would be a second
 * chance to spell the INT32_MIN case wrong.
 */
static void pl_stage_label(void)
{
	struct pl_text_target t;
	struct plugin_printer out;
	struct pl_sbuf sb;
	char line[PL_CHARS + 1u];
	int rc;

	t.px     = pl_strip;
	t.w      = PL_STRIP_W;
	t.h      = PL_STRIP_H;
	t.stride = PL_STRIP_W;

	pl_sbuf_init(&sb, line, sizeof line);
	pl_sbuf_printer(&out, &sb);

	rc = pl_fmt_cstr_pad(&out, pl_label[pl_top[0].cls], PL_LABEL_W);
	if (rc == 0)
		rc = pl_fmt_cstr(&out, "  ");
	if (rc == 0)
		rc = pl_fmt_i32_pad(&out, pl_top[0].milli, 6u);
	/* A refused write leaves the prefix, which is a shorter label rather than
	 * a wrong one; there is nothing further to do about it here. */
	(void)rc;

	pl_text_fill(&t, 0, 0, (int32_t)PL_STRIP_W, (int32_t)PL_STRIP_H, PL_BG);
	(void)pl_text_draw(&t, (int32_t)PL_PAD, (int32_t)PL_PAD, line, PL_SCALE,
	                   PL_FG);
}

static int pl_decode(const struct tensor_desc *outs, unsigned n)
{
	const struct tensor_desc *t;
	const int8_t *q;
	int i, k, j;

	/* First, before any path can return: what is on the panel must never
	 * outlive the decode that produced it. */
	pl_draw_valid = 0;

	if (pl_base == NULL) {
		pl_status = PL_ERR_UNINIT;
		pl_ntop = 0;
		return pl_status;
	}
	if (outs == NULL) {
		pl_status = PL_ERR_ARG;
		pl_ntop = 0;
		return pl_status;
	}
	t = pl_find(outs, n);
	if (t == NULL) {
		pl_status = PL_ERR_MODEL;
		pl_ntop = 0;
		return pl_status;
	}

	q = (const int8_t *)t->data;
	pl_ntop = 0;
	for (i = 0; i < PL_CLASSES; i++) {
		int32_t milli = (int32_t)(((float)q[i] - (float)t->zero_point) *
		                          t->scale * 1000.0f);

		for (k = 0; k < PL_TOP; k++) {
			if (k < pl_ntop && milli <= pl_top[k].milli)
				continue;
			for (j = PL_TOP - 1; j > k; j--)
				pl_top[j] = pl_top[j - 1];
			pl_top[k].milli = milli;
			pl_top[k].raw   = q[i];
			pl_top[k].cls   = i;
			if (pl_ntop < PL_TOP)
				pl_ntop++;
			break;
		}
	}

	pl_status = 0;
	pl_stage_label();
	pl_draw_valid = 1;
	return pl_ntop;
}

/*
 * Paint the last decode.
 *
 * One blit of a finished rectangle: everything expensive already happened on the
 * producer.  Opaque -- a negative key -- because a solid bar is what keeps a
 * label legible over an arbitrary camera scene, and because a colour-keyed blit
 * is charged for every source pixel it READS anyway, so transparency would buy
 * nothing from the budget.
 */
static void pl_draw(const struct plugin_painter *paint)
{
	struct plugin_rect r;

	if (paint == NULL || paint->blit == NULL || !pl_draw_valid)
		return;

	r.x0 = PL_STRIP_X;
	r.y0 = PL_STRIP_Y;
	r.x1 = PL_STRIP_X + (int32_t)PL_STRIP_W;
	r.y1 = PL_STRIP_Y + (int32_t)PL_STRIP_H;
	pl_paint_blit(paint, &r, pl_strip, PL_STRIP_W, -1);
}

/* ---- the console ---------------------------------------------------------- */

static int pl_report_refusal(const struct plugin_printer *out)
{
	const char *why;
	int rc;

	switch (pl_status) {
	case PL_ERR_MODEL:
		why = "the open model is not the 10-class CIFAR-10 classifier";
		break;
	case PL_ERR_UNINIT:
		why = "nothing has been decoded yet";
		break;
	default:
		why = "a bad argument reached the decoder";
		break;
	}
	rc = pl_fmt_cstr(out, "cifar10: no result (");
	if (rc == 0)
		rc = pl_fmt_cstr(out, why);
	if (rc == 0)
		rc = pl_fmt_cstr(out, ")\r\n");
	return rc;
}

/*
 * Describe the last decode.
 *
 * [!] EVERY FAILURE PROPAGATES -- see plugin_fmt.h.  And the whole point of the
 * report is the LABEL: the raw byte and the milli score are printed beside it
 * because the raw one is the half that does not depend on the scale being right,
 * which is what gets checked first when a dequantised number looks wrong.
 */
static int pl_report(const struct plugin_printer *out)
{
	int i, rc;

	if (out == NULL || out->write == NULL)
		return -1;
	if (pl_status != 0)
		return pl_report_refusal(out);

	rc = pl_fmt_cstr(out, "cifar10  top ");
	if (rc == 0)
		rc = pl_fmt_u32(out, (uint32_t)pl_ntop);
	if (rc == 0)
		rc = pl_fmt_cstr(out, " of 10  (dequantised outputs, not "
		                      "probabilities)\r\n");

	for (i = 0; i < pl_ntop && rc == 0; i++) {
		rc = pl_fmt_cstr(out, "  #");
		if (rc == 0)
			rc = pl_fmt_u32(out, (uint32_t)(i + 1));
		if (rc == 0)
			rc = pl_fmt_cstr(out, "  ");
		if (rc == 0)
			rc = pl_fmt_cstr_pad(out, pl_label[pl_top[i].cls], PL_LABEL_W);
		/*
		 * [!] NOT CALLED A SCORE, AND THE BOARD IS WHAT SETTLED IT.  The first
		 * version printed `score N/1000` with a comment claiming that put it on
		 * the same scale as `nn dets`, which prints a sigmoid probability.  It
		 * does not: this model's output tensor is quantised at scale 0.065 zero
		 * point 4, so it represents roughly -8.6..+8.0 -- and the first run on
		 * hardware printed `#1 cat score -783/1000`.  A negative probability is
		 * not a near miss, it is the wrong unit, and the comment asserting the
		 * unit was right is exactly how that survives review.
		 *
		 * `out` is the vocabulary `nn out` already uses for the dequantised
		 * value of an output tensor, which is precisely what this is.  It is
		 * left as the model produced it rather than turned into a probability:
		 * a softmax needs exp(), the plugin links no libm, and an approximation
		 * would replace a number the model computed with one this file
		 * invented.  The raw byte beside it is the half that does not depend on
		 * the scale being right.
		 *
		 * The shared `nn run` class report cannot make this distinction -- it
		 * sees an arbitrary output vector and has no idea what the model meant
		 * by it.  A plugin does.  That difference is the whole of issue #78 in
		 * one line of formatting.
		 */
		if (rc == 0)
			rc = pl_fmt_cstr(out, "  out ");
		if (rc == 0)
			rc = pl_fmt_i32_pad(out, pl_top[i].milli, 5u);
		if (rc == 0)
			rc = pl_fmt_cstr(out, "/1000  raw ");
		if (rc == 0)
			rc = pl_fmt_i32_pad(out, pl_top[i].raw, 4u);
		if (rc == 0)
			rc = pl_fmt_cstr(out, "\r\n");
	}
	return rc;
}

/* ---- the slot table ------------------------------------------------------- */

/*
 * [!] KEPT ALIVE BY THE LINKER SCRIPT, NOT BY A REFERENCE.  Nothing in this
 * image calls these; the loader reaches them through the manifest's offsets, so
 * --gc-sections would drop every one of them if the script did not KEEP the
 * section this table lives in.  The table is also what the packer reads to fill
 * those offsets in, which is why the order is enum plugin_slot's order.
 *
 * PARAM_SET and PARAM_GET are left NULL.  The packer turns a NULL into
 * PLUGIN_SLOT_ABSENT and derives the capability word from the same table, so
 * "the manifest says DRAW and the slot is empty" is not a state this build can
 * produce.
 */
__attribute__((used, section(".plugin_slots")))
const void *const plugin_slot_table[PLUGIN_SLOT_COUNT] = {
	[PLUGIN_SLOT_ENTRY]     = (const void *)(uintptr_t)&pl_entry,
	[PLUGIN_SLOT_SHAPES_OK] = (const void *)(uintptr_t)&pl_shapes_ok,
	[PLUGIN_SLOT_DECODE]    = (const void *)(uintptr_t)&pl_decode,
	[PLUGIN_SLOT_DRAW]      = (const void *)(uintptr_t)&pl_draw,
	[PLUGIN_SLOT_REPORT]    = (const void *)(uintptr_t)&pl_report,
	[PLUGIN_SLOT_PARAM_SET] = NULL,
	[PLUGIN_SLOT_PARAM_GET] = NULL,
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
