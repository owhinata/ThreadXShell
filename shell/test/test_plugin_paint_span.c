/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Host tests for svc/plugin_paint_span.c (issue #126): what a painter primitive
 * decides before its first store, shared by grove-vision-ai-v2 and wio-lite-ai.
 *
 * What the LOOPS store is not tested here and cannot be: each board's
 * test_plugin_paint.c counts the stores its own loop makes and compares them
 * with what this file's functions charged.  Here the questions are the ones
 * with no pixel in them -- which part of a rectangle survives, which part of a
 * blit's source that is, what is charged, and that a refusal leaves the caller
 * nothing to draw.
 *
 * [!] EVERY GEOMETRY CASE RUNS ON BOTH ORIENTATIONS OF THE SAME SURFACE.  Both
 * boards hand over 320 x 240 today; a function that confused w with h would
 * pass on a square surface and on any case that stays away from the edges, so
 * each case is also run on 240 x 320 with the rectangle transposed.
 */
#include "plugin_paint_span.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int failures;

static void ok(const char *what, int cond)
{
	if (!cond) {
		printf("  FAIL %s\n", what);
		failures++;
	} else {
		printf("  ok   %s\n", what);
	}
}

/* The allowance both boards give one draw(): a quarter of a 320 x 240 frame
 * and 64 primitives (NN_OV_DRAW_* / PREVIEW_PLUGIN_DRAW_*). */
#define LIMIT 19200u
#define OPS   64u

static struct plugin_paint_budget fresh(void)
{
	struct plugin_paint_budget b;

	b.pixels  = LIMIT;
	b.ops     = OPS;
	b.refused = 0u;
	return b;
}

/* One orientation: surface w x h, and whether rectangles are transposed. */
struct orient {
	const char *name;
	uint16_t w, h;
	int swap;
};

static const struct orient orients[] = {
	{ "320x240", 320u, 240u, 0 },
	{ "240x320", 240u, 320u, 1 },
};

static struct plugin_rect R(const struct orient *o, int32_t x0, int32_t y0,
                            int32_t x1, int32_t y1)
{
	struct plugin_rect r;

	if (o->swap) {
		r.x0 = y0; r.y0 = x0; r.x1 = y1; r.y1 = x1;
	} else {
		r.x0 = x0; r.y0 = y0; r.x1 = x1; r.y1 = y1;
	}
	return r;
}

static int box_is(const struct orient *o, const struct plugin_paint_box *b,
                  int32_t x0, int32_t y0, int32_t x1, int32_t y1)
{
	struct plugin_rect want = R(o, x0, y0, x1, y1);

	return b->x0 == want.x0 && b->y0 == want.y0 &&
	       b->x1 == want.x1 && b->y1 == want.y1;
}

static void test_clip(const struct orient *o)
{
	struct plugin_paint_box b;
	struct plugin_rect r;
	char what[128];

	printf("clip, %s:\n", o->name);

	r = R(o, 10, 20, 30, 40);
	snprintf(what, sizeof what, "inside the surface it is unchanged (%s)",
	         o->name);
	ok(what, plugin_paint_clip(o->w, o->h, &r, &b) &&
	         box_is(o, &b, 10, 20, 30, 40));

	r = R(o, -5, -7, 330, 250);
	snprintf(what, sizeof what, "past every edge it stops AT the edge, "
	         "half-open (%s)", o->name);
	ok(what, plugin_paint_clip(o->w, o->h, &r, &b) &&
	         box_is(o, &b, 0, 0, 320, 240));

	r = R(o, 319, 239, 400, 400);
	snprintf(what, sizeof what, "the last pixel is on the surface (%s)",
	         o->name);
	ok(what, plugin_paint_clip(o->w, o->h, &r, &b) &&
	         box_is(o, &b, 319, 239, 320, 240));

	r = R(o, 320, 0, 330, 10);
	snprintf(what, sizeof what, "one column past the long edge is nothing "
	         "(%s)", o->name);
	ok(what, !plugin_paint_clip(o->w, o->h, &r, &b));

	r = R(o, 0, 240, 10, 250);
	snprintf(what, sizeof what, "one row past the short edge is nothing "
	         "(%s)", o->name);
	ok(what, !plugin_paint_clip(o->w, o->h, &r, &b));

	r = R(o, 30, 20, 10, 40);
	snprintf(what, sizeof what, "an inverted rectangle is nothing (%s)",
	         o->name);
	ok(what, !plugin_paint_clip(o->w, o->h, &r, &b));

	r = R(o, 10, 20, 10, 40);
	snprintf(what, sizeof what, "an empty one is nothing (%s)", o->name);
	ok(what, !plugin_paint_clip(o->w, o->h, &r, &b));

	r = R(o, INT32_MIN, INT32_MIN, INT32_MAX, INT32_MAX);
	snprintf(what, sizeof what, "the widest coordinates clip to the "
	         "surface (%s)", o->name);
	ok(what, plugin_paint_clip(o->w, o->h, &r, &b) &&
	         box_is(o, &b, 0, 0, 320, 240));
}

static void test_fill(const struct orient *o)
{
	struct plugin_paint_budget bud;
	struct plugin_paint_box b;
	struct plugin_rect r;
	char what[128];

	printf("fill, %s:\n", o->name);

	bud = fresh();
	r = R(o, -10, -10, 20, 5);
	snprintf(what, sizeof what, "charges the clipped area and one dispatch "
	         "(%s)", o->name);
	ok(what, plugin_paint_fill_begin(&bud, o->w, o->h, &r, &b) &&
	         box_is(o, &b, 0, 0, 20, 5) &&
	         bud.pixels == LIMIT - 100u && bud.ops == OPS - 1u &&
	         bud.refused == 0u);

	bud = fresh();
	r = R(o, 400, 400, 410, 410);
	snprintf(what, sizeof what, "[!] one that clips away still costs a "
	         "dispatch, and no pixels (%s)", o->name);
	ok(what, !plugin_paint_fill_begin(&bud, o->w, o->h, &r, &b) &&
	         bud.pixels == LIMIT && bud.ops == OPS - 1u && bud.refused == 0u);

	bud = fresh();
	snprintf(what, sizeof what, "and so does a NULL rectangle (%s)", o->name);
	ok(what, !plugin_paint_fill_begin(&bud, o->w, o->h, NULL, &b) &&
	         bud.pixels == LIMIT && bud.ops == OPS - 1u);

	bud = fresh();
	r = R(o, 0, 0, 320, 240);
	snprintf(what, sizeof what, "[!] a whole frame is over the cap: refused, "
	         "nothing deducted (%s)", o->name);
	ok(what, !plugin_paint_fill_begin(&bud, o->w, o->h, &r, &b) &&
	         bud.pixels == LIMIT && bud.ops == OPS && bud.refused == 1u);
}

static void test_blit(const struct orient *o)
{
	struct plugin_paint_budget bud;
	struct plugin_paint_blit_span sp;
	struct plugin_rect r;
	char what[128];
	uint32_t sx, sy, cols, rows;

	printf("blit, %s:\n", o->name);

	/* A 16 x 8 source whose origin is 5 columns left of and 3 rows above the
	 * surface: the part that survives starts 5 across and 3 down in it. */
	bud = fresh();
	r = R(o, -5, -3, 11, 5);
	sx = o->swap ? 3u : 5u;  sy = o->swap ? 5u : 3u;
	cols = o->swap ? 5u : 11u;  rows = o->swap ? 11u : 5u;
	snprintf(what, sizeof what, "the source offset moves with the clip "
	         "(%s)", o->name);
	ok(what, plugin_paint_blit_begin(&bud, o->w, o->h, &r, 16u, &sp) &&
	         sp.x0 == 0 && sp.y0 == 0 && sp.sx0 == sx && sp.sy0 == sy &&
	         sp.cols == cols && sp.rows == rows);
	snprintf(what, sizeof what, "[!] and every source pixel read is "
	         "charged (%s)", o->name);
	ok(what, bud.pixels == LIMIT - 55u && bud.ops == OPS - 1u);

	bud = fresh();
	r = R(o, 300, 230, 340, 250);
	sx = 0u; sy = 0u;
	cols = o->swap ? 10u : 20u;  rows = o->swap ? 20u : 10u;
	snprintf(what, sizeof what, "clipped at the far edges, the offset is "
	         "zero and the extent shrinks (%s)", o->name);
	ok(what, plugin_paint_blit_begin(&bud, o->w, o->h, &r, 40u, &sp) &&
	         sp.sx0 == sx && sp.sy0 == sy && sp.cols == cols &&
	         sp.rows == rows &&
	         sp.x0 == (o->swap ? 230 : 300) && sp.y0 == (o->swap ? 300 : 230));

	/* [!] r->x0 = INT32_MIN: `x0 - r->x0` in 32 bits is undefined.  The
	 * offset is 2^31 exactly, which only the widened subtraction produces. */
	bud = fresh();
	r = R(o, INT32_MIN, 0, 4, 2);
	snprintf(what, sizeof what, "[!] an origin at INT32_MIN gives the offset "
	         "2^31, not a wrapped one (%s)", o->name);
	ok(what, plugin_paint_blit_begin(&bud, o->w, o->h, &r, 1u, &sp) &&
	         (o->swap ? sp.sy0 : sp.sx0) == 0x80000000u &&
	         (o->swap ? sp.sx0 : sp.sy0) == 0u);

	bud = fresh();
	r = R(o, 0, 0, 4, 4);
	snprintf(what, sizeof what, "a zero stride is nothing to draw, and costs "
	         "a dispatch (%s)", o->name);
	ok(what, !plugin_paint_blit_begin(&bud, o->w, o->h, &r, 0u, &sp) &&
	         bud.pixels == LIMIT && bud.ops == OPS - 1u);

	bud = fresh();
	r = R(o, -40, -40, -20, -20);
	snprintf(what, sizeof what, "so is a source wholly off the surface "
	         "(%s)", o->name);
	ok(what, !plugin_paint_blit_begin(&bud, o->w, o->h, &r, 20u, &sp) &&
	         bud.pixels == LIMIT && bud.ops == OPS - 1u);

	bud = fresh();
	bud.pixels = 15u;
	r = R(o, 0, 0, 4, 4);
	snprintf(what, sizeof what, "one pixel short is refused, nothing "
	         "deducted (%s)", o->name);
	ok(what, !plugin_paint_blit_begin(&bud, o->w, o->h, &r, 4u, &sp) &&
	         bud.pixels == 15u && bud.ops == OPS && bud.refused == 1u);
}

static void test_rect(const struct orient *o)
{
	struct plugin_paint_budget bud;
	struct rect_geom g;
	struct plugin_rect r;
	char what[128];

	printf("rect, %s:\n", o->name);

	/* A 10 x 6 outline, stroke 2: the top and bottom bands store 2 x 10 each
	 * and the two side bands 2 x 2 each over the middle two rows -- 48. */
	bud = fresh();
	r = R(o, 5, 5, 15, 11);
	snprintf(what, sizeof what, "[!] an outline is charged what it stores, "
	         "not the 60 it encloses (%s)", o->name);
	ok(what, plugin_paint_rect_begin(&bud, o->w, o->h, &r, 2u, &g) &&
	         bud.pixels == LIMIT - 48u && bud.ops == OPS - 1u &&
	         rect_geom_writes(&g) == 48u);

	bud = fresh();
	r = R(o, 5, 5, 15, 11);
	snprintf(what, sizeof what, "a stroke of zero is nothing, and costs a "
	         "dispatch (%s)", o->name);
	ok(what, !plugin_paint_rect_begin(&bud, o->w, o->h, &r, 0u, &g) &&
	         bud.pixels == LIMIT && bud.ops == OPS - 1u);

	bud = fresh();
	snprintf(what, sizeof what, "so is a NULL rectangle (%s)", o->name);
	ok(what, !plugin_paint_rect_begin(&bud, o->w, o->h, NULL, 1u, &g) &&
	         bud.pixels == LIMIT && bud.ops == OPS - 1u);

	bud = fresh();
	bud.ops = 0u;
	r = R(o, 5, 5, 15, 11);
	snprintf(what, sizeof what, "with no dispatches left it is refused "
	         "(%s)", o->name);
	ok(what, !plugin_paint_rect_begin(&bud, o->w, o->h, &r, 1u, &g) &&
	         bud.pixels == LIMIT && bud.refused == 1u);
}

static void dummy_rect(void *ctx, const struct plugin_rect *r, uint16_t c,
                       uint16_t s)
{
	(void)ctx; (void)r; (void)c; (void)s;
}

static void dummy_fill(void *ctx, const struct plugin_rect *r, uint16_t c)
{
	(void)ctx; (void)r; (void)c;
}

static void dummy_blit(void *ctx, const struct plugin_rect *r,
                       const uint16_t *src, uint32_t stride, int32_t key)
{
	(void)ctx; (void)r; (void)src; (void)stride; (void)key;
}

static void test_vtable(void)
{
	struct plugin_painter p;
	int ctx;

	printf("the painter:\n");
	memset(&p, 0xA5, sizeof p);
	plugin_paint_vtable(&p, &ctx, dummy_rect, dummy_fill, dummy_blit);
	ok("[!] version and size are filled -- the veneer refuses a painter "
	   "without them",
	   p.version == PLUGIN_ABI_VERSION && p.size == sizeof(p));
	ok("and so are the context and all three primitives",
	   p.ctx == &ctx && p.rect == dummy_rect && p.fill_rect == dummy_fill &&
	   p.blit == dummy_blit);
	plugin_paint_vtable(NULL, &ctx, dummy_rect, dummy_fill, dummy_blit);
	ok("a NULL painter is ignored", 1);
}

int main(void)
{
	size_t i;

	printf("test_plugin_paint_span (svc/plugin_paint_span.c):\n");
	for (i = 0; i < sizeof orients / sizeof orients[0]; i++) {
		test_clip(&orients[i]);
		test_fill(&orients[i]);
		test_blit(&orients[i]);
		test_rect(&orients[i]);
	}
	test_vtable();

	if (failures) {
		printf("test_plugin_paint_span: %d FAILED\n", failures);
		return 1;
	}
	printf("test_plugin_paint_span: all passed\n");
	return 0;
}
