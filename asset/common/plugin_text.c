/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    plugin_text.c
 * @brief   The font and the rasteriser.  See plugin_text.h.
 *
 * THE FONT is a conventional 5x7 ASCII cell -- the proportions every character
 * LCD and glass terminal settled on, because seven rows is the fewest that
 * gives a lower-case letter a descender and a distinguishable `l`/`1`.  It is
 * stored column-major, five bytes per glyph, bit n of each byte lighting row n
 * from the top.  Column-major because the rasteriser walks destination columns
 * within a row of the cell, so this is the order that reads one byte and shifts.
 *
 * COVERAGE IS THE WHOLE PRINTABLE SET, deliberately.  The two plugins that ship
 * today need lower case, digits and a minus sign, and a font cut to exactly that
 * would silently drop the first character some later container's labels
 * contain.  Everything outside the range draws as a hollow box instead, which
 * costs one more glyph and makes the omission visible on the panel.
 */
#include "plugin_text.h"

#include <stddef.h>

/* ---- the font ------------------------------------------------------------ */

#define PL_GLYPH_W 5u   /**< ink columns; the sixth column of the cell is gap */
#define PL_GLYPH_H 7u   /**< ink rows;    the eighth row    of the cell is gap */

static const uint8_t pl_font[(PL_TEXT_LAST - PL_TEXT_FIRST + 1) * PL_GLYPH_W] = {
	0x00, 0x00, 0x00, 0x00, 0x00,   /* 0x20 ' ' */
	0x00, 0x00, 0x5F, 0x00, 0x00,   /* 0x21 '!' */
	0x00, 0x07, 0x00, 0x07, 0x00,   /* 0x22 '"' */
	0x14, 0x7F, 0x14, 0x7F, 0x14,   /* 0x23 '#' */
	0x24, 0x2A, 0x7F, 0x2A, 0x12,   /* 0x24 '$' */
	0x23, 0x13, 0x08, 0x64, 0x62,   /* 0x25 '%' */
	0x36, 0x49, 0x55, 0x22, 0x50,   /* 0x26 '&' */
	0x00, 0x05, 0x03, 0x00, 0x00,   /* 0x27 '\'' */
	0x00, 0x1C, 0x22, 0x41, 0x00,   /* 0x28 '(' */
	0x00, 0x41, 0x22, 0x1C, 0x00,   /* 0x29 ')' */
	0x14, 0x08, 0x3E, 0x08, 0x14,   /* 0x2A '*' */
	0x08, 0x08, 0x3E, 0x08, 0x08,   /* 0x2B '+' */
	0x00, 0x50, 0x30, 0x00, 0x00,   /* 0x2C ',' */
	0x08, 0x08, 0x08, 0x08, 0x08,   /* 0x2D '-' */
	0x00, 0x60, 0x60, 0x00, 0x00,   /* 0x2E '.' */
	0x20, 0x10, 0x08, 0x04, 0x02,   /* 0x2F '/' */
	0x3E, 0x51, 0x49, 0x45, 0x3E,   /* 0x30 '0' */
	0x00, 0x42, 0x7F, 0x40, 0x00,   /* 0x31 '1' */
	0x42, 0x61, 0x51, 0x49, 0x46,   /* 0x32 '2' */
	0x21, 0x41, 0x45, 0x4B, 0x31,   /* 0x33 '3' */
	0x18, 0x14, 0x12, 0x7F, 0x10,   /* 0x34 '4' */
	0x27, 0x45, 0x45, 0x45, 0x39,   /* 0x35 '5' */
	0x3C, 0x4A, 0x49, 0x49, 0x30,   /* 0x36 '6' */
	0x01, 0x71, 0x09, 0x05, 0x03,   /* 0x37 '7' */
	0x36, 0x49, 0x49, 0x49, 0x36,   /* 0x38 '8' */
	0x06, 0x49, 0x49, 0x29, 0x1E,   /* 0x39 '9' */
	0x00, 0x36, 0x36, 0x00, 0x00,   /* 0x3A ':' */
	0x00, 0x56, 0x36, 0x00, 0x00,   /* 0x3B ';' */
	0x08, 0x14, 0x22, 0x41, 0x00,   /* 0x3C '<' */
	0x14, 0x14, 0x14, 0x14, 0x14,   /* 0x3D '=' */
	0x00, 0x41, 0x22, 0x14, 0x08,   /* 0x3E '>' */
	0x02, 0x01, 0x51, 0x09, 0x06,   /* 0x3F '?' */
	0x32, 0x49, 0x79, 0x41, 0x3E,   /* 0x40 '@' */
	0x7E, 0x11, 0x11, 0x11, 0x7E,   /* 0x41 'A' */
	0x7F, 0x49, 0x49, 0x49, 0x36,   /* 0x42 'B' */
	0x3E, 0x41, 0x41, 0x41, 0x22,   /* 0x43 'C' */
	0x7F, 0x41, 0x41, 0x22, 0x1C,   /* 0x44 'D' */
	0x7F, 0x49, 0x49, 0x49, 0x41,   /* 0x45 'E' */
	0x7F, 0x09, 0x09, 0x09, 0x01,   /* 0x46 'F' */
	0x3E, 0x41, 0x49, 0x49, 0x7A,   /* 0x47 'G' */
	0x7F, 0x08, 0x08, 0x08, 0x7F,   /* 0x48 'H' */
	0x00, 0x41, 0x7F, 0x41, 0x00,   /* 0x49 'I' */
	0x20, 0x40, 0x41, 0x3F, 0x01,   /* 0x4A 'J' */
	0x7F, 0x08, 0x14, 0x22, 0x41,   /* 0x4B 'K' */
	0x7F, 0x40, 0x40, 0x40, 0x40,   /* 0x4C 'L' */
	0x7F, 0x02, 0x0C, 0x02, 0x7F,   /* 0x4D 'M' */
	0x7F, 0x04, 0x08, 0x10, 0x7F,   /* 0x4E 'N' */
	0x3E, 0x41, 0x41, 0x41, 0x3E,   /* 0x4F 'O' */
	0x7F, 0x09, 0x09, 0x09, 0x06,   /* 0x50 'P' */
	0x3E, 0x41, 0x51, 0x21, 0x5E,   /* 0x51 'Q' */
	0x7F, 0x09, 0x19, 0x29, 0x46,   /* 0x52 'R' */
	0x46, 0x49, 0x49, 0x49, 0x31,   /* 0x53 'S' */
	0x01, 0x01, 0x7F, 0x01, 0x01,   /* 0x54 'T' */
	0x3F, 0x40, 0x40, 0x40, 0x3F,   /* 0x55 'U' */
	0x1F, 0x20, 0x40, 0x20, 0x1F,   /* 0x56 'V' */
	0x3F, 0x40, 0x38, 0x40, 0x3F,   /* 0x57 'W' */
	0x63, 0x14, 0x08, 0x14, 0x63,   /* 0x58 'X' */
	0x07, 0x08, 0x70, 0x08, 0x07,   /* 0x59 'Y' */
	0x61, 0x51, 0x49, 0x45, 0x43,   /* 0x5A 'Z' */
	0x00, 0x7F, 0x41, 0x41, 0x00,   /* 0x5B '[' */
	0x02, 0x04, 0x08, 0x10, 0x20,   /* 0x5C '\\' */
	0x00, 0x41, 0x41, 0x7F, 0x00,   /* 0x5D ']' */
	0x04, 0x02, 0x01, 0x02, 0x04,   /* 0x5E '^' */
	0x40, 0x40, 0x40, 0x40, 0x40,   /* 0x5F '_' */
	0x00, 0x01, 0x02, 0x04, 0x00,   /* 0x60 '`' */
	0x20, 0x54, 0x54, 0x54, 0x78,   /* 0x61 'a' */
	0x7F, 0x48, 0x44, 0x44, 0x38,   /* 0x62 'b' */
	0x38, 0x44, 0x44, 0x44, 0x20,   /* 0x63 'c' */
	0x38, 0x44, 0x44, 0x48, 0x7F,   /* 0x64 'd' */
	0x38, 0x54, 0x54, 0x54, 0x18,   /* 0x65 'e' */
	0x08, 0x7E, 0x09, 0x01, 0x02,   /* 0x66 'f' */
	0x0C, 0x52, 0x52, 0x52, 0x3E,   /* 0x67 'g' */
	0x7F, 0x08, 0x04, 0x04, 0x78,   /* 0x68 'h' */
	0x00, 0x44, 0x7D, 0x40, 0x00,   /* 0x69 'i' */
	0x20, 0x40, 0x44, 0x3D, 0x00,   /* 0x6A 'j' */
	0x7F, 0x10, 0x28, 0x44, 0x00,   /* 0x6B 'k' */
	0x00, 0x41, 0x7F, 0x40, 0x00,   /* 0x6C 'l' */
	0x7C, 0x04, 0x18, 0x04, 0x78,   /* 0x6D 'm' */
	0x7C, 0x08, 0x04, 0x04, 0x78,   /* 0x6E 'n' */
	0x38, 0x44, 0x44, 0x44, 0x38,   /* 0x6F 'o' */
	0x7C, 0x14, 0x14, 0x14, 0x08,   /* 0x70 'p' */
	0x08, 0x14, 0x14, 0x18, 0x7C,   /* 0x71 'q' */
	0x7C, 0x08, 0x04, 0x04, 0x08,   /* 0x72 'r' */
	0x48, 0x54, 0x54, 0x54, 0x20,   /* 0x73 's' */
	0x04, 0x3F, 0x44, 0x40, 0x20,   /* 0x74 't' */
	0x3C, 0x40, 0x40, 0x20, 0x7C,   /* 0x75 'u' */
	0x1C, 0x20, 0x40, 0x20, 0x1C,   /* 0x76 'v' */
	0x3C, 0x40, 0x30, 0x40, 0x3C,   /* 0x77 'w' */
	0x44, 0x28, 0x10, 0x28, 0x44,   /* 0x78 'x' */
	0x0C, 0x50, 0x50, 0x50, 0x3C,   /* 0x79 'y' */
	0x44, 0x64, 0x54, 0x4C, 0x44,   /* 0x7A 'z' */
	0x00, 0x08, 0x36, 0x41, 0x00,   /* 0x7B '{' */
	0x00, 0x00, 0x7F, 0x00, 0x00,   /* 0x7C '|' */
	0x00, 0x41, 0x36, 0x08, 0x00,   /* 0x7D '}' */
	0x08, 0x04, 0x08, 0x10, 0x08,   /* 0x7E '~' */
};

/* Drawn for anything the font does not cover, so a gap is visible. */
static const uint8_t pl_font_missing[PL_GLYPH_W] = {
	0x7F, 0x41, 0x41, 0x41, 0x7F,
};

/*
 * The five columns of one character.
 *
 * [!] THE INDEX IS COMPUTED FROM AN UNSIGNED CHAR.  `char` is signed on this
 * toolchain, so a byte above 0x7F in a label -- a stray UTF-8 continuation, say
 * -- would arrive negative and index backwards out of the table.  The cast
 * happens here, once, rather than at each call site.
 */
static const uint8_t *pl_glyph(char c)
{
	unsigned u = (unsigned)(unsigned char)c;

	if (u < (unsigned)PL_TEXT_FIRST || u > (unsigned)PL_TEXT_LAST)
		return pl_font_missing;
	return &pl_font[(u - (unsigned)PL_TEXT_FIRST) * PL_GLYPH_W];
}

/* ---- the target ---------------------------------------------------------- */

int pl_text_target_ok(const struct pl_text_target *t)
{
	if (t == NULL || t->px == NULL)
		return 0;
	if (t->w == 0u || t->h == 0u)
		return 0;
	/* A stride below the width would make row n's tail overlap row n+1's
	 * head: every write would still land inside the array, so nothing would
	 * fault and the label would simply come out sheared. */
	return t->stride >= t->w;
}

uint32_t pl_text_advance(uint32_t chars, uint32_t scale)
{
	if (scale == 0u)
		return 0u;
	/* Bounded by construction: a caller's buffer is a few hundred pixels
	 * wide, and anything that overflowed this would be refused by the
	 * clipping below rather than wrapping into a small number. */
	if (chars > 0xFFFFu || scale > 0xFFu)
		return 0u;
	return chars * PL_TEXT_CELL_W * scale;
}

uint32_t pl_text_height(uint32_t scale)
{
	if (scale == 0u || scale > 0xFFu)
		return 0u;
	return PL_TEXT_CELL_H * scale;
}

/*
 * Clip [v, v + n) to [0, limit) and report what survived.
 *
 * Returns 0 when nothing does.  Signed in, unsigned out, and the subtraction
 * that produces the offset is done after the clamp so it cannot go negative.
 */
static int pl_clip(int32_t v, int32_t n, uint32_t limit,
                   uint32_t *lo, uint32_t *count)
{
	int64_t a = (int64_t)v;
	int64_t b = a + (int64_t)n;

	if (n <= 0)
		return 0;
	if (a < 0)
		a = 0;
	if (b > (int64_t)limit)
		b = (int64_t)limit;
	if (b <= a)
		return 0;
	*lo    = (uint32_t)a;
	*count = (uint32_t)(b - a);
	return 1;
}

void pl_text_fill(const struct pl_text_target *t, int32_t x, int32_t y,
                  int32_t w, int32_t h, uint16_t colour)
{
	uint32_t x0, y0, cols, rows, r, c;

	if (!pl_text_target_ok(t))
		return;
	if (!pl_clip(x, w, t->w, &x0, &cols))
		return;
	if (!pl_clip(y, h, t->h, &y0, &rows))
		return;

	for (r = 0u; r < rows; r++) {
		uint16_t *row = t->px + (size_t)(y0 + r) * (size_t)t->stride;

		for (c = 0u; c < cols; c++)
			row[x0 + c] = colour;
	}
}

/*
 * One glyph, scaled.
 *
 * Each font pixel becomes a scale x scale block.  The blocks are clipped
 * individually rather than the glyph as a whole: at scale 1 that is the same
 * thing, and at larger scales it keeps a glyph straddling the edge from being
 * dropped entirely.
 */
static void pl_text_glyph(const struct pl_text_target *t, int64_t x, int64_t y,
                          char ch, uint32_t scale, uint16_t fg)
{
	const uint8_t *g = pl_glyph(ch);
	uint32_t col, row;

	for (col = 0u; col < PL_GLYPH_W; col++) {
		uint8_t bits = g[col];

		if (bits == 0u)
			continue;
		for (row = 0u; row < PL_GLYPH_H; row++) {
			int64_t px, py;

			if ((bits & (uint8_t)(1u << row)) == 0u)
				continue;
			px = x + (int64_t)((uint64_t)col * scale);
			py = y + (int64_t)((uint64_t)row * scale);
			/*
			 * [!] A BLOCK THAT IS NOT REPRESENTABLE IS DROPPED, NOT WRAPPED.
			 * The coordinates are signed and this header advertises clipping,
			 * so a caller may legitimately hand over INT32_MAX -- and the sum
			 * of that and a glyph offset is not an int32_t.  Computing in 64
			 * bits and refusing what does not fit keeps the clip's own
			 * arithmetic in range; wrapping would put the block at the
			 * OPPOSITE edge, which looks like a font bug rather than an
			 * overflow.
			 */
			if (px < -2147483647LL || px > 2147483647LL ||
			    py < -2147483647LL || py > 2147483647LL)
				continue;
			pl_text_fill(t, (int32_t)px, (int32_t)py,
			             (int32_t)scale, (int32_t)scale, fg);
		}
	}
}

uint32_t pl_text_draw(const struct pl_text_target *t, int32_t x, int32_t y,
                      const char *s, uint32_t scale, uint16_t fg)
{
	uint32_t n = 0u;

	if (!pl_text_target_ok(t) || s == NULL || scale == 0u || scale > 0xFFu)
		return 0u;

	while (s[n] != '\0') {
		uint32_t adv = pl_text_advance(n, scale);

		/*
		 * [!] STOP WHEN THE ADVANCE STOPS BEING REPRESENTABLE, not when
		 * the glyph falls off the right edge.  A caller may legitimately
		 * pass a negative x to scroll a long label, so "off the target"
		 * is not a reason to stop -- but an advance that has grown past
		 * what pl_text_advance() will compute would come back 0 and draw
		 * every remaining glyph on top of the first.
		 */
		if (n != 0u && adv == 0u)
			break;
		pl_text_glyph(t, (int64_t)x + (int64_t)adv, (int64_t)y, s[n], scale,
		              fg);
		n++;
		if (n > 0xFFFFu)
			break;
	}
	return pl_text_advance(n, scale);
}

/* ---- the string sink ----------------------------------------------------- */

void pl_sbuf_init(struct pl_sbuf *sb, char *buf, size_t cap)
{
	if (sb == NULL)
		return;
	sb->buf  = buf;
	sb->cap  = cap;
	sb->len  = 0u;
	sb->full = 0;
	if (buf != NULL && cap != 0u)
		buf[0] = '\0';
}

/*
 * [!] A REFUSAL, NOT A TRUNCATION.  plugin_fmt's contract is that a sink which
 * has said no has said no, and its helpers stop on the first negative return.
 * Silently keeping the prefix would leave a label reading "cat  -78" -- a number
 * that is wrong rather than absent, which is the failure this project keeps
 * finding worse than none.  The buffer still holds what fitted, and @ref full
 * says the rest did not.
 */
int pl_sbuf_write(void *ctx, const char *s, size_t len)
{
	struct pl_sbuf *sb = (struct pl_sbuf *)ctx;
	size_t i;

	if (sb == NULL || sb->buf == NULL || sb->cap == 0u)
		return -1;
	if (s == NULL)
		return -1;
	if (len > sb->cap - 1u - sb->len) {
		sb->full = 1;
		return -1;
	}
	for (i = 0u; i < len; i++)
		sb->buf[sb->len + i] = s[i];
	sb->len += len;
	sb->buf[sb->len] = '\0';
	return (int)len;
}

void pl_sbuf_printer(struct plugin_printer *out, struct pl_sbuf *sb)
{
	if (out == NULL)
		return;
	out->ctx   = sb;
	out->write = pl_sbuf_write;
}
