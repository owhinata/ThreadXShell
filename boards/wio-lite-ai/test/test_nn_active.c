/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    test_nn_active.c
 * @brief   The active-decoder shim, with a plugin and without one
 *          (issues #110, #116).
 *
 * port/nn/nn_active.c is the ONE place this board decides what reads a model's
 * outputs.  Since issue #116 the firmware carries no decoder of its own, so
 * every one of its answers has two forms -- with a plugin loaded, and with none
 * -- and the second form is new: it used to fall back on a resident BlazeFace
 * decoder, and each fallback read as something working.
 *
 * [!] WHAT THIS CATCHES IS INVISIBLE FROM A CONSOLE.  Nothing on the board
 * should reach the no-plugin arm of a decode: the worker asks
 * nn_active_is_plugin() first and publishes "nothing decoded this" instead, and
 * a stream is refused admission before a camera is lit.  A shim that quietly
 * decoded there anyway, or that kept answering with a threshold nothing reads,
 * would look exactly like a working board -- which is why the arm is checked
 * precisely because it is unreachable.
 *
 * [!] AND BOTH DIRECTIONS ARE CHECKED, not just the new one.  "With no plugin
 * it refuses" is satisfied by a shim that refuses ALWAYS; the plugin arm below
 * is what makes the refusals mean something.  The threshold is the sharpest of
 * them: it is set THROUGH the shim and read back through it, and then the
 * plugin is unloaded and the shim must report NONE rather than a number it
 * kept somewhere of its own.
 *
 * [!] THE SHIM IS COMPILED, NOT MODELLED.  Turning a slot into a callable
 * address is plugin_run.c's job on the board; here the test supplies
 * plugin_run_active() and plugin_run_slot() over the plugin's OWN
 * `plugin_slot_table`, which is the same table the packer reads.  The plugin
 * linked in is the real asset/plugins/blazeface, and `struct nn_model` is
 * opaque in nn.h (it is defined in nn.c), so the two accessors the shim uses
 * are supplied here -- which is also what makes the shim testable at all.
 */
#include "nn_active.h"
#include "nn_desc.h"
#include "plugin_run.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* ---- what the board supplies on hardware --------------------------------- */

void log_write(unsigned level, const char *tag, const char *fmt, ...)
{
	(void)level;
	(void)tag;
	(void)fmt;
}

/* The plugin's slot table, from asset/plugins/blazeface/plugin_main.c.  On the
 * board the loader copies the image into the reservation and adds the
 * manifest's offsets to its base; here the linker has already placed the
 * functions and the table already holds their addresses. */
extern const void *const plugin_slot_table[PLUGIN_SLOT_COUNT];

static int pl_loaded;

int plugin_run_active(void)
{
	return pl_loaded;
}

void *plugin_run_slot(unsigned slot)
{
	if (!pl_loaded || slot >= (unsigned)PLUGIN_SLOT_COUNT)
		return NULL;
	return (void *)(uintptr_t)plugin_slot_table[slot];
}

/* ---- the model handle the shim pulls tensors from ------------------------ */

struct nn_model {
	struct nn_tensor out[NN_MAX_IO];
	int              n;
};

static struct nn_model stub;

int nn_output_count(const struct nn_model *m)
{
	return m ? m->n : 0;
}

struct nn_tensor *nn_output(struct nn_model *m, int idx)
{
	if (m == NULL || idx < 0 || idx >= m->n)
		return NULL;
	if (m->out[idx].data == NULL && m->out[idx].bytes == 0u)
		return NULL;   /* a hole in the set, deliberately */
	return &m->out[idx];
}

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

/* ---- a BlazeFace-shaped float32 model ------------------------------------ */

/*
 * This board's graphs are float32 and publish scale 0 for an unquantised
 * tensor, which is what the translation hands the decoder.
 */
#define A512    512
#define A384    384
#define STRIDE  16

static float box512[A512 * STRIDE];
static float scr512[A512];
static float box384[A384 * STRIDE];
static float scr384[A384];

static void set_f32(struct nn_tensor *t, void *data, uint32_t bytes,
                    uint16_t anchors, uint16_t chan)
{
	memset(t, 0, sizeof(*t));
	t->data    = data;
	t->bytes   = bytes;
	t->ndim    = 3;
	t->dims[0] = 1;
	t->dims[1] = anchors;
	t->dims[2] = chan;
	t->dtype   = NN_DTYPE_FLOAT32;
}

static void reset_model(void)
{
	memset(box512, 0, sizeof box512);
	memset(scr512, 0, sizeof scr512);
	memset(box384, 0, sizeof box384);
	memset(scr384, 0, sizeof scr384);

	memset(&stub, 0, sizeof stub);
	stub.n = 4;
	set_f32(&stub.out[0], box512, sizeof box512, A512, STRIDE);
	set_f32(&stub.out[1], scr512, sizeof scr512, A512, 1);
	set_f32(&stub.out[2], box384, sizeof box384, A384, STRIDE);
	set_f32(&stub.out[3], scr384, sizeof scr384, A384, 1);
}

/* One face at anchor 0: a raw score well over any sane threshold, and a box
 * with no offset so it lands on the anchor centre. */
static void put_one_face(void)
{
	scr512[0] = 2.0f;
	box512[0] = 0.0f;
	box512[1] = 0.0f;
	box512[2] = 20.0f;
	box512[3] = 20.0f;
}

/* ---- a recording painter -------------------------------------------------- */

/*
 * NOT port/plugin/plugin_paint.c: that one owns the budget and the clipping and
 * has its own test.  What is wanted here is whether the plugin was asked to
 * paint at all.
 */
static unsigned rec_rects, rec_fills, rec_blits;

static void rec_rect(void *ctx, const struct plugin_rect *r, uint16_t rgb,
                     uint16_t stroke)
{
	(void)ctx; (void)r; (void)rgb; (void)stroke;
	rec_rects++;
}

static void rec_fill(void *ctx, const struct plugin_rect *r, uint16_t rgb)
{
	(void)ctx; (void)r; (void)rgb;
	rec_fills++;
}

static void rec_blit(void *ctx, const struct plugin_rect *r,
                     const uint16_t *src, uint32_t stride, int32_t key)
{
	(void)ctx; (void)r; (void)src; (void)stride; (void)key;
	rec_blits++;
}

static const struct plugin_painter rec_painter = {
	NULL, rec_rect, rec_fill, rec_blit
};

static void rec_reset(void)
{
	rec_rects = 0u;
	rec_fills = 0u;
	rec_blits = 0u;
}

/* ---- a capture for report() ----------------------------------------------- */

static size_t cap_len;

static int cap_write(void *ctx, const char *s, size_t len)
{
	(void)ctx; (void)s;
	cap_len += len;
	return (int)len;
}

int main(void)
{
	int n, rc;
	unsigned th;

	printf("test_nn_active\n");

	reset_model();
	put_one_face();

	/* ================================================================
	 * 1.  With no plugin there is NO DECODER (issue #116)
	 * ================================================================ */
	pl_loaded = 0;

	expect("no plugin loaded: the shim says so", nn_active_is_plugin() == 0,
	       "claims a plugin");

	n = nn_active_decode(&stub);
	expect("[!] a decode says no decoder is bound rather than decoding",
	       n == BF_ERR_UNINIT, "got %d", n);
	expect("and it does not say 'not a detector', which routes to the class "
	       "report", n != BF_ERR_MODEL, "got %d", n);

	rec_reset();
	nn_active_draw(&rec_painter);
	expect("nothing paints", rec_rects == 0u && rec_fills == 0u &&
	       rec_blits == 0u, "%u rect(s), %u fill(s), %u blit(s)", rec_rects,
	       rec_fills, rec_blits);
	expect("[!] and a stream is refused for having nothing to draw",
	       nn_active_can_draw() == 0, "claims it draws");

	cap_len = 0u;
	expect("it describes nothing, successfully",
	       nn_active_report(cap_write, NULL) == 0 && cap_len == 0u,
	       "wrote %lu B", (unsigned long)cap_len);
	expect("and says so beforehand", nn_active_can_report() == 0,
	       "claims a report");

	expect("[!] the threshold is reported absent, not borrowed from anywhere",
	       nn_active_get_thresh_milli() == NN_SVC_THRESH_NONE, "%u",
	       nn_active_get_thresh_milli());
	rc = nn_active_set_thresh_milli(700u);
	expect("[!] and setting one is refused as a state, not as a bad value",
	       rc == NN_ACTIVE_THRESH_NO_DECODER, "got %d", rc);
	expect("which changed nothing",
	       nn_active_get_thresh_milli() == NN_SVC_THRESH_NONE, "%u",
	       nn_active_get_thresh_milli());

	/*
	 * [!] THE SHAPE QUESTION IS THE ONE THAT MUST STILL PASS.  It is the
	 * admission `nn run` shares with `nn stream start`: nothing is going to
	 * read these outputs, so nothing can object to them, and refusing here
	 * would refuse `nn run` on every bare model -- which still runs the
	 * inference and reports the tensors themselves.  What stops the STREAM is
	 * can_draw() above, and only when a panel was asked for.
	 */
	expect("[!] but the shapes pass, because nobody is going to read them",
	       nn_active_shapes_ok(&stub) != 0, "refused");
	expect("even shapes no decoder would accept",
	       (stub.out[1].ndim = 1, nn_active_shapes_ok(&stub) != 0), "refused");
	reset_model();
	put_one_face();
	expect("a null model is still refused", nn_active_shapes_ok(NULL) == 0,
	       "accepted");

	/* ================================================================
	 * 2.  With the plugin loaded, every one of those answers moves
	 * ================================================================
	 *
	 * Without this the section above is satisfied by a shim that refuses
	 * unconditionally, which is a different bug with the same test output.
	 */
	{
		plugin_entry_fn entry =
			(plugin_entry_fn)(uintptr_t)plugin_slot_table[PLUGIN_SLOT_ENTRY];

		/* pl_loaded stays 0 across the call, as the loader's own flag does on
		 * the board: entry() runs before the plugin is published. */
		expect("the plugin accepts the base vtable",
		       entry(nn_active_base()) == 0, "refused");
		pl_loaded = 1;
	}

	expect("the shim now says a plugin is in force", nn_active_is_plugin() != 0,
	       "says none");
	expect("it reads the shapes", nn_active_shapes_ok(&stub) != 0, "refused");

	n = nn_active_decode(&stub);
	expect("[!] and a decode reaches it -- the refusal above was about there "
	       "being nobody, not about refusing", n >= 0, "got %d", n);
	expect("which found the face", n == 1, "got %d", n);

	rec_reset();
	nn_active_draw(&rec_painter);
	expect("it paints", rec_rects + rec_fills + rec_blits > 0u,
	       "nothing drawn");
	expect("and says it would", nn_active_can_draw() != 0, "says it draws "
	       "nothing");

	cap_len = 0u;
	expect("it describes its own result", nn_active_can_report() != 0 &&
	       nn_active_report(cap_write, NULL) >= 0 && cap_len > 0u,
	       "wrote %lu B", (unsigned long)cap_len);

	/* ================================================================
	 * 3.  The threshold belongs to the plugin, and goes with it
	 * ================================================================ */
	rc = nn_active_set_thresh_milli(642u);
	expect("a threshold set through the shim is accepted",
	       rc == NN_ACTIVE_THRESH_OK, "got %d", rc);
	th = nn_active_get_thresh_milli();
	expect("and reads back through it", th == 642u, "got %u", th);

	pl_loaded = 0;
	expect("[!] with the plugin gone the threshold is gone with it -- the shim "
	       "kept no copy", nn_active_get_thresh_milli() == NN_SVC_THRESH_NONE,
	       "%u", nn_active_get_thresh_milli());

	/* And the decode does not resume from somewhere else, either. */
	n = nn_active_decode(&stub);
	expect("and no second decoder appears once the first is unloaded",
	       n == BF_ERR_UNINIT, "got %d", n);

	/* ================================================================
	 * 4.  The transform a plugin is handed
	 * ================================================================
	 *
	 * The one piece of arithmetic in the base vtable.  On this board the
	 * mapping is a multiply by the drawing surface, because the band
	 * downsample squashes the whole frame onto the model's square input.
	 */
	{
		struct plugin_rect r;

		memset(&r, 0, sizeof r);
		expect("a unit box maps onto the whole frame",
		       nn_active_to_frame(NULL, 0.0f, 0.0f, 1.0f, 1.0f, &r) == 0 &&
		               r.x0 == 0 && r.y0 == 0 &&
		               r.x1 == NN_ACTIVE_FRAME_W && r.y1 == NN_ACTIVE_FRAME_H,
		       "(%ld,%ld)-(%ld,%ld)", (long)r.x0, (long)r.y0, (long)r.x1,
		       (long)r.y1);
		expect("a non-finite coordinate is refused rather than cast",
		       nn_active_to_frame(NULL, 0.0f / 0.0f, 0.0f, 1.0f, 1.0f, &r) != 0,
		       "accepted");
		expect("and a null destination too",
		       nn_active_to_frame(NULL, 0.0f, 0.0f, 1.0f, 1.0f, NULL) != 0,
		       "accepted");
	}

	if (failures) {
		printf("test_nn_active: %d failure(s)\n", failures);
		return 1;
	}
	printf("test_nn_active: all cases pass\n");
	return 0;
}
