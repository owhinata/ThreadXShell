/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Host tests for port/plugin/plugin_paint.c (issue #110 = #78 Step 3b).
 *
 * A plugin's draw() runs on the panel thread with the frame lock held, and
 * everything else that wants the panel is waiting meanwhile, so the work the
 * BASE does on a plugin's behalf is capped.  What is tested is whether the cap
 * can be talked past: an absurd rectangle, a zero stride, a mostly-transparent
 * blit, and a refused primitive leaving the framebuffer untouched rather than
 * half drawn.
 *
 * [!] THE COUNT COMES FROM THE LOOP, NOT FROM THE FORMULA.  The charge is a
 * claim ABOUT these loops -- "the budget decrement equals the pixels they
 * write" -- so a test that compared the charge against rect_geom_writes()
 * would be checking one side against itself.  plugin_paint.c is built here
 * with PLUGIN_PAINT_COUNT_STORES so every store is counted where it happens,
 * and golden numbers are pinned beside that, so the two sides are not one side
 * twice.
 *
 * [!] AND THE GOLDENS ARE THE OTHER BOARD'S.  rect_geom.c is shared, so an
 * 8x8 outline of stroke 2 must cost 48 here exactly as it does there.  If the
 * two boards' loops ever disagree about that, one of them has stopped
 * following the rule they both borrow.
 */
#include "plugin_paint.h"

#include <stdio.h>
#include <string.h>

/* The seam's counter.  Defined HERE so no test scaffolding lands in the
 * driver's translation unit. */
unsigned long plugin_paint_stores;

static int fails;

#define CHECK(cond, what)                                                     \
	do {                                                                      \
		if (cond) {                                                           \
			printf("  ok   %s\n", (what));                                    \
		} else {                                                              \
			printf("  FAIL %s  (%s:%d)\n", (what), __FILE__, __LINE__);       \
			fails++;                                                          \
		}                                                                     \
	} while (0)

/* The real surface: landscape 320x240 over a portrait 240x320 buffer. */
#define SW 320
#define SH 240

static uint16_t fb[SW * SH];
static struct plugin_painter paint;
static struct plugin_paint_budget bud;

#define LIMIT 19200u      /* a quarter frame, as the other board budgets */
#define OPS   64u

static void arm(uint32_t pixels, uint32_t ops)
{
	memset(fb, 0, sizeof fb);
	bud.pixels  = pixels;
	bud.ops     = ops;
	bud.refused = 0u;
	plugin_paint_stores = 0ul;
	plugin_paint_bind(&paint, &bud, fb, SW, SH);
}

static uint32_t spent(uint32_t from)
{
	return from - bud.pixels;
}

static int fb_is_clear(void)
{
	size_t i;

	for (i = 0u; i < sizeof fb / sizeof fb[0]; i++)
		if (fb[i] != 0u)
			return 0;
	return 1;
}

/* Where landscape (x, y) must land, stated independently of the driver. */
static uint16_t px_at(int x, int y)
{
	return fb[(size_t)(SW - 1 - x) * SH + (size_t)y];
}

static struct plugin_rect rc(int32_t x0, int32_t y0, int32_t x1, int32_t y1)
{
	struct plugin_rect r = { x0, y0, x1, y1 };

	return r;
}

/* --- cases ---------------------------------------------------------------- */

static void test_geometry_and_rotation(void)
{
	struct plugin_rect r;

	printf("where the pixels land:\n");

	arm(LIMIT, OPS);
	r = rc(10, 20, 13, 24);
	paint.fill_rect(paint.ctx, &r, 0xBEEFu);
	CHECK(spent(LIMIT) == 12u && plugin_paint_stores == 12ul,
	      "a 3x4 fill costs twelve pixels and writes twelve");
	CHECK(px_at(10, 20) == 0xBEEFu && px_at(12, 23) == 0xBEEFu,
	      "[!] both corners land where the rotation says they should");
	CHECK(px_at(9, 20) == 0u && px_at(13, 20) == 0u &&
	              px_at(10, 19) == 0u && px_at(10, 24) == 0u,
	      "and the rectangle is half-open on every side");

	arm(LIMIT, OPS);
	r = rc(0, 0, 1, 1);
	paint.fill_rect(paint.ctx, &r, 0x1234u);
	CHECK(fb[(size_t)(SW - 1) * SH] == 0x1234u,
	      "landscape (0,0) is the LAST frame-buffer row, not the first");

	arm(LIMIT, OPS);
	r = rc(SW - 1, SH - 1, SW, SH);
	paint.fill_rect(paint.ctx, &r, 0x4321u);
	CHECK(fb[(size_t)SH - 1u] == 0x4321u,
	      "and the far corner is the first row's last pixel");
	CHECK(px_at(0, 0) == 0u, "nothing else moved");
}

static void test_outline_goldens(void)
{
	struct plugin_rect r;

	printf("[!] the outline's cost, against the OTHER board's goldens:\n");

	arm(LIMIT, OPS);
	r = rc(4, 4, 12, 12);
	paint.rect(paint.ctx, &r, 0x07E0u, 2u);
	CHECK(plugin_paint_stores == 48ul && spent(LIMIT) == 48u,
	      "8x8 stroke 2 writes 48, not the 64 its area suggests");

	arm(LIMIT, OPS);
	r = rc(0, 0, 5, 9);
	paint.rect(paint.ctx, &r, 0x07E0u, 3u);
	CHECK(plugin_paint_stores == 48ul && spent(LIMIT) == 48u,
	      "[!] 5x9 stroke 3 writes 48 -- the overlapping column is stored "
	      "twice and charged twice");

	arm(LIMIT, OPS);
	r = rc(0, 0, 10, 1);
	paint.rect(paint.ctx, &r, 0x07E0u, 1u);
	CHECK(plugin_paint_stores == 10ul && spent(LIMIT) == 10u,
	      "10x1 writes 10");

	arm(LIMIT, OPS);
	r = rc(0, 0, 1, 10);
	paint.rect(paint.ctx, &r, 0x07E0u, 1u);
	CHECK(plugin_paint_stores == 18ul && spent(LIMIT) == 18u,
	      "[!] but 1x10 writes 18 -- the rule is not symmetric, and the loop "
	      "follows the rule rather than the shape");

	arm(LIMIT, OPS);
	r = rc(4, 4, 12, 12);
	paint.rect(paint.ctx, &r, 0x07E0u, 0u);
	CHECK(plugin_paint_stores == 0ul && spent(LIMIT) == 0u &&
	              bud.ops == OPS - 1u,
	      "a stroke of zero draws nothing and still costs a dispatch");

	/* The close-up face issue #105 found: 200x200 at stroke 2 is 40,000 by
	 * area and would be refused outright, which is how a box disappears. */
	arm(LIMIT, OPS);
	r = rc(20, 20, 220, 220);
	paint.rect(paint.ctx, &r, 0x07E0u, 2u);
	CHECK(plugin_paint_stores == spent(LIMIT) && spent(LIMIT) == 1584u &&
	              bud.refused == 0u,
	      "[!] a 200x200 box costs 1,584 and is drawn -- by area it would be "
	      "40,000 and refused");
}

static void test_clipping(void)
{
	struct plugin_rect r;

	printf("clipping:\n");

	arm(LIMIT, OPS);
	r = rc(-1000, -1000, -900, -900);
	paint.fill_rect(paint.ctx, &r, 0xFFFFu);
	CHECK(plugin_paint_stores == 0ul && fb_is_clear() && bud.ops == OPS - 1u,
	      "a rectangle entirely off-surface draws nothing and costs a "
	      "dispatch");

	arm(LIMIT, OPS);
	r = rc(-4, -4, 4, 4);
	paint.fill_rect(paint.ctx, &r, 0xAAAAu);
	CHECK(spent(LIMIT) == 16u && plugin_paint_stores == 16ul,
	      "one starting off the top-left is charged for what survives");
	CHECK(px_at(0, 0) == 0xAAAAu && px_at(3, 3) == 0xAAAAu &&
	              px_at(4, 4) == 0u,
	      "and lands inside the surface");

	arm(LIMIT, OPS);
	r = rc(INT32_MIN, INT32_MIN, INT32_MAX, INT32_MAX);
	paint.fill_rect(paint.ctx, &r, 0x5555u);
	CHECK(spent(LIMIT) == 0u && bud.refused == 1u && fb_is_clear(),
	      "[!] an absurd rectangle is clipped to the surface -- 76,800 "
	      "pixels -- and then refused, not wrapped into something cheap");

	arm(SW * SH, OPS);
	r = rc(INT32_MIN, INT32_MIN, INT32_MAX, INT32_MAX);
	paint.fill_rect(paint.ctx, &r, 0x5555u);
	CHECK(plugin_paint_stores == (unsigned long)(SW * SH),
	      "and with a budget that allows it, it fills exactly the surface");

	arm(LIMIT, OPS);
	r = rc(10, 10, 5, 5);
	paint.fill_rect(paint.ctx, &r, 0xFFFFu);
	CHECK(plugin_paint_stores == 0ul && fb_is_clear(),
	      "an inverted rectangle draws nothing");
}

static void test_blit(void)
{
	static uint16_t src[8 * 8];
	struct plugin_rect r;
	unsigned i;

	printf("blit:\n");
	for (i = 0u; i < 8u * 8u; i++)
		src[i] = (uint16_t)(0x1000u + i);

	arm(LIMIT, OPS);
	r = rc(100, 50, 108, 58);
	paint.blit(paint.ctx, &r, src, 8u, -1);
	CHECK(spent(LIMIT) == 64u && plugin_paint_stores == 64ul,
	      "an opaque 8x8 blit costs and writes 64");
	CHECK(px_at(100, 50) == 0x1000u && px_at(107, 57) == 0x1000u + 63u,
	      "[!] source (0,0) and (7,7) land at the rectangle's corners, "
	      "transposed and in that order");
	CHECK(px_at(101, 50) == 0x1001u,
	      "and the source's x axis runs along the landscape x axis");

	/* Clipped on the near edges: the source origin has to move with it. */
	arm(LIMIT, OPS);
	r = rc(-3, -2, 5, 6);
	paint.blit(paint.ctx, &r, src, 8u, -1);
	CHECK(spent(LIMIT) == 5u * 6u && plugin_paint_stores == 30ul,
	      "a clipped blit is charged for what survives");
	CHECK(px_at(0, 0) == src[2u * 8u + 3u],
	      "[!] and the surviving source pixel is the right one, not the "
	      "buffer's first");

	arm(LIMIT, OPS);
	r = rc(100, 50, 108, 58);
	paint.blit(paint.ctx, &r, src, 0u, -1);
	CHECK(plugin_paint_stores == 0ul && fb_is_clear() && bud.ops == OPS - 1u,
	      "a zero stride draws nothing and costs a dispatch");

	arm(LIMIT, OPS);
	r = rc(100, 50, 108, 58);
	paint.blit(paint.ctx, &r, NULL, 8u, -1);
	CHECK(plugin_paint_stores == 0ul && bud.ops == OPS,
	      "a null source is not even a dispatch -- there was no request");

	/* Colour-keyed: only one pixel differs from the key, so one store -- and
	 * the charge is still 64. */
	for (i = 0u; i < 8u * 8u; i++)
		src[i] = 0xF81Fu;
	src[9] = 0x07E0u;
	arm(LIMIT, OPS);
	r = rc(100, 50, 108, 58);
	paint.blit(paint.ctx, &r, src, 8u, 0xF81F);
	CHECK(spent(LIMIT) == 64u,
	      "[!] a keyed blit is charged for every source pixel it READS");
	CHECK(plugin_paint_stores == 1ul,
	      "even though it writes one");
	CHECK(px_at(101, 51) == 0x07E0u && px_at(100, 50) == 0u,
	      "and the transparent pixels really were left alone");
}

static void test_budget(void)
{
	struct plugin_rect r;
	unsigned i;

	printf("the budget:\n");

	arm(100u, OPS);
	r = rc(0, 0, 20, 20);
	paint.fill_rect(paint.ctx, &r, 0xFFFFu);
	CHECK(plugin_paint_stores == 0ul && fb_is_clear() && bud.refused == 1u &&
	              bud.pixels == 100u,
	      "[!] a primitive that does not fit draws NOTHING -- half a box looks "
	      "like a rendering bug, not a budget");

	arm(LIMIT, 2u);
	r = rc(0, 0, 2, 2);
	for (i = 0u; i < 4u; i++)
		paint.fill_rect(paint.ctx, &r, 0xFFFFu);
	CHECK(bud.ops == 0u && bud.refused == 2u && plugin_paint_stores == 8ul,
	      "the dispatch count is a cap of its own");

	arm(0u, OPS);
	r = rc(0, 0, 4, 4);
	paint.rect(paint.ctx, &r, 0xFFFFu, 1u);
	CHECK(plugin_paint_stores == 0ul && bud.refused == 1u,
	      "an outline with no pixels left is refused too");
}

int main(void)
{
	printf("test_plugin_paint (port/plugin/plugin_paint.c):\n");
	test_geometry_and_rotation();
	test_outline_goldens();
	test_clipping();
	test_blit();
	test_budget();

	if (fails != 0) {
		printf("test_plugin_paint: %d FAILED\n", fails);
		return 1;
	}
	printf("test_plugin_paint: all cases pass\n");
	return 0;
}
