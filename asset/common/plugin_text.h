/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    plugin_text.h
 * @brief   A font, a rasteriser and a string sink, for plugins that put words
 *          on the panel (issue #105 = #78 Step 2).
 *
 * [!] THE FONT LIVES IN THE PLUGIN, NOT IN THE FIRMWARE, AND THAT IS THE POINT.
 * Issue #78 exists because a model's meaning cannot be held by the base: the
 * class NAMES travel with the weights and nowhere else.  A `text()` primitive on
 * the painter would put a typeface in three firmwares and make every later
 * question about it -- a bigger cell, a glyph the font lacks, a second script --
 * a firmware change, which is the errand this issue removes.  It would also cost
 * an ABI break: struct plugin_painter carries no version or size field (unlike
 * struct plugin_base_api), so a member appended to it cannot be detected by a
 * plugin built against the older shape, and PLUGIN_ABI_VERSION is compared for
 * exact equality -- every container in the store would have to be rebuilt and
 * re-sent.
 *
 * So the escape hatch the painter was BUILT with is the one used here: a plugin
 * rasterises into its OWN buffer and hands the base spans through `blit`.  The
 * base still validates every rectangle, so the one property that boundary
 * provides -- loaded code cannot write outside the frame -- is unchanged.
 *
 * WHERE THE WORK GOES.  Rasterise in decode(), on the camera producer, with no
 * panel guard held and a whole frame period to spend.  draw() runs on the panel
 * thread inside the guard and should do nothing but hand over the finished
 * spans.  That is the split cam_lcd_sink.h already documents, and the pipeline's
 * one-outstanding-delivery rule is what makes the hand-off need no lock.
 *
 * [!] NOTHING HERE OWNS STORAGE.  Every entry point writes only the buffer the
 * caller describes, and the caller is expected to derive that buffer's extents
 * from ONE constant and to _Static_assert the relations (stride >= width,
 * elements >= stride * height).  The plugin image gate proves neither
 * out-of-bounds reads nor out-of-bounds writes -- check_plugin_image.py says so
 * in its own header -- so the assertion at the declaration is the check.
 */
#ifndef PLUGIN_TEXT_H
#define PLUGIN_TEXT_H

#include <stddef.h>
#include <stdint.h>

#include "plugin_abi.h"

/**
 * The character cell, in font pixels.
 *
 * Glyphs are 5x7 inside a 6x8 cell: the spare column and row are the gap, so a
 * caller lays text out by multiplying and never has to know about kerning.
 */
#define PL_TEXT_CELL_W  6u
#define PL_TEXT_CELL_H  8u

/** Lowest and highest character the font draws.  Anything else prints as a
 *  hollow box, so a missing glyph is VISIBLE rather than a silent gap. */
#define PL_TEXT_FIRST   0x20
#define PL_TEXT_LAST    0x7E

/** A buffer to rasterise into.  Pixels are RGB565 in the PLUGIN's own colour
 *  space -- the wire swap belongs to the base and happens inside blit. */
struct pl_text_target {
	uint16_t *px;       /**< first pixel                                  */
	uint32_t  w, h;     /**< extents, in pixels                           */
	uint32_t  stride;   /**< pixels per row; >= w                         */
};

/**
 * @brief  Is this target usable?
 *
 * @return non-zero when it is.  Every function below returns without touching
 *         anything when it is not, so a caller that forgot to fill one in gets
 *         a blank label rather than a wild write.
 */
int pl_text_target_ok(const struct pl_text_target *t);

/** Destination pixels @p chars characters occupy at @p scale. */
uint32_t pl_text_advance(uint32_t chars, uint32_t scale);

/** Destination pixels one line occupies vertically at @p scale. */
uint32_t pl_text_height(uint32_t scale);

/**
 * @brief  Fill a rectangle of @p t with @p colour.
 *
 * Signed and clipped, like everything else that takes a rectangle in this
 * project: a caller computing a label box near an edge should not have to clamp
 * first, because that is the same arithmetic done twice, differently.
 */
void pl_text_fill(const struct pl_text_target *t, int32_t x, int32_t y,
                  int32_t w, int32_t h, uint16_t colour);

/**
 * @brief  Draw @p s at @p x, @p y in @p fg.  Set pixels only.
 *
 * The background is left alone, so a caller chooses between an opaque label
 * (fill first, then draw) and a colour-keyed one (fill with the key, draw, then
 * blit with that key) without this function needing to know which.
 *
 * @return the advance the string WOULD occupy, whether or not it was clipped,
 *         so a caller can lay out a second field beside it.  Zero for a null or
 *         empty string, or an unusable target.
 */
uint32_t pl_text_draw(const struct pl_text_target *t, int32_t x, int32_t y,
                      const char *s, uint32_t scale, uint16_t fg);

/* ---- a string sink ------------------------------------------------------- */

/**
 * A fixed buffer that a @ref plugin_printer can write into.
 *
 * [!] SO THAT NUMBER FORMATTING HAS ONE IMPLEMENTATION.  A label reads
 * "cat  -783", and the second half is a number: without this, plugin_text would
 * grow its own integer formatter beside plugin_fmt's, and the two would drift.
 * Instead the plugin points a printer at a char buffer and the EXISTING
 * pl_fmt_* helpers fill it -- including their rule that a refusal propagates.
 */
struct pl_sbuf {
	char   *buf;
	size_t  cap;     /**< bytes in @ref buf, including the terminator     */
	size_t  len;     /**< bytes written so far                            */
	int     full;    /**< set once something did not fit; never cleared   */
};

/** Point @p sb at @p buf and terminate it empty. */
void pl_sbuf_init(struct pl_sbuf *sb, char *buf, size_t cap);

/**
 * @brief  Bind a printer that appends to @p sb.
 *
 * The buffer stays NUL-terminated after every write, so @ref pl_sbuf::buf can
 * be handed straight to @ref pl_text_draw.
 *
 * [!] THE WRITER MUST STAY A LEAF.  It is reached through the pl_print_write
 * veneer, and the stack gate cannot see across a veneer -- it charges a flat
 * allowance there for whatever is on the other side.  Normally that other side
 * is the base; here it is this image's own function, which the gate therefore
 * does not analyse as part of the caller's chain.  board.cmake bounds it by
 * name with its own --entry so the assumption is checked rather than trusted.
 */
void pl_sbuf_printer(struct plugin_printer *out, struct pl_sbuf *sb);

/** The sink behind @ref pl_sbuf_printer.  Named, because board.cmake bounds it
 *  by name -- see above. */
int pl_sbuf_write(void *ctx, const char *s, size_t len);

#endif /* PLUGIN_TEXT_H */
