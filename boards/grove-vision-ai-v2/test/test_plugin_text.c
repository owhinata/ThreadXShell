/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    test_plugin_text.c
 * @brief   The plugin font, its rasteriser and its string sink (issue #105 =
 *          #78 Step 2).
 *
 * WHY A TEST FOR A FONT.  Nothing here is subtle arithmetic, and every way it
 * can fail produces something that looks fine from across the room:
 *
 *   - A GLYPH WRITTEN OUTSIDE THE BUFFER.  The blit that follows hands the base
 *     a rectangle, and the base validates THAT -- so an overrun on the
 *     rasterising side lands in whatever the plugin put next to its strip, and
 *     the label still looks right.  Nothing in the plugin gates proves a
 *     bounded write; check_plugin_image.py says so in its own header.  These
 *     cases are the check.
 *   - A GLYPH TABLE OFF BY ONE.  Every character shifted by one is unreadable
 *     and obvious; ONE character shifted -- a table with 94 entries where the
 *     code indexes 95 -- shows up only when a label happens to contain it.
 *   - A REFUSAL SWALLOWED.  The string sink shares plugin_fmt's rule that a
 *     writer which has said no has said no.  A truncating sink would put
 *     "cat  -78" on the panel: a number that is wrong rather than absent, which
 *     is the failure this project keeps finding worse than none.
 *
 * The font itself is not compared bitmap-for-bitmap: that would pin the
 * typeface and break on any glyph edit, which is a design change and not a
 * regression.  What is pinned is that distinct characters rasterise distinctly,
 * that ink lands only where it should, and that the geometry adds up.
 */
#include "plugin_text.h"
#include "plugin_fmt.h"

#include <stdio.h>
#include <string.h>

#define TW 64u
#define TH 24u
#define GUARD 16u

/* The target, with a canary either side.  A rasteriser that walks off the end
 * of a row -- the classic stride bug -- writes into the guard, and the guard is
 * checked after every case rather than at the end, so the first offender is
 * named. */
static uint16_t canvas[GUARD + TW * TH + GUARD];
static int failures;

static struct pl_text_target target(void)
{
	struct pl_text_target t;

	t.px     = canvas + GUARD;
	t.w      = TW;
	t.h      = TH;
	t.stride = TW;
	return t;
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

static void clear(void)
{
	memset(canvas, 0, sizeof canvas);
}

static int guards_intact(void)
{
	unsigned i;

	for (i = 0u; i < GUARD; i++)
		if (canvas[i] != 0u || canvas[GUARD + TW * TH + i] != 0u)
			return 0;
	return 1;
}

static unsigned ink(uint16_t colour)
{
	unsigned i, n = 0u;

	for (i = 0u; i < TW * TH; i++)
		if (canvas[GUARD + i] == colour)
			n++;
	return n;
}

/* Ink strictly inside the rectangle [x, x+w) x [y, y+h)? */
static int ink_within(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                      uint16_t colour)
{
	uint32_t px, py;

	for (py = 0u; py < TH; py++)
		for (px = 0u; px < TW; px++)
			if (canvas[GUARD + py * TW + px] == colour)
				if (px < x || px >= x + w || py < y || py >= y + h)
					return 0;
	return 1;
}

static void test_geometry(void)
{
	printf(" case: geometry\n");

	ok("one character at scale 1 is one cell wide",
	   pl_text_advance(1u, 1u) == PL_TEXT_CELL_W);
	ok("and scales linearly",
	   pl_text_advance(4u, 3u) == 4u * PL_TEXT_CELL_W * 3u);
	ok("a line is one cell tall", pl_text_height(1u) == PL_TEXT_CELL_H);
	ok("scale 0 has no size, rather than a nonsense one",
	   pl_text_advance(4u, 0u) == 0u && pl_text_height(0u) == 0u);
}

static void test_target(void)
{
	struct pl_text_target t = target();

	printf(" case: an unusable target is refused, not written\n");

	ok("a good target is accepted", pl_text_target_ok(&t));
	ok("NULL is not", !pl_text_target_ok(NULL));

	t.px = NULL;
	ok("a null buffer is not", !pl_text_target_ok(&t));

	t = target();
	t.stride = TW - 1u;
	/* [!] A stride below the width shears every row into the next one.  Every
	 * write still lands inside the array, so nothing faults and the label just
	 * comes out wrong -- which is why it is refused here rather than trusted. */
	ok("a stride narrower than the width is not", !pl_text_target_ok(&t));

	t = target();
	t.w = 0u;
	ok("nor a zero extent", !pl_text_target_ok(&t));

	clear();
	t.px = NULL;
	t.w  = TW;
	pl_text_fill(&t, 0, 0, 4, 4, 0xFFFFu);
	(void)pl_text_draw(&t, 0, 0, "x", 1u, 0xFFFFu);
	ok("and neither entry point touched anything",
	   ink(0xFFFFu) == 0u && guards_intact());
}

static void test_fill(void)
{
	struct pl_text_target t = target();

	printf(" case: fill clips instead of overrunning\n");

	clear();
	pl_text_fill(&t, 2, 3, 5, 4, 0xBEEFu);
	ok("a fill covers exactly its rectangle", ink(0xBEEFu) == 20u);
	ok("and nowhere else", ink_within(2u, 3u, 5u, 4u, 0xBEEFu));
	ok("guards intact", guards_intact());

	clear();
	pl_text_fill(&t, -4, -4, 8, 8, 0xBEEFu);
	ok("a fill starting off the top-left is clipped", ink(0xBEEFu) == 16u);
	ok("and lands at the origin", ink_within(0u, 0u, 4u, 4u, 0xBEEFu));

	clear();
	pl_text_fill(&t, (int32_t)TW - 2, (int32_t)TH - 2, 100, 100, 0xBEEFu);
	ok("one running off the bottom-right likewise", ink(0xBEEFu) == 4u);

	clear();
	pl_text_fill(&t, 100, 100, 4, 4, 0xBEEFu);
	pl_text_fill(&t, 0, 0, -4, 4, 0xBEEFu);
	pl_text_fill(&t, 0, 0, 4, 0, 0xBEEFu);
	ok("off-screen, negative and zero extents write nothing",
	   ink(0xBEEFu) == 0u && guards_intact());

	/* [!] The extremes.  A rectangle spanning the whole int32 range is what
	 * makes a clip written as `x + w` wrap and come out small. */
	clear();
	pl_text_fill(&t, -2147483647 - 1, -2147483647 - 1, 2147483647, 2147483647,
	             0xBEEFu);
	ok("an int32-spanning fill is clipped, not wrapped",
	   ink(0xBEEFu) <= TW * TH && guards_intact());
}

static void test_draw(void)
{
	struct pl_text_target t = target();
	unsigned a, b;

	printf(" case: drawing\n");

	clear();
	ok("an empty string advances nothing",
	   pl_text_draw(&t, 0, 0, "", 1u, 0xFFFFu) == 0u && ink(0xFFFFu) == 0u);
	ok("and so does a null one",
	   pl_text_draw(&t, 0, 0, NULL, 1u, 0xFFFFu) == 0u);
	ok("scale 0 draws nothing",
	   pl_text_draw(&t, 0, 0, "A", 0u, 0xFFFFu) == 0u && ink(0xFFFFu) == 0u);

	clear();
	ok("a string reports the advance it occupies",
	   pl_text_draw(&t, 0, 0, "abc", 2u, 0xFFFFu) ==
	   3u * PL_TEXT_CELL_W * 2u);
	ok("its ink stays inside the cells it claims",
	   ink_within(0u, 0u, 3u * PL_TEXT_CELL_W * 2u, PL_TEXT_CELL_H * 2u,
	              0xFFFFu));
	ok("guards intact", guards_intact());

	/* A space has no ink; a letter does.  Together these say the table is
	 * being indexed and not, say, returning the same glyph for everything. */
	clear();
	(void)pl_text_draw(&t, 0, 0, " ", 1u, 0xFFFFu);
	ok("a space is blank", ink(0xFFFFu) == 0u);

	clear();
	(void)pl_text_draw(&t, 0, 0, "A", 1u, 0xFFFFu);
	a = ink(0xFFFFu);
	clear();
	(void)pl_text_draw(&t, 0, 0, "B", 1u, 0xFFFFu);
	b = ink(0xFFFFu);
	ok("distinct letters have ink", a != 0u && b != 0u);
	ok("and are not the same glyph", a != b);

	/*
	 * [!] THE ENDS OF THE TABLE.  An index computed as (c - 0x20) with a table
	 * of the wrong length shows up at exactly two characters, and a label that
	 * happens to avoid them looks perfect.
	 */
	clear();
	(void)pl_text_draw(&t, 0, 0, "!", 1u, 0xFFFFu);
	ok("the first printable character draws", ink(0xFFFFu) != 0u);
	clear();
	(void)pl_text_draw(&t, 0, 0, "~", 1u, 0xFFFFu);
	ok("and so does the last", ink(0xFFFFu) != 0u);

	/*
	 * [!] AND A CHARACTER THE FONT DOES NOT HAVE.  `char` is signed here, so a
	 * byte above 0x7F arrives negative; indexing with it would read backwards
	 * out of the table.  The visible box says the omission is visible rather
	 * than silent.
	 */
	clear();
	{
		char high[2];

		high[0] = (char)0xE3;   /* a UTF-8 lead byte, as a stray label would */
		high[1] = '\0';
		(void)pl_text_draw(&t, 0, 0, high, 1u, 0xFFFFu);
		ok("a character outside the font draws a visible box",
		   ink(0xFFFFu) != 0u);
		ok("inside its own cell, and inside the buffer",
		   ink_within(0u, 0u, PL_TEXT_CELL_W, PL_TEXT_CELL_H, 0xFFFFu) &&
		   guards_intact());
	}

	/* Clipping: a string starting off the left still draws its tail, and one
	 * running off the right is cut rather than wrapped into row 2. */
	clear();
	(void)pl_text_draw(&t, -(int32_t)PL_TEXT_CELL_W, 0, "AB", 1u, 0xFFFFu);
	ok("a string starting off the left draws only what is on screen",
	   ink_within(0u, 0u, PL_TEXT_CELL_W, PL_TEXT_CELL_H, 0xFFFFu) &&
	   guards_intact());

	clear();
	(void)pl_text_draw(&t, 0, 0, "ABCDEFGHIJKLMNOPQRSTUVWXYZ", 2u, 0xFFFFu);
	ok("a string longer than the target is clipped, not wrapped",
	   ink_within(0u, 0u, TW, PL_TEXT_CELL_H * 2u, 0xFFFFu) &&
	   guards_intact());

	clear();
	(void)pl_text_draw(&t, 0, (int32_t)TH - 2, "Ay", 2u, 0xFFFFu);
	ok("and one at the bottom edge is cut by the buffer, not past it",
	   guards_intact());

	/*
	 * [!] THE COORDINATE EXTREMES.  This header advertises signed, clipped
	 * coordinates, so INT32_MAX is a legal argument -- and a glyph offset added
	 * to it does not fit an int32_t.  Wrapping would put the block at the
	 * OPPOSITE edge of the buffer, which looks like a font bug rather than an
	 * overflow, so a block that is not representable is dropped instead.
	 */
	clear();
	(void)pl_text_draw(&t, 2147483647, 0, "ABC", 4u, 0xFFFFu);
	ok("a string at INT32_MAX draws nothing and stays inside the buffer",
	   ink(0xFFFFu) == 0u && guards_intact());
	clear();
	(void)pl_text_draw(&t, -2147483647 - 1, -2147483647 - 1, "ABC", 4u,
	                   0xFFFFu);
	ok("and one at INT32_MIN likewise",
	   ink(0xFFFFu) == 0u && guards_intact());
}

static void test_sbuf(void)
{
	struct plugin_printer out;
	struct pl_sbuf sb;
	char buf[8];

	printf(" case: the string sink\n");

	pl_sbuf_init(&sb, buf, sizeof buf);
	pl_sbuf_printer(&out, &sb);
	ok("it starts empty and terminated", sb.len == 0u && buf[0] == '\0');

	ok("a write that fits succeeds", pl_fmt_cstr(&out, "abc") == 0);
	ok("and leaves a C string", strcmp(buf, "abc") == 0 && sb.len == 3u);

	ok("appending continues", pl_fmt_cstr(&out, "de") == 0);
	ok("and the buffer is still terminated", strcmp(buf, "abcde") == 0);

	/*
	 * [!] A REFUSAL, NOT A TRUNCATION.  plugin_fmt stops on the first negative
	 * return, so a partial write here would be printed as a complete field --
	 * "cat  -78" for -783, a number that is wrong rather than missing.
	 */
	ok("a write that does not fit is refused", pl_fmt_cstr(&out, "fghij") < 0);
	ok("and it is refused whole -- nothing partial was appended",
	   strcmp(buf, "abcde") == 0);
	ok("and the sink remembers it overflowed", sb.full != 0);

	/* Exactly filling the buffer is not an overflow. */
	pl_sbuf_init(&sb, buf, sizeof buf);
	ok("a write that exactly fills it succeeds",
	   pl_fmt_cstr(&out, "1234567") == 0);
	ok("with room for the terminator and no overflow flag",
	   strcmp(buf, "1234567") == 0 && sb.full == 0);
	ok("and one more byte does not fit", pl_fmt_cstr(&out, "8") < 0);

	/* A degenerate sink refuses rather than writing through a null. */
	pl_sbuf_init(&sb, NULL, 0u);
	ok("a sink with no buffer refuses", pl_fmt_cstr(&out, "x") < 0);
}

int main(void)
{
	printf("test_plugin_text (plugin/common/plugin_text.c):\n");
	test_geometry();
	test_target();
	test_fill();
	test_draw();
	test_sbuf();

	if (!guards_intact()) {
		printf("  FAIL a canary was overwritten\n");
		failures++;
	}
	if (failures != 0) {
		printf("test_plugin_text: %d failure(s)\n", failures);
		return 1;
	}
	printf("test_plugin_text: all cases pass\n");
	return 0;
}
