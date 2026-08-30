/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    test_plugin_decode.c
 * @brief   The active-decoder shim, and the resident decoder against the plugin
 *          that replaces it (issue #103).
 *
 * TWO SUBJECTS, ONE BINARY, BECAUSE THE POINT IS THE COMPARISON.  The real
 * port/npu/nn_active.c is compiled here together with BOTH decoders it routes
 * between: the resident adapter (port/npu/nn_decoder.c) and the REAL plugin
 * source (plugin/blazeface/plugin_main.c, linked against the same
 * svc/blazeface.c the firmware links).  Neither is stubbed, so what is measured
 * is what runs.
 *
 * [!] WHY THE ARITHMETIC BEING SHARED DOES NOT MAKE THIS TAUTOLOGICAL.  Both
 * decoders call one blazeface_decode(), so of course they agree about anchors
 * and NMS -- that is settled in shell/test/test_blazeface.c.  What differs is
 * everything AROUND it, and every one of those has a way to be wrong that the
 * board reports as a working preview:
 *
 *   - the two hold SEPARATE state and SEPARATE thresholds, so a threshold that
 *     reached the wrong one leaves `nn thresh` reporting a number the boxes do
 *     not obey.  Issue #103's own plan calls that out as the divergence a
 *     differential test which gives BOTH decoders the same threshold cannot
 *     see -- so this file sets the threshold through the SHIM and checks the
 *     other decoder did not move;
 *   - the plugin's result is PRIVATE.  It comes back only through draw() and
 *     report(), so those are the only surfaces on which its boxes can be
 *     compared with the resident ones at all;
 *   - the coordinate transform is published separately from the decode, and
 *     wiring it to the wrong one of the two publishers is what made every box
 *     `nn run` produced come back "outside the frame" on hardware.  Here that
 *     is a case, not a session next to the old output.
 *
 * [!] AND THE SHIM IS COMPILED, NOT MODELLED.  Turning a slot offset into a
 * callable address is plugin_run.c's job on the board; here the test supplies
 * plugin_run_active() and plugin_run_slot() over the plugin's OWN
 * `plugin_slot_table`, which is the same table the packer reads.  A host
 * address does not fit the uint32_t a manifest carries, and that is the only
 * reason this seam exists -- the arithmetic it replaces has its own coverage in
 * the container tests.
 *
 * npu_tensor_is_int8() and log_write() are defined here for the same reason
 * test_nn_decoder.c defines them: on the board the first lives in npu_tflm.cc,
 * the one translation unit that can see TfLiteType.
 */
#include "nn_active.h"
#include "nn_decoder.h"
#include "nn_preproc.h"
#include "plugin_run.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

/* ---- what the board supplies on hardware --------------------------------- */

#define T_INT8   9

bool npu_tensor_is_int8(int8_t type)
{
	return type == (int8_t)T_INT8;
}

void log_write(unsigned level, const char *tag, const char *fmt, ...)
{
	(void)level;
	(void)tag;
	(void)fmt;
}

/* The plugin's slot table, from plugin/blazeface/plugin_main.c.  On the board
 * the loader copies the image into the reservation and adds the manifest's
 * offsets to its base; here the linker has already placed the functions and the
 * table already holds their addresses. */
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

/* ---- the synthetic model -------------------------------------------------- */

/*
 * The real model's quantisation, as in test_nn_decoder.c and the core decoder
 * test.  The four tensors carry four DIFFERENT scales and zero points, which is
 * the property a shared dequantisation constant would break.
 */
#define S_BOX512   0.306708127f
#define Z_BOX512   (-47)
#define S_SCR512   0.0369369201f
#define Z_SCR512   49
#define S_BOX384   1.2020129f
#define Z_BOX384   (-47)
#define S_SCR384   1.22469842f
#define Z_SCR384   126

#define A512       512
#define A384       384
#define STRIDE     16

static int8_t box512[A512 * STRIDE];
static int8_t scr512[A512];
static int8_t box384[A384 * STRIDE];
static int8_t scr384[A384];

static struct npu_tensor tens[NN_DECODER_MAX_OUTPUTS];

static void set_tensor(struct npu_tensor *t, void *data, size_t bytes,
                       int32_t anchors, int32_t chan, float scale, int32_t zp)
{
	memset(t, 0, sizeof(*t));
	t->data       = data;
	t->bytes      = bytes;
	t->rank       = 3;
	t->dims[0]    = 1;
	t->dims[1]    = anchors;
	t->dims[2]    = chan;
	t->type       = (int8_t)T_INT8;
	t->scale      = scale;
	t->zero_point = zp;
}

/* Quiet tensors: filled with each one's OWN zero point, so nothing decodes. */
static void reset_tensors(void)
{
	int i;

	for (i = 0; i < A512; i++)
		scr512[i] = (int8_t)Z_SCR512;
	for (i = 0; i < A384; i++)
		scr384[i] = (int8_t)Z_SCR384;
	memset(box512, (int8_t)Z_BOX512, sizeof box512);
	memset(box384, (int8_t)Z_BOX384, sizeof box384);

	memset(tens, 0, sizeof tens);
	set_tensor(&tens[0], box512, sizeof box512, A512, STRIDE, S_BOX512, Z_BOX512);
	set_tensor(&tens[1], scr512, sizeof scr512, A512, 1,      S_SCR512, Z_SCR512);
	set_tensor(&tens[2], box384, sizeof box384, A384, STRIDE, S_BOX384, Z_BOX384);
	set_tensor(&tens[3], scr384, sizeof scr384, A384, 1,      S_SCR384, Z_SCR384);
}

/*
 * A face at one anchor of the 16x16 group.
 *
 * The regressors are in INPUT PIXELS, so q 18 against zero point -47 is
 * (18 + 47) * 0.3067 = 19.9 px of width and height on a 128 px input -- small
 * enough that anchors several cells apart do not merge under NMS, which is what
 * lets this file check ORDER as well as count.
 */
static void put_face_512(int anchor, int8_t score_q)
{
	scr512[anchor] = score_q;
	box512[anchor * STRIDE + 0] = (int8_t)Z_BOX512;   /* dx = 0 */
	box512[anchor * STRIDE + 1] = (int8_t)Z_BOX512;   /* dy = 0 */
	box512[anchor * STRIDE + 2] = 18;                 /* w      */
	box512[anchor * STRIDE + 3] = 18;                 /* h      */
}

static void put_face_384(int anchor, int8_t score_q)
{
	scr384[anchor] = score_q;
	box384[anchor * STRIDE + 0] = (int8_t)Z_BOX384;
	box384[anchor * STRIDE + 1] = (int8_t)Z_BOX384;
	box384[anchor * STRIDE + 2] = (int8_t)(Z_BOX384 + 17);   /* ~20 px */
	box384[anchor * STRIDE + 3] = (int8_t)(Z_BOX384 + 17);
}

/* Three faces spread across the 16x16 grid plus one in the 8x8 group, so both
 * anchor groups are scanned and the result has an order to compare. */
static void put_the_scene(void)
{
	put_face_512(2 * (1 * 16 + 1), 127);    /* cell (1,1)   */
	put_face_512(2 * (3 * 16 + 8), 110);    /* cell (8,3)   */
	put_face_512(2 * (12 * 16 + 13), 90);   /* cell (13,12) */
	put_face_384(6 * (6 * 8 + 2), 127);     /* 8x8 cell (2,6) */
}

/* ---- a recording painter -------------------------------------------------- */

/*
 * NOT port/plugin/plugin_paint.c: that one owns the budget and the clipping and
 * has its own test.  What is wanted here is the plugin's REQUEST -- the
 * rectangle it asked for -- because that is the thing to hold against the
 * resident decoder's box.
 */
struct rec_rect {
	struct plugin_rect r;
	uint16_t rgb, stroke;
};

static struct rec_rect rec_v[BF_MAX_DET * 4];
static unsigned        rec_n;
static unsigned        rec_other;   /* fill_rect / blit calls */

static void rec_rect_fn(void *ctx, const struct plugin_rect *r, uint16_t rgb,
                        uint16_t stroke)
{
	(void)ctx;
	if (rec_n < (unsigned)(sizeof rec_v / sizeof rec_v[0])) {
		rec_v[rec_n].r      = *r;
		rec_v[rec_n].rgb    = rgb;
		rec_v[rec_n].stroke = stroke;
	}
	rec_n++;
}

static void rec_fill_fn(void *ctx, const struct plugin_rect *r, uint16_t rgb)
{
	(void)ctx; (void)r; (void)rgb;
	rec_other++;
}

static void rec_blit_fn(void *ctx, const struct plugin_rect *r,
                        const uint16_t *src, uint32_t stride, int32_t key)
{
	(void)ctx; (void)r; (void)src; (void)stride; (void)key;
	rec_other++;
}

static struct plugin_painter rec_painter = {
	NULL, rec_rect_fn, rec_fill_fn, rec_blit_fn
};

static void rec_reset(void)
{
	rec_n = 0u;
	rec_other = 0u;
	memset(rec_v, 0, sizeof rec_v);
}

/* ---- a capturing printer -------------------------------------------------- */

static char     cap_buf[8192];
static size_t   cap_len;
static unsigned cap_calls;
static int      cap_fail_at = -1;   /* refuse the Nth write (0-based) */

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

static void cap_reset(void)
{
	cap_len = 0u;
	cap_calls = 0u;
	cap_fail_at = -1;
	cap_buf[0] = '\0';
}

/* ---- the base vtable the plugin is handed --------------------------------- */

static unsigned base_logs;

static void base_log(void *ctx, const char *s, size_t len)
{
	(void)ctx; (void)s; (void)len;
	base_logs++;
}

/*
 * [!] THE REAL nn_active_to_frame, not a stand-in.  It is what nn_svc_grove.c
 * puts in this slot, and the transform being the SAME ONE the resident path
 * uses is precisely the property under test.
 */
static const struct plugin_base_api base_api = {
	PLUGIN_ABI_VERSION,
	(uint32_t)sizeof(struct plugin_base_api),
	NULL,
	base_log,
	nn_active_to_frame,
};

/* ---- helpers -------------------------------------------------------------- */

/* The geometry `nn run` and the overlay both publish: a 240x240 centre square
 * of the 320x240 frame, scaled to a 128x128 model input. */
static struct nn_preproc_geom the_geom;

static void publish_geom(void)
{
	int rc = nn_preproc_geom(320u, 240u, 128u, 128u, &the_geom);

	if (rc != 0) {
		printf("  FAIL geometry setup returned %d\n", rc);
		failures++;
	}
	nn_active_set_geom(&the_geom);
}

/* Poison for the arrays a plugin must not touch. */
#define POISON 0x5A

static struct bf_det    det[BF_MAX_DET];
static struct bf_result res;

static void poison_out(void)
{
	memset(det, POISON, sizeof det);
	memset(&res, POISON, sizeof res);
}

static int out_still_poisoned(void)
{
	unsigned i;
	const uint8_t *p = (const uint8_t *)det;

	for (i = 0u; i < sizeof det; i++)
		if (p[i] != POISON)
			return 0;
	p = (const uint8_t *)&res;
	for (i = 0u; i < sizeof res; i++)
		if (p[i] != POISON)
			return 0;
	return 1;
}

int main(void)
{
	struct bf_det    ref_det[BF_MAX_DET];
	struct bf_result ref_res;
	int ref_n, n, i;
	unsigned resident_thresh;

	printf("test_plugin_decode\n");

	/* ================================================================
	 * 1.  With no plugin, everything routes to the resident decoder
	 * ================================================================ */
	pl_loaded = 0;
	reset_tensors();
	put_the_scene();
	publish_geom();

	expect("no plugin loaded: the shim says so",
	       nn_active_is_plugin() == 0, "claims a plugin");
	expect("shapes_ok is the resident answer",
	       nn_active_shapes_ok(tens, 4) == nn_decoder_shapes_ok(tens, 4),
	       "disagree");

	memset(det, 0, sizeof det);
	memset(&res, 0, sizeof res);
	n = nn_active_decode(tens, 4, det, BF_MAX_DET, &res);
	expect("the resident decode runs and fills the boxes in",
	       n > 0 && res.status == BF_OK && res.thresh_milli != 0u,
	       "n %d status %d thresh %u", n, res.status, res.thresh_milli);

	rec_reset();
	nn_active_draw(&rec_painter);
	expect("and the shim paints nothing -- the base draws those boxes",
	       rec_n == 0u && rec_other == 0u, "%u rect(s), %u other", rec_n,
	       rec_other);

	cap_reset();
	expect("and reports nothing, successfully",
	       nn_active_report(cap_write, NULL) == 0 && cap_len == 0u,
	       "wrote %zu B", cap_len);

	expect("the threshold read back is the resident one",
	       nn_active_get_thresh_milli() == nn_decoder_get_thresh_milli(),
	       "%u vs %u", nn_active_get_thresh_milli(),
	       nn_decoder_get_thresh_milli());

	/* Keep the reference for the differential section below, decoded at the
	 * DEFAULT threshold and with the geometry published. */
	ref_n = nn_decoder_run(tens, 4, ref_det, BF_MAX_DET, &ref_res);
	expect("the reference decode found more than one face",
	       ref_n > 1, "n %d -- the scene is not exercising order", ref_n);
	resident_thresh = nn_decoder_get_thresh_milli();

	/* ================================================================
	 * 2.  Load the plugin -- its own entry point, with the real vtable
	 * ================================================================ */
	{
		plugin_entry_fn entry =
			(plugin_entry_fn)(uintptr_t)plugin_slot_table[PLUGIN_SLOT_ENTRY];

		/* pl_loaded stays 0 across the call, as pl_started does on the board:
		 * a plugin is not active until its own entry point has accepted. */
		expect("the plugin accepts the base vtable", entry(&base_api) == 0,
		       "refused");
		pl_loaded = 1;
	}
	expect("and the shim now routes to it", nn_active_is_plugin() == 1,
	       "still resident");

	/* A base whose size does not match is refused rather than called through:
	 * the plugin checks version and size before it touches a member. */
	{
		plugin_entry_fn entry =
			(plugin_entry_fn)(uintptr_t)plugin_slot_table[PLUGIN_SLOT_ENTRY];
		struct plugin_base_api shorter = base_api;

		shorter.size = (uint32_t)sizeof(struct plugin_base_api) - 4u;
		expect("a base of the wrong size is refused", entry(&shorter) != 0,
		       "accepted");
		shorter = base_api;
		shorter.version = PLUGIN_ABI_VERSION + 1u;
		expect("so is a base of the wrong ABI version", entry(&shorter) != 0,
		       "accepted");
		/* Put it back the way the rest of this file needs it. */
		expect("and re-entry with the right vtable succeeds",
		       entry(&base_api) == 0, "refused");
	}

	/* ================================================================
	 * 3.  The plugin decodes, and leaves the resident output alone
	 * ================================================================ */
	poison_out();
	n = nn_active_decode(tens, 4, det, BF_MAX_DET, &res);
	expect("the plugin decode returns the same count as the resident one",
	       n == ref_n, "plugin %d, resident %d", n, ref_n);
	expect("[!] and did not write the caller's boxes or diagnostics",
	       out_still_poisoned(), "the shim let a plugin fill them in");

	expect("shapes_ok goes to the plugin and agrees",
	       nn_active_shapes_ok(tens, 4) != 0, "the plugin refused the tensors");

	/* ================================================================
	 * 4.  Differential: the plugin's boxes, through draw()
	 * ================================================================ */
	rec_reset();
	nn_active_draw(&rec_painter);
	expect("the plugin drew one rectangle per detection",
	       rec_n == (unsigned)ref_n && rec_other == 0u,
	       "%u rect(s) for %d face(s), %u other", rec_n, ref_n, rec_other);

	for (i = 0; i < ref_n && (unsigned)i < rec_n; i++) {
		struct nn_preproc_box b;
		int ok = nn_preproc_box(&the_geom, ref_det[i].x, ref_det[i].y,
		                        ref_det[i].w, ref_det[i].h, &b) == 0;

		expect("the box is representable in frame pixels", ok, "face %d", i);
		if (!ok)
			continue;
		expect("[!] the plugin's rectangle is the resident one, in order",
		       rec_v[i].r.x0 == b.x0 && rec_v[i].r.y0 == b.y0 &&
		       rec_v[i].r.x1 == b.x1 && rec_v[i].r.y1 == b.y1,
		       "face %d: plugin (%d,%d)-(%d,%d) resident (%d,%d)-(%d,%d)", i,
		       rec_v[i].r.x0, rec_v[i].r.y0, rec_v[i].r.x1, rec_v[i].r.y1,
		       b.x0, b.y0, b.x1, b.y1);
	}

	/* ================================================================
	 * 5.  Differential: the plugin's scores, through report()
	 * ================================================================ */
	cap_reset();
	expect("the plugin's report succeeds",
	       nn_active_report(cap_write, NULL) == 0, "refused");

	{
		char want[64];

		snprintf(want, sizeof want, "faces %d ", ref_n);
		expect("and names the same number of faces", strstr(cap_buf, want) != NULL,
		       "no '%s' in:\n%s", want, cap_buf);

		snprintf(want, sizeof want, "thresh %u/1000", resident_thresh);
		expect("at the threshold the decode applied",
		       strstr(cap_buf, want) != NULL, "no '%s' in:\n%s", want, cap_buf);

		for (i = 0; i < ref_n; i++) {
			snprintf(want, sizeof want, "score %u\r\n",
			         (unsigned)(ref_det[i].score * 1000.0f));
			expect("[!] and the resident score, in milli, for every face",
			       strstr(cap_buf, want) != NULL,
			       "face %d: no '%s' in:\n%s", i, want, cap_buf);
		}
		/* The box the report prints is the drawn one, so the console and the
		 * panel cannot disagree -- the bug hardware found in step 7. */
		snprintf(want, sizeof want, "x %d y %d", rec_v[0].r.x0 < 0 ? 0 : rec_v[0].r.x0,
		         rec_v[0].r.y0 < 0 ? 0 : rec_v[0].r.y0);
		expect("the printed box is the drawn box",
		       strstr(cap_buf, want) != NULL, "no '%s' in:\n%s", want, cap_buf);
	}

	/* ================================================================
	 * 6.  A refused writer stops the report where it was refused
	 * ================================================================ */
	cap_reset();
	cap_fail_at = 2;
	expect("[!] a writer that refuses is propagated, not written past",
	       nn_active_report(cap_write, NULL) < 0, "reported success");
	expect("and nothing was written after the refusal",
	       cap_calls == 3u, "%u write(s) -- it carried on", cap_calls);

	/* ================================================================
	 * 7.  [!] The threshold follows the decoder that will use it
	 * ================================================================ */
	expect("with a plugin loaded, the threshold read back is the plugin's",
	       nn_active_get_thresh_milli() == resident_thresh,
	       "%u vs %u -- they start equal", nn_active_get_thresh_milli(),
	       resident_thresh);

	expect("the shim accepts a new threshold",
	       nn_active_set_thresh_milli(800u) != 0, "refused");
	expect("and reads it back", nn_active_get_thresh_milli() == 800u,
	       "got %u", nn_active_get_thresh_milli());
	expect("[!] while the RESIDENT decoder's threshold has not moved",
	       nn_decoder_get_thresh_milli() == resident_thresh,
	       "resident is now %u -- the shim wrote the wrong decoder",
	       nn_decoder_get_thresh_milli());

	/* And it is the number the next decode really applies. */
	cap_reset();
	(void)nn_active_decode(tens, 4, det, BF_MAX_DET, &res);
	(void)nn_active_report(cap_write, NULL);
	expect("[!] and the plugin's next decode applies it",
	       strstr(cap_buf, "thresh 800/1000") != NULL,
	       "the report says otherwise:\n%s", cap_buf);

	expect("an out-of-range threshold is refused through the shim too",
	       nn_active_set_thresh_milli(1000u) == 0, "accepted");
	expect("and changed nothing", nn_active_get_thresh_milli() == 800u,
	       "got %u", nn_active_get_thresh_milli());

	/* Unloading gives the question back to the resident decoder, with the
	 * value it has had all along. */
	pl_loaded = 0;
	expect("unloaded, the threshold is the resident one again",
	       nn_active_get_thresh_milli() == resident_thresh,
	       "got %u, want %u", nn_active_get_thresh_milli(), resident_thresh);
	pl_loaded = 1;
	(void)nn_active_set_thresh_milli(resident_thresh);

	/* ================================================================
	 * 8.  [!] No geometry: nothing is drawn and the report says so
	 * ================================================================
	 * This is the hardware failure of implementation step 7 -- the plugin's
	 * transform was wired to the stream's geometry, so `nn run`, which
	 * publishes a different one, produced "outside the frame" every time.
	 */
	nn_active_clear_geom();
	(void)nn_active_decode(tens, 4, det, BF_MAX_DET, &res);
	rec_reset();
	nn_active_draw(&rec_painter);
	expect("[!] with no published geometry a plugin draws nothing",
	       rec_n == 0u, "%u rectangle(s) at unknown coordinates", rec_n);

	cap_reset();
	(void)nn_active_report(cap_write, NULL);
	expect("and says so rather than printing a box it could not map",
	       strstr(cap_buf, "outside the frame") != NULL, "report was:\n%s",
	       cap_buf);
	publish_geom();

	/* ================================================================
	 * 9.  The two decoders agree about REFUSALS, not just successes
	 * ================================================================
	 * [!] The codes are not interchangeable (issue #97): BF_ERR_MODEL means
	 * "open a different model" and the rest mean the firmware is wired wrong.
	 * A plugin that folded them would send somebody to check a model that is
	 * fine, so what is compared here is the code and not merely the sign.
	 */
	{
		unsigned c;

		/* (a) an element type the board cannot vouch for */
		reset_tensors();
		put_the_scene();
		tens[1].type = (int8_t)(T_INT8 + 1);
		pl_loaded = 0;
		ref_n = nn_decoder_run(tens, 4, ref_det, BF_MAX_DET, &ref_res);
		pl_loaded = 1;
		poison_out();
		n = nn_active_decode(tens, 4, det, BF_MAX_DET, &res);
		expect("a non-int8 tensor: both refuse with the same code",
		       n == ref_n && n == BF_ERR_MODEL, "plugin %d, resident %d", n,
		       ref_n);
		expect("and the plugin still left the caller's arrays alone",
		       out_still_poisoned(), "it wrote them");
		expect("both refuse it in shapes_ok as well",
		       nn_active_shapes_ok(tens, 4) == 0, "the plugin accepted it");

		/* (b) the rank-0 "not representable" marker */
		reset_tensors();
		put_the_scene();
		tens[3].rank = 0;
		pl_loaded = 0;
		ref_n = nn_decoder_run(tens, 4, ref_det, BF_MAX_DET, &ref_res);
		pl_loaded = 1;
		n = nn_active_decode(tens, 4, det, BF_MAX_DET, &res);
		expect("a rank-0 tensor: both refuse with the same code",
		       n == ref_n && n == BF_ERR_MODEL, "plugin %d, resident %d", n,
		       ref_n);

		/* (c) a refusal the plugin has to REPORT, since it has no boxes */
		cap_reset();
		expect("the plugin reports the refusal rather than zero faces",
		       nn_active_report(cap_write, NULL) == 0 &&
		       strstr(cap_buf, "decode refused") != NULL, "report was:\n%s",
		       cap_buf);
		rec_reset();
		nn_active_draw(&rec_painter);
		expect("and draws nothing for it",
		       rec_n == 0u, "%u rectangle(s) from a failed decode", rec_n);

		/* (d) a null tensor array.  Unreachable from either caller in the
		 * firmware -- which is why the two branches could have disagreed about
		 * it forever; the resident one checks and a plugin cannot. */
		for (c = 0u; c < 2u; c++) {
			pl_loaded = (int)c;
			memset(&res, 0, sizeof res);
			n = nn_active_decode(NULL, 4, det, BF_MAX_DET, &res);
			expect("a null tensor array is an argument error either way",
			       n == BF_ERR_ARG && res.status == BF_ERR_ARG,
			       "%s: n %d status %d", c ? "plugin" : "resident", n,
			       res.status);
			expect("and shapes_ok refuses it either way",
			       nn_active_shapes_ok(NULL, 4) == 0, "%s: accepted",
			       c ? "plugin" : "resident");
		}
		pl_loaded = 1;
	}

	/* ================================================================
	 * 10.  An oversized output count is bounded, on both paths
	 * ================================================================ */
	reset_tensors();
	put_the_scene();
	pl_loaded = 0;
	ref_n = nn_decoder_run(tens, NN_DECODER_MAX_OUTPUTS + 4u, ref_det,
	                       BF_MAX_DET, &ref_res);
	pl_loaded = 1;
	n = nn_active_decode(tens, NN_DECODER_MAX_OUTPUTS + 4u, det, BF_MAX_DET,
	                     &res);
	expect("an output count past the descriptor array is clamped, not walked",
	       n == ref_n && n > 0, "plugin %d, resident %d", n, ref_n);

	expect("the plugin never needed the log channel for a good decode",
	       base_logs == 0u, "%u log line(s)", base_logs);

	if (failures) {
		printf("test_plugin_decode: %d failure(s)\n", failures);
		return 1;
	}
	printf("test_plugin_decode: all cases pass\n");
	return 0;
}
