/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    test_plugin_cifar10.c
 * @brief   The classifier plugin: what it accepts, what it ranks, what it says
 *          (issue #103 = #78 Step 1b).
 *
 * The REAL plugin/cifar10/plugin_main.c, driven through its own slot table -- a
 * second binary rather than a section of test_plugin_decode.c because two
 * plugins are two PROGRAMS and each exports `plugin_slot_table`.  On the board
 * they never coexist either: one reservation, one loaded image.
 *
 * WHY THIS NEEDS A TEST AT ALL, given how little arithmetic there is.  Every
 * failure it can have is a confident sentence about the wrong thing, and none of
 * them looks like an error on the board:
 *
 *   - A LABEL OFF BY ONE.  `cat` for a `deer` is a working demo to anyone who is
 *     not holding a deer.  The mapping is pinned here against the vendor
 *     scenario app's order, which is where the ten names come from.
 *   - A RANKING THAT IS NOT A RANKING.  An insertion sort with the comparison
 *     the wrong way round still prints three rows in a tidy column.
 *   - AN OUTPUT VECTOR OF THE WRONG LENGTH ACCEPTED.  Reading ten entries of a
 *     1000-class model's output would name a CIFAR-10 label for an ImageNet
 *     score, and `nn stream`'s admission asks exactly this question before it
 *     starts a camera.
 *   - A REFUSAL PRINTED AS A RESULT.  "Nothing decoded yet" and "that is not
 *     this model" are opposite instructions to whoever is holding the board.
 *
 * The dequantisation constants below are the REAL model's, from
 * `verify_vela_model` over sdk/model_zoo/tflm_mb_cls/qat_pruning_model_vela.tflite:
 * INT8 [1x10], scale 0.0652714893, zero point 4.
 */
#include "plugin_abi.h"
#include "plugin_fmt.h"
#include "tensor.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

extern const void *const plugin_slot_table[PLUGIN_SLOT_COUNT];

/* ---- reporting ------------------------------------------------------------ */

static int failures;

static void expect(const char *what, int cond, const char *fmt, ...)
{
	va_list ap;

	if (cond) {
		printf("  ok   %s\n", what);
		return;
	}
	printf("  FAIL %s: ", what);
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	printf("\n");
	failures++;
}

/* ---- the base vtable ------------------------------------------------------ */

static void base_log(void *ctx, const char *s, size_t len)
{
	(void)ctx; (void)s; (void)len;
}

static int base_to_frame(void *ctx, float x, float y, float w, float h,
                         struct plugin_rect *out)
{
	(void)ctx; (void)x; (void)y; (void)w; (void)h; (void)out;
	return -1;   /* a classifier maps no boxes; it must not need this */
}

static const struct plugin_base_api base_api = {
	PLUGIN_ABI_VERSION,
	(uint32_t)sizeof(struct plugin_base_api),
	NULL,
	base_log,
	base_to_frame,
};

/* ---- a capturing printer -------------------------------------------------- */

static char     cap_buf[4096];
static size_t   cap_len;
static unsigned cap_calls;
static int      cap_fail_at = -1;

static int cap_write(void *ctx, const char *s, size_t len)
{
	(void)ctx;
	if (cap_fail_at >= 0 && cap_calls == (unsigned)cap_fail_at) {
		cap_calls++;
		return -1;
	}
	cap_calls++;
	if (cap_len + len < sizeof cap_buf) {
		memcpy(cap_buf + cap_len, s, len);
		cap_len += len;
		cap_buf[cap_len] = '\0';
	}
	return (int)len;
}

static struct plugin_printer printer = { NULL, cap_write };

static void cap_reset(void)
{
	cap_len = 0u;
	cap_calls = 0u;
	cap_fail_at = -1;
	cap_buf[0] = '\0';
}

/* ---- the model's output --------------------------------------------------- */

#define CLS_SCALE  0.0652714893f
#define CLS_ZP     4
#define CLS_N      10

static int8_t             scores[CLS_N];
static struct tensor_desc out;

static void set_out(uint8_t rank, int32_t d0, int32_t d1, int32_t d2, int32_t d3)
{
	memset(&out, 0, sizeof out);
	out.data       = scores;
	out.bytes      = sizeof scores;
	out.rank       = rank;
	out.dims[0]    = d0;
	out.dims[1]    = d1;
	out.dims[2]    = d2;
	out.dims[3]    = d3;
	out.dtype      = (uint8_t)TENSOR_DTYPE_INT8;
	out.scale      = CLS_SCALE;
	out.zero_point = CLS_ZP;
}

static void quiet(void)
{
	int i;

	for (i = 0; i < CLS_N; i++)
		scores[i] = (int8_t)CLS_ZP;    /* every class dequantises to 0 */
	set_out(2u, 1, CLS_N, 0, 0);
}

/* The dequantised output, in thousandths, the plugin should print for a byte. */
static int milli_of(int8_t q)
{
	return (int)(((float)q - (float)CLS_ZP) * CLS_SCALE * 1000.0f);
}

/* ---- the slots ------------------------------------------------------------ */

static plugin_entry_fn     pl_entry;
static plugin_shapes_ok_fn pl_shapes_ok;
static plugin_decode_fn    pl_decode;
static plugin_draw_fn      pl_draw;
static plugin_report_fn    pl_report;

/* ---- what draw() hands the base (issue #105) ------------------------------ */

/*
 * A recording painter.  What matters is not what the label LOOKS like -- a
 * bitmap comparison would pin the font and break on every glyph edit -- but the
 * shape of the hand-off: exactly one blit, of a rectangle whose extents match
 * the source it declares, and nothing at all when there is no valid decode.
 * That last one is the property with teeth: the alternative to "nothing" is last
 * frame's label painted over a live picture indefinitely.
 */
static unsigned draw_blits, draw_rects, draw_fills;
static struct plugin_rect draw_last;
static uint32_t draw_last_stride;
static int32_t  draw_last_key;
static const uint16_t *draw_last_src;

static void rec_rect(void *ctx, const struct plugin_rect *r, uint16_t c,
                     uint16_t stroke)
{
	(void)ctx; (void)r; (void)c; (void)stroke;
	draw_rects++;
}

static void rec_fill(void *ctx, const struct plugin_rect *r, uint16_t c)
{
	(void)ctx; (void)r; (void)c;
	draw_fills++;
}

static void rec_blit(void *ctx, const struct plugin_rect *r,
                     const uint16_t *src, uint32_t stride, int32_t key)
{
	(void)ctx;
	draw_blits++;
	draw_last        = *r;
	draw_last_src    = src;
	draw_last_stride = stride;
	draw_last_key    = key;
}

static const struct plugin_painter rec_painter = {
	NULL, rec_rect, rec_fill, rec_blit,
};

static void draw_reset(void)
{
	draw_blits = 0u;
	draw_rects = 0u;
	draw_fills = 0u;
	draw_last_src = NULL;
}

/* Is every pixel of the declared source rectangle the same value? */
static int strip_uniform(void)
{
	uint32_t w = (uint32_t)(draw_last.x1 - draw_last.x0);
	uint32_t h = (uint32_t)(draw_last.y1 - draw_last.y0);
	uint32_t x, y;

	if (draw_last_src == NULL || w == 0u || h == 0u)
		return 1;
	for (y = 0u; y < h; y++)
		for (x = 0u; x < w; x++)
			if (draw_last_src[y * draw_last_stride + x] != draw_last_src[0])
				return 0;
	return 1;
}

int main(void)
{
	char want[128];
	int n;

	printf("test_plugin_cifar10\n");

	pl_entry = (plugin_entry_fn)(uintptr_t)plugin_slot_table[PLUGIN_SLOT_ENTRY];
	pl_shapes_ok =
		(plugin_shapes_ok_fn)(uintptr_t)plugin_slot_table[PLUGIN_SLOT_SHAPES_OK];
	pl_decode = (plugin_decode_fn)(uintptr_t)plugin_slot_table[PLUGIN_SLOT_DECODE];
	pl_draw = (plugin_draw_fn)(uintptr_t)plugin_slot_table[PLUGIN_SLOT_DRAW];
	pl_report = (plugin_report_fn)(uintptr_t)plugin_slot_table[PLUGIN_SLOT_REPORT];

	/* ---- the manifest this build will produce -------------------------- */
	/*
	 * [!] THE ABSENT SLOTS ARE PART OF THE DESIGN, not an oversight.  The packer
	 * derives the capability word from this table, so a NULL here is what makes
	 * the container say so -- and `nn stream` refuses a decoder that cannot draw
	 * rather than running a preview that annotates nothing.
	 *
	 * [!] DRAW IS NO LONGER ONE OF THEM (issue #105 = #78 Step 2).  This
	 * assertion used to read "draw is deliberately absent (a label needs a
	 * font)", and flipping it is the whole of Step 2: the font is in the plugin
	 * now, so the classifier can hold the panel and `nn stream` admits it.
	 */
	expect("the three mandatory slots are exported",
	       pl_entry != NULL && pl_shapes_ok != NULL && pl_decode != NULL,
	       "one of them is absent");
	expect("report is exported -- it is the whole point of this plugin",
	       pl_report != NULL, "absent");
	expect("[!] draw is exported -- the classifier annotates the panel (#105)",
	       pl_draw != NULL, "absent");
	expect("[!] and so are the parameters -- a classifier has no threshold",
	       plugin_slot_table[PLUGIN_SLOT_PARAM_SET] == NULL &&
	       plugin_slot_table[PLUGIN_SLOT_PARAM_GET] == NULL, "present");

	/* ---- before entry() ------------------------------------------------ */
	quiet();
	n = pl_decode(&out, 1u);
	expect("a decode before entry() refuses, distinctly", n == -2,
	       "got %d", n);
	cap_reset();
	(void)pl_report(&printer);
	expect("and the report says which refusal it was",
	       strstr(cap_buf, "nothing has been decoded yet") != NULL,
	       "report was:\n%s", cap_buf);

	/* ---- entry --------------------------------------------------------- */
	expect("the plugin accepts the base vtable", pl_entry(&base_api) == 0,
	       "refused");
	{
		struct plugin_base_api bad = base_api;

		bad.size = (uint32_t)sizeof(struct plugin_base_api) - 4u;
		expect("a base of the wrong size is refused", pl_entry(&bad) != 0,
		       "accepted");
		bad = base_api;
		bad.version = PLUGIN_ABI_VERSION + 1u;
		expect("so is a base of the wrong ABI version", pl_entry(&bad) != 0,
		       "accepted");
		bad = base_api;
		bad.log = NULL;
		expect("and one with a hole in its vtable", pl_entry(&bad) != 0,
		       "accepted");
		expect("re-entry with the right vtable succeeds",
		       pl_entry(&base_api) == 0, "refused");
	}

	/* ---- what it will and will not read -------------------------------- */
	quiet();
	expect("the model's own [1x10] int8 output is accepted",
	       pl_shapes_ok(&out, 1u) != 0, "refused");

	set_out(4u, 1, 1, 1, CLS_N);
	expect("and the same tensor spelled [1x1x1x10]",
	       pl_shapes_ok(&out, 1u) != 0, "refused");

	set_out(2u, 1, CLS_N + 1, 0, 0);
	expect("[!] a vector of a different length is NOT this model",
	       pl_shapes_ok(&out, 1u) == 0, "accepted");

	set_out(2u, 1, CLS_N, 0, 0);
	out.dtype = (uint8_t)TENSOR_DTYPE_FLOAT32;
	expect("a float32 output is refused, not read as bytes",
	       pl_shapes_ok(&out, 1u) == 0, "accepted");

	set_out(2u, 1, CLS_N, 0, 0);
	out.dtype = (uint8_t)TENSOR_DTYPE_UNSUPPORTED;
	expect("and so is the descriptor's 'not representable' marker",
	       pl_shapes_ok(&out, 1u) == 0, "accepted");

	set_out(2u, 1, CLS_N, 0, 0);
	out.scale = 0.0f;
	expect("[!] a zero scale is refused -- it would score every class the same",
	       pl_shapes_ok(&out, 1u) == 0, "accepted");

	set_out(2u, 1, CLS_N, 0, 0);
	out.bytes = CLS_N - 1u;
	expect("a buffer shorter than the shape is refused",
	       pl_shapes_ok(&out, 1u) == 0, "accepted");

	set_out(2u, 1, CLS_N, 0, 0);
	out.data = NULL;
	expect("a null data pointer is refused", pl_shapes_ok(&out, 1u) == 0,
	       "accepted");

	set_out(0u, 0, 0, 0, 0);
	expect("a rank-0 tensor is refused", pl_shapes_ok(&out, 1u) == 0,
	       "accepted");

	quiet();
	expect("two outputs are not this model either",
	       pl_shapes_ok(&out, 2u) == 0, "accepted");
	expect("nor none at all", pl_shapes_ok(&out, 0u) == 0, "accepted");
	expect("a null array is refused", pl_shapes_ok(NULL, 1u) == 0, "accepted");

	/* A decode agrees with shapes_ok about every one of those. */
	set_out(2u, 1, CLS_N + 1, 0, 0);
	n = pl_decode(&out, 1u);
	expect("decode refuses what shapes_ok refused, with the model code",
	       n == -1, "got %d", n);
	cap_reset();
	(void)pl_report(&printer);
	expect("[!] and the report says so rather than printing zero classes",
	       strstr(cap_buf, "not the 10-class CIFAR-10 classifier") != NULL,
	       "report was:\n%s", cap_buf);

	n = pl_decode(NULL, 1u);
	expect("a null array is an argument error, a different code", n == -3,
	       "got %d", n);

	/* ---- the ranking --------------------------------------------------- */
	/*
	 * Class 3 (`cat`) strongest, then 8 (`ship`), then 0 (`airplane`); the rest
	 * are at the zero point.  Chosen so the order is neither ascending nor
	 * descending by index -- a sort that ignored the score would still be
	 * "right" against a monotonic scene.
	 */
	quiet();
	scores[3] = 100;
	scores[8] = 60;
	scores[0] = 20;
	scores[5] = (int8_t)(CLS_ZP - 30);   /* clearly negative, and not in the top */

	n = pl_decode(&out, 1u);
	expect("the decode returns the number of rows it will report", n == 3,
	       "got %d", n);

	cap_reset();
	expect("the report succeeds", pl_report(&printer) == 0, "refused");
	expect("and names the model and how many rows",
	       strstr(cap_buf, "cifar10  top 3 of 10  (dequantised outputs, not "
	                       "probabilities)") != NULL,
	       "report was:\n%s", cap_buf);
	/*
	 * [!] AND DOES NOT CALL THE NUMBER A SCORE.  The first version did, on the
	 * reasoning that milli is the unit `nn dets` prints -- but that one is a
	 * sigmoid probability and this is a dequantised model output on a scale
	 * that runs to about -8.6.  Hardware printed `#1 cat score -783/1000`
	 * before anyone noticed the unit was wrong, which no gate could have.
	 */
	expect("[!] and does not call a dequantised output a score",
	       strstr(cap_buf, "score") == NULL, "report was:\n%s", cap_buf);

	/*
	 * [!] LABEL AND ORDER TOGETHER, IN ONE STRING.  Checked as whole lines
	 * because a label lookup off by one and a ranking the wrong way round each
	 * produce a report in which every individual token is present.
	 */
	snprintf(want, sizeof want, "  #1  cat         out  %d/1000  raw  100\r\n",
	         milli_of(100));
	expect("[!] #1 is the strongest class, by its own name",
	       strstr(cap_buf, want) != NULL, "want '%s' in:\n%s", want, cap_buf);

	snprintf(want, sizeof want, "  #2  ship        out  %d/1000  raw   60\r\n",
	         milli_of(60));
	expect("[!] #2 is the next strongest", strstr(cap_buf, want) != NULL,
	       "want '%s' in:\n%s", want, cap_buf);

	snprintf(want, sizeof want, "  #3  airplane    out  %d/1000  raw   20\r\n",
	         milli_of(20));
	expect("[!] #3 is the third", strstr(cap_buf, want) != NULL,
	       "want '%s' in:\n%s", want, cap_buf);

	expect("and the classes below the top three are not printed",
	       strstr(cap_buf, "dog") == NULL && strstr(cap_buf, "#4") == NULL,
	       "report was:\n%s", cap_buf);

	/* Every label, at least once, in the vendor app's order. */
	{
		static const char *const names[CLS_N] = {
			"airplane", "automobile", "bird", "cat", "deer",
			"dog", "frog", "horse", "ship", "truck",
		};
		int i;

		for (i = 0; i < CLS_N; i++) {
			quiet();
			scores[i] = 100;
			(void)pl_decode(&out, 1u);
			cap_reset();
			(void)pl_report(&printer);
			snprintf(want, sizeof want, "  #1  %s", names[i]);
			expect("[!] each class index prints the vendor app's label",
			       strstr(cap_buf, want) != NULL,
			       "class %d: want '%s' in:\n%s", i, want, cap_buf);
		}
	}

	/* A negative output is printed with its sign, not as a huge unsigned. */
	quiet();
	for (n = 0; n < CLS_N; n++)
		scores[n] = (int8_t)(CLS_ZP - 50 - n);
	(void)pl_decode(&out, 1u);
	cap_reset();
	(void)pl_report(&printer);
	snprintf(want, sizeof want, "out %d/1000", milli_of((int8_t)(CLS_ZP - 50)));
	expect("a negative output keeps its sign", strstr(cap_buf, want) != NULL &&
	       milli_of((int8_t)(CLS_ZP - 50)) < 0, "want '%s' in:\n%s", want,
	       cap_buf);

	/* Ties do not reorder: equal scores keep the lower class index first. */
	quiet();
	scores[2] = 100;
	scores[7] = 100;
	(void)pl_decode(&out, 1u);
	cap_reset();
	(void)pl_report(&printer);
	expect("a tie is broken towards the lower class index, deterministically",
	       strstr(cap_buf, "  #1  bird") != NULL &&
	       strstr(cap_buf, "  #2  horse") != NULL, "report was:\n%s", cap_buf);

	/* ---- a writer that refuses ----------------------------------------- */
	quiet();
	scores[3] = 100;
	(void)pl_decode(&out, 1u);
	cap_reset();
	cap_fail_at = 1;
	expect("[!] a writer that refuses is propagated, not written past",
	       pl_report(&printer) < 0, "reported success");
	expect("and nothing was written after the refusal", cap_calls == 2u,
	       "%u write(s) -- it carried on", cap_calls);

	/* ---- the panel (issue #105 = #78 Step 2) --------------------------- */

	/*
	 * [!] ONE BLIT, AND NOTHING ELSE.  draw() runs on the panel thread with the
	 * panel guard held; the glyphs are rasterised in decode() on the producer.
	 * A fill_rect or a run of rects here would mean the plugin had started
	 * painting through the base under the guard, which is the split
	 * cam_lcd_sink.h exists to keep.
	 */
	quiet();
	scores[3] = 100;
	(void)pl_decode(&out, 1u);
	draw_reset();
	pl_draw(&rec_painter);
	expect("a decoded frame paints exactly one blit and nothing else",
	       draw_blits == 1u && draw_rects == 0u && draw_fills == 0u,
	       "%u blit(s), %u rect(s), %u fill(s)",
	       draw_blits, draw_rects, draw_fills);
	expect("the strip is anchored at the frame origin",
	       draw_last.x0 == 0 && draw_last.y0 == 0,
	       "at %ld,%ld", (long)draw_last.x0, (long)draw_last.y0);
	expect("its stride matches the width it declares",
	       draw_last_stride == (uint32_t)(draw_last.x1 - draw_last.x0),
	       "stride %lu for width %ld", (unsigned long)draw_last_stride,
	       (long)(draw_last.x1 - draw_last.x0));
	expect("and the blit is opaque, so the label stays legible over the scene",
	       draw_last_key < 0, "keyed on %ld", (long)draw_last_key);
	expect("[!] something was actually rasterised, not a blank bar",
	       !strip_uniform(), "every pixel is the same value");

	expect("a null painter is survivable", (pl_draw(NULL), 1), "");
	{
		struct plugin_painter half = { NULL, rec_rect, rec_fill, NULL };

		draw_reset();
		pl_draw(&half);
		expect("and so is a painter with no blit",
		       draw_rects == 0u && draw_fills == 0u,
		       "it painted something else instead");
	}

	/*
	 * [!] A FAILED DECODE LEAVES NOTHING ON THE PANEL.  The firmware already
	 * declines to call draw() for such a frame -- nn_overlay.c returns non-zero
	 * and the sink never installs the hook -- so this is the second line of
	 * defence, and it is the one that survives someone adding an early return
	 * to decode() later.  Without it the panel would keep showing the last good
	 * label over a live picture, which reads as a working classifier.
	 */
	quiet();
	(void)pl_decode(NULL, 1u);
	draw_reset();
	pl_draw(&rec_painter);
	expect("[!] after a failed decode the panel gets nothing at all",
	       draw_blits == 0u && draw_rects == 0u && draw_fills == 0u,
	       "%u blit(s) -- last frame's label would still be showing",
	       draw_blits);

	/* And a fresh good decode brings it back, so the flag is not one-way. */
	quiet();
	scores[3] = 100;
	(void)pl_decode(&out, 1u);
	draw_reset();
	pl_draw(&rec_painter);
	expect("and the next good decode restores it", draw_blits == 1u,
	       "%u blit(s)", draw_blits);

	if (failures) {
		printf("test_plugin_cifar10: %d failure(s)\n", failures);
		return 1;
	}
	printf("test_plugin_cifar10: all cases pass\n");
	return 0;
}
