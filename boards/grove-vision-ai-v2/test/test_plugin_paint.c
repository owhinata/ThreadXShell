/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Host unit test for port/plugin/plugin_paint.c (issue #103).
 *
 * The subject is the BUDGET and the CLIPPING, not the drawing.  A plugin's
 * draw() runs on the panel thread with the panel guard held and everything else
 * that wants the panel failing its non-blocking acquire meanwhile, so what the
 * base does on a plugin's behalf has to be bounded -- and bounded in a way that
 * a nonsense rectangle cannot talk its way past.
 *
 * [!] lcd_rect_wire() IS STUBBED HERE, deliberately.  It is the driver's own
 * pure primitive with its own clipping and stroke rules, tested where it lives;
 * pulling lcd_st7789.c onto the host would drag in the SPI driver for no gain.
 * What this file must see is whether the PAINTER charged, refused, or let
 * something through -- so the stub records that it was reached.
 */
#include "plugin_paint.h"

#include <stdio.h>
#include <string.h>

#define W 32u
#define H 16u

static uint16_t fb[W * H];
static unsigned rect_calls;
static int failures;

/* The stub.  See the header comment. */
void lcd_rect_wire(uint16_t *f, uint16_t fw, uint16_t fh,
                   int32_t x0, int32_t y0, int32_t x1, int32_t y1,
                   uint16_t rgb565, uint16_t stroke)
{
	(void)f; (void)fw; (void)fh; (void)x0; (void)y0; (void)x1; (void)y1;
	(void)rgb565; (void)stroke;
	rect_calls++;
}

static void ok(const char *what, int cond)
{
	if (!cond) {
		printf("  FAIL %s\n", what);
		failures++;
	} else {
		printf("  ok   %s\n", what);
	}
}

static struct plugin_painter p;
static struct plugin_paint_budget bud;

static void setup(uint32_t pixels, uint32_t ops)
{
	memset(fb, 0, sizeof fb);
	rect_calls = 0;
	bud.pixels = pixels;
	bud.ops    = ops;
	bud.refused = 0u;
	plugin_paint_bind(&p, &bud, fb, (uint16_t)W, (uint16_t)H);
}

static int fb_is_clear(void)
{
	unsigned i;

	for (i = 0u; i < W * H; i++)
		if (fb[i] != 0u)
			return 0;
	return 1;
}

static void test_charging(void)
{
	struct plugin_rect r = { 0, 0, 4, 4 };

	printf(" case: charging\n");

	setup(1000u, 10u);
	p.fill_rect(p.ctx, &r, 0xFFFFu);
	ok("a 4x4 fill costs 16 pixels and one op",
	   bud.pixels == 1000u - 16u && bud.ops == 9u);
	ok("and it drew", !fb_is_clear());

	/*
	 * [!] A REFUSED PRIMITIVE MUST LEAVE THE FRAMEBUFFER ALONE.  Half a box is
	 * worse than none: it reads as a rendering bug rather than as a budget, and
	 * the operator would go looking in the wrong place.
	 */
	setup(8u, 10u);
	p.fill_rect(p.ctx, &r, 0xFFFFu);
	ok("a fill that does not fit the budget is refused", bud.refused == 1u);
	ok("and it drew nothing at all", fb_is_clear());
	ok("and it spent nothing", bud.pixels == 8u && bud.ops == 10u);

	/* The dispatch itself costs, even when the rectangle clips away. */
	{
		struct plugin_rect away = { 100, 100, 110, 110 };

		setup(1000u, 2u);
		p.fill_rect(p.ctx, &away, 0xFFFFu);
		ok("a rectangle entirely off-screen still costs a dispatch",
		   bud.ops == 1u);
		ok("and draws nothing", fb_is_clear());
	}

	setup(1000u, 1u);
	p.fill_rect(p.ctx, &r, 0xFFFFu);
	p.fill_rect(p.ctx, &r, 0xFFFFu);
	ok("the op count runs out independently of the pixels",
	   bud.ops == 0u && bud.refused == 1u);
}

static void test_clipping(void)
{
	printf(" case: clipping\n");

	/* Straddling every edge, including negative coordinates -- a detection
	 * routinely runs past the edge of the image it was found in. */
	{
		struct plugin_rect r = { -4, -4, 4, 4 };

		setup(1000u, 10u);
		p.fill_rect(p.ctx, &r, 0xFFFFu);
		ok("a rectangle starting off the top-left is clipped, and charged for "
		   "what survives", bud.pixels == 1000u - 16u);
	}
	{
		struct plugin_rect r = { (int32_t)W - 2, (int32_t)H - 2,
		                         (int32_t)W + 40, (int32_t)H + 40 };

		setup(1000u, 10u);
		p.fill_rect(p.ctx, &r, 0xFFFFu);
		ok("and one running off the bottom-right likewise",
		   bud.pixels == 1000u - 4u);
	}
	{
		/*
		 * [!] THE ADDITION THAT WOULD WRAP.  A rectangle whose corners are near
		 * the extremes is what makes `spent + cost <= limit` overflow and pass;
		 * the charge is written `cost <= remaining` so it cannot.  Clipping
		 * bounds the cost to the framebuffer either way, which is the point:
		 * the numbers that reach the arithmetic are already sane.
		 */
		struct plugin_rect r = { -2000000000, -2000000000,
		                         2000000000, 2000000000 };

		setup(1000u, 10u);
		p.fill_rect(p.ctx, &r, 0xFFFFu);
		ok("an absurd rectangle is clipped to the framebuffer, not wrapped",
		   bud.pixels == 1000u - (W * H));
	}
	{
		struct plugin_rect inverted = { 8, 8, 4, 4 };

		setup(1000u, 10u);
		p.fill_rect(p.ctx, &inverted, 0xFFFFu);
		ok("an inverted rectangle draws nothing", fb_is_clear());
	}
}

static void test_blit(void)
{
	uint16_t src[16];
	struct plugin_rect r = { 0, 0, 4, 4 };
	unsigned i;

	printf(" case: blit and the colour key\n");

	for (i = 0u; i < 16u; i++)
		src[i] = (uint16_t)(i == 0u ? 0x1234u : 0xABCDu);

	setup(1000u, 10u);
	p.blit(p.ctx, &r, src, 4u, -1);
	ok("an opaque blit charges every pixel", bud.pixels == 1000u - 16u);
	ok("and wrote the first pixel byte-swapped", fb[0] == 0x3412u);

	/*
	 * [!] THE TRANSPARENT PIXELS ARE CHARGED TOO.  The budget bounds time spent
	 * with the guard held, and a keyed pixel costs a read and a compare whether
	 * or not it is written.  Charging only what lands would let a
	 * mostly-transparent bitmap of any size through for almost nothing -- which
	 * is precisely the shape someone would reach for to draw a full-screen
	 * overlay.
	 */
	setup(1000u, 10u);
	p.blit(p.ctx, &r, src, 4u, 0xABCD);
	ok("a mostly-transparent blit costs the same as an opaque one",
	   bud.pixels == 1000u - 16u);
	ok("the keyed pixels were not written", fb[1] == 0u);
	ok("the un-keyed one was", fb[0] == 0x3412u);

	setup(1000u, 10u);
	p.blit(p.ctx, &r, NULL, 4u, -1);
	ok("a NULL source draws nothing and charges nothing",
	   fb_is_clear() && bud.ops == 10u);

	setup(1000u, 10u);
	p.blit(p.ctx, &r, src, 0u, -1);
	ok("a zero stride is refused", fb_is_clear() && bud.ops == 9u);

	/* Clipped blit: the source origin moves with the clip, so the visible part
	 * comes from the right place rather than from the source's corner. */
	{
		struct plugin_rect off = { -2, -2, 2, 2 };

		setup(1000u, 10u);
		p.blit(p.ctx, &off, src, 4u, -1);
		ok("a clipped blit charges only the visible part",
		   bud.pixels == 1000u - 4u);
		ok("and takes its pixels from the matching part of the source",
		   fb[0] == (uint16_t)((src[2 * 4 + 2] >> 8) | (src[2 * 4 + 2] << 8)));
	}
}

static void test_rect(void)
{
	struct plugin_rect r = { 0, 0, 8, 8 };

	printf(" case: rect reaches the driver's own primitive\n");

	setup(1000u, 10u);
	p.rect(p.ctx, &r, 0x07E0u, 2u);
	ok("an outline that fits is drawn", rect_calls == 1u);
	ok("and is charged its enclosing area", bud.pixels == 1000u - 64u);

	setup(8u, 10u);
	p.rect(p.ctx, &r, 0x07E0u, 2u);
	ok("an outline that does not fit never reaches the driver",
	   rect_calls == 0u && bud.refused == 1u);
}

int main(void)
{
	printf("test_plugin_paint (port/plugin/plugin_paint.c):\n");
	test_charging();
	test_clipping();
	test_blit();
	test_rect();

	if (failures != 0) {
		printf("test_plugin_paint: %d failure(s)\n", failures);
		return 1;
	}
	printf("test_plugin_paint: all cases pass\n");
	return 0;
}
