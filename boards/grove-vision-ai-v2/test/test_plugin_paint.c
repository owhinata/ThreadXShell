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
 * [!] AND SINCE ISSUE #105, lcd_rect_wire() IS THE REAL ONE.  It used to be
 * stubbed with a call counter -- reasonably, while the painter charged a number
 * of its own devising, and unavoidably, because it lived in lcd_st7789.c which
 * drags the SPI driver onto the host.  Then the charge became a claim ABOUT that
 * loop: an outline costs the pixels it WRITES, not the area it encloses.  A test
 * that replaced the loop could not check that claim, and the claim is the whole
 * reason a close-up face gets a box at all (40,000 > the 19,200 cap).
 *
 * So the primitive moved to lcd_rect.c, this links it, and the comparison is
 * between two independently produced numbers:
 *
 *   - what the REAL loop stored, counted one store at a time through a seam
 *     inside the driver (LCD_RECT_COUNT_STORES).  Counting stores and not
 *     touched pixels is the point: on an odd, narrow rectangle the clamped
 *     stroke makes the side bands overlap and that column is written twice, and
 *     that second write is real time under the panel guard;
 *   - what the painter deducted from the budget.
 *
 * They share the geometry rule -- one clip, one stroke clamp -- and nothing
 * else.  Golden numbers written out by hand below cover the shared part, so a
 * wrong edit to the rule cannot move both sides and pass.
 */
#include "plugin_paint.h"

#include <stdio.h>
#include <string.h>

#define W 32u
#define H 16u

static uint16_t fb[W * H];
static int failures;

/* The counter lcd_rect.c increments, one per store, when built with
 * LCD_RECT_COUNT_STORES.  Defined here so the driver TU stays free of test
 * scaffolding it would otherwise carry into the firmware. */
unsigned long lcd_rect_stores;

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
	lcd_rect_stores = 0ul;
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

/*
 * The outline: what it charged against what the loop stored.
 *
 * Every case names the two numbers separately.  `spent` comes from the budget
 * the painter deducted; `lcd_rect_stores` comes from the driver's own store
 * seam.  A charge computed from the wrong rule fails here even though both sides
 * share the clip, because only one side was produced by running the loop.
 */
static uint32_t spent(uint32_t started)
{
	return started - bud.pixels;
}

static void test_rect(void)
{
	printf(" case: an outline is charged what it writes\n");

	{
		struct plugin_rect r = { 0, 0, 8, 8 };

		setup(1000u, 10u);
		p.rect(p.ctx, &r, 0x07E0u, 2u);
		/* Golden, by hand: 8x8, stroke 2.  Four edge rows of 8 = 32, four
		 * interior rows of 2+2 = 16.  NOT 64, which is what charging the
		 * enclosing area used to cost. */
		ok("an 8x8 stroke-2 outline costs 48, not its 64-pixel area",
		   spent(1000u) == 48u);
		ok("and that is exactly what the loop stored",
		   lcd_rect_stores == spent(1000u));
	}

	/*
	 * [!] THE CASE THE OLD CHARGE GOT WRONG IN THE DIRECTION THAT MATTERED.  A
	 * close-up face fills the frame, and its enclosing area is more than the
	 * whole per-frame cap -- so the box was refused and the operator saw a face
	 * with nothing drawn round it.  Its outline is a few hundred pixels.
	 */
	{
		struct plugin_rect r = { 0, 0, 200, 200 };

		setup(19200u, 64u);
		p.rect(p.ctx, &r, 0x07E0u, 2u);
		ok("a face-sized box is drawn rather than refused", bud.refused == 0u);
		ok("and costs far less than its area", spent(19200u) < 4000u);
		ok("and matches the loop", lcd_rect_stores == spent(19200u));
	}

	/*
	 * [!] THE OVERLAP.  Width 5 with a stroke of 3 clamps to t = (5+1)/2 = 3,
	 * and the two side bands then cover columns 0..2 and 2..4: column 2 is
	 * stored TWICE on every interior row.  A charge of "perimeter x stroke,
	 * without double counting" -- which is what a first reading of the loop
	 * suggests -- under-charges here, and this is the case that says so.
	 */
	{
		struct plugin_rect r = { 0, 0, 5, 9 };

		setup(1000u, 10u);
		p.rect(p.ctx, &r, 0x07E0u, 3u);
		/* Golden: t clamps to 3 on x, then min(9/2, 3) leaves 3 on y.  Edge
		 * rows = min(9, 6) = 6, at 5 each = 30; 3 interior rows at 2*3 = 6
		 * each = 18. */
		ok("an odd narrow box charges the doubly-written column twice",
		   spent(1000u) == 48u);
		ok("and the loop really did store that many times",
		   lcd_rect_stores == spent(1000u));
	}

	/*
	 * A single row and a single column, both with a stroke far larger than
	 * they are.  The clamp brings t to 1 in each -- but the two are NOT
	 * symmetric, and the asymmetry is the overlap again:
	 *
	 *   10x1: every row is an edge row, so it is 1 row of 10 = 10 stores;
	 *   1x10: 2 edge rows of 1 = 2, and 8 interior rows that write the left
	 *         band and the right band, which are THE SAME COLUMN -- 2 stores
	 *         each = 16.  Eighteen, not ten.
	 *
	 * Writing the two out separately is the point: "a 1-pixel line costs its
	 * length" is the intuitive answer and it is wrong in one of the two
	 * orientations.
	 */
	{
		struct plugin_rect row = { 0, 0, 10, 1 };
		struct plugin_rect col = { 0, 0, 1, 10 };

		setup(1000u, 10u);
		p.rect(p.ctx, &row, 0x07E0u, 4u);
		ok("a 1-pixel-tall box is one solid row: 10",
		   spent(1000u) == 10u && lcd_rect_stores == 10ul);

		setup(1000u, 10u);
		p.rect(p.ctx, &col, 0x07E0u, 4u);
		ok("a 1-pixel-wide box writes its interior column twice: 18",
		   spent(1000u) == 18u && lcd_rect_stores == 18ul);
	}

	/*
	 * [!] STROKE ZERO DRAWS NOTHING, AND THE CHARGE HAS TO AGREE.  The driver
	 * returns before it clips and before it raises a zero stroke to one, so the
	 * honest cost is no pixels at all -- and one dispatch, because the call
	 * still happened.
	 */
	{
		struct plugin_rect r = { 0, 0, 8, 8 };

		setup(1000u, 10u);
		p.rect(p.ctx, &r, 0x07E0u, 0u);
		ok("a zero stroke costs no pixels and one op",
		   spent(1000u) == 0u && bud.ops == 9u);
		ok("and stored nothing", lcd_rect_stores == 0ul && fb_is_clear());
	}

	/* Clipped away, inverted, and the extremes: no stores, one dispatch each. */
	{
		struct plugin_rect away = { 100, 100, 110, 110 };
		struct plugin_rect inv  = { 8, 8, 4, 4 };
		struct plugin_rect huge = { -2147483647 - 1, -2147483647 - 1,
		                            2147483647, 2147483647 };

		setup(1000u, 10u);
		p.rect(p.ctx, &away, 0x07E0u, 2u);
		ok("an off-screen outline stores nothing and costs a dispatch",
		   lcd_rect_stores == 0ul && bud.ops == 9u && spent(1000u) == 0u);

		setup(1000u, 10u);
		p.rect(p.ctx, &inv, 0x07E0u, 2u);
		ok("an inverted outline likewise",
		   lcd_rect_stores == 0ul && spent(1000u) == 0u);

		/* INT32_MIN..INT32_MAX clips to the whole framebuffer.  The stroke
		 * clamps against the CLIPPED extents, so this is an ordinary box. */
		setup(100000u, 10u);
		p.rect(p.ctx, &huge, 0x07E0u, 2u);
		ok("the extremes clip to the framebuffer and are charged what they store",
		   lcd_rect_stores == spent(100000u) && spent(100000u) != 0u);
	}

	{
		struct plugin_rect r = { 0, 0, 8, 8 };

		setup(8u, 10u);
		p.rect(p.ctx, &r, 0x07E0u, 2u);
		ok("an outline that does not fit never reaches the driver",
		   lcd_rect_stores == 0ul && bud.refused == 1u && fb_is_clear());
	}
}

/*
 * The composite case: eight boxes and eight labels, deterministic.
 *
 * [!] THIS IS THE PROOF, AND HARDWARE IS THE SMOKE TEST.  On the board the
 * number of faces, their size and the width of each score is whatever happened
 * to be in front of the camera, so observing `refused == 0` there says the scene
 * was easy and nothing more.  Here the inventory is fixed: BF_MAX_DET boxes and
 * the label chip plugin/blazeface blits beside each one.
 *
 * Two cases, because "it always fits" is not true and claiming it would be the
 * kind of comfortable statement this project keeps having to retract:
 *
 *   - the realistic one -- eight half-frame boxes -- fits with room to spare;
 *   - the pathological one -- eight boxes each filling the WHOLE frame, which
 *     NMS could not actually produce -- does not, and the point is that it
 *     refuses CLEANLY: the primitive that does not fit draws nothing, the ones
 *     before it are untouched, and the refusal is counted where `nn stream
 *     stats` will show it.
 */
#define WORST_BOXES  8u
#define WORST_LBL_W  24u    /* PL_TEXT_CELL_W * 4 chars at scale 1 */
#define WORST_LBL_H  8u
#define FRAME_W      320u
#define FRAME_H      240u
#define DRAW_PIXELS  (FRAME_W * FRAME_H / 4u)   /* NN_OV_DRAW_PIXELS */
#define DRAW_OPS     64u                        /* NN_OV_DRAW_OPS    */

static uint16_t big_fb[FRAME_W * FRAME_H];
static uint16_t chip[WORST_LBL_W * WORST_LBL_H];
static struct plugin_paint_budget wb;
static struct plugin_painter wp;

/* Boxes and chips, alternating, exactly as plugin/blazeface's draw() does. */
static void run_boxes(uint32_t w, uint32_t h)
{
	unsigned i;

	memset(big_fb, 0, sizeof big_fb);
	memset(chip, 0, sizeof chip);
	wb.pixels  = DRAW_PIXELS;
	wb.ops     = DRAW_OPS;
	wb.refused = 0u;
	plugin_paint_bind(&wp, &wb, big_fb, (uint16_t)FRAME_W, (uint16_t)FRAME_H);

	for (i = 0u; i < WORST_BOXES; i++) {
		struct plugin_rect box = { 0, 0, (int32_t)w, (int32_t)h };
		struct plugin_rect lbl = { 2, 2, 2 + (int32_t)WORST_LBL_W,
		                           2 + (int32_t)WORST_LBL_H };

		wp.rect(wp.ctx, &box, 0x07E0u, 2u);
		wp.blit(wp.ctx, &lbl, chip, WORST_LBL_W, -1);
	}
}

static void test_worst_case(void)
{
	/* Golden, by hand.  A w x h outline at stroke 2 stores 4 edge rows of w
	 * plus (h - 4) interior rows of 4; a chip is 24 x 8. */
	const uint32_t half = 4u * 160u + (120u - 4u) * 4u;   /* 1,104 */
	const uint32_t full = 4u * 320u + (240u - 4u) * 4u;   /* 2,224 */
	const uint32_t lbl  = WORST_LBL_W * WORST_LBL_H;      /*   192 */

	printf(" case: eight boxes and eight labels\n");

	run_boxes(FRAME_W / 2u, FRAME_H / 2u);
	ok("half-frame boxes: nothing refused", wb.refused == 0u);
	ok("all sixteen primitives ran", wb.ops == DRAW_OPS - 2u * WORST_BOXES);
	ok("and the spend is the sum of the parts",
	   DRAW_PIXELS - wb.pixels == WORST_BOXES * (half + lbl));

	/*
	 * Full-frame boxes overrun.  Seven pairs cost 7 * (2,224 + 192) = 16,912,
	 * leaving 2,288; the eighth box costs 2,224 and fits, leaving 64; its chip
	 * costs 192 and does not.  A refusal consumes no op, so fifteen ran.
	 */
	run_boxes(FRAME_W, FRAME_H);
	ok("full-frame boxes: exactly one primitive is refused", wb.refused == 1u);
	ok("fifteen ran, and the refused one cost no op",
	   wb.ops == DRAW_OPS - 15u);
	ok("the budget stopped where the arithmetic says",
	   wb.pixels == DRAW_PIXELS - (WORST_BOXES * full + 7u * lbl));
	printf("       (half-frame %lu of %u, full-frame %lu of %u)\n",
	       (unsigned long)(WORST_BOXES * (half + lbl)), DRAW_PIXELS,
	       (unsigned long)(WORST_BOXES * full + 7u * lbl), DRAW_PIXELS);
}

int main(void)
{
	printf("test_plugin_paint (port/plugin/plugin_paint.c):\n");
	test_charging();
	test_clipping();
	test_blit();
	test_rect();
	test_worst_case();

	if (failures != 0) {
		printf("test_plugin_paint: %d failure(s)\n", failures);
		return 1;
	}
	printf("test_plugin_paint: all cases pass\n");
	return 0;
}
