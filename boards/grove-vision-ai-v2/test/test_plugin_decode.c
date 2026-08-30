/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    test_plugin_decode.c
 * @brief   The active-decoder shim, and the plugin decoder against a reference
 *          this file builds (issues #103, #104).
 *
 * [!] WHAT THIS COMPARES CHANGED WITH ISSUE #104, AND SO DID THE CLAIM.  It used
 * to hold two decoders that both shipped -- the firmware's resident adapter and
 * the plugin -- and to check they agreed.  The firmware no longer HAS a decoder,
 * so the second half is now a reference this test builds for itself, directly on
 * svc/blazeface.c.  That is a weaker statement about the product (nothing is
 * being cross-checked against another shipped implementation) and it is still
 * worth making: the plugin's wrapper, not the arithmetic, is what this catches.
 *
 * A test whose stated subject and contents disagree is worse than one with no
 * comment at all, which is why this paragraph exists rather than a rename.
 *
 * [!] WHY THE SHARED ARITHMETIC DOES NOT MAKE IT TAUTOLOGICAL.  The reference
 * and the plugin both call one blazeface_decode(), so of course they agree about
 * anchors and NMS -- that is settled in shell/test/test_blazeface.c.  What is
 * tested is everything AROUND it, and each has a way to be wrong that hardware
 * reports as a working preview:
 *
 *   - the plugin holds its OWN state and its OWN threshold.  Since issue #104
 *     the firmware has none to diverge from, so what is checked is that the shim
 *     reaches the plugin's and that there is no second number anywhere: with no
 *     plugin loaded the shim reports NN_SVC_THRESH_NONE rather than borrowing
 *     one.  Setting through the SHIM and reading the reference is what would
 *     catch a threshold that went somewhere else;
 *   - the plugin's result is PRIVATE.  It comes back only through draw() and
 *     report(), so those are the only surfaces on which its boxes can be
 *     compared with the reference's at all;
 *   - the coordinate transform is published separately from the decode, and
 *     wiring it to the wrong publisher is what made every box `nn run` produced
 *     come back "outside the frame" on hardware.  Here that is a case, not a
 *     session next to the old output;
 *   - with NO plugin the shim must REFUSE rather than decode.  On the board
 *     nothing should reach that backstop -- the service adapter decides
 *     plugin-or-raw in one helper and the stream refuses admission -- but a shim
 *     that quietly decoded instead would be invisible from outside.
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
 * test_npu_desc.c defines the first: on the board it lives in npu_tflm.cc, the
 * one translation unit that can see TfLiteType.
 */
#include "nn_active.h"
#include "npu_desc.h"
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
 * The real model's quantisation, as in test_npu_desc.c and the core decoder
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

static struct npu_tensor tens[NPU_DESC_MAX_OUTPUTS];

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

/* ---- the reference decoder, built HERE (issue #104) ----------------------
 *
 * It used to be port/npu/nn_decoder.c, which the firmware linked.  With that
 * gone this is the test's own instance of the same shared arithmetic: its own
 * state, its own scratch, its own threshold, reached by nobody else.  That
 * independence is the point -- a threshold set through the shim must not move
 * it, and if it ever did, the shim would be writing somewhere unexpected.
 */
static struct bf_cand   ref_scratch[BF_MAX_CAND];
static struct blazeface ref_bf;

static unsigned ref_to_desc(const struct npu_tensor *outs, unsigned n,
                            struct tensor_desc *d, unsigned cap)
{
	unsigned i;

	if (n > cap)
		n = cap;
	for (i = 0u; i < n; i++)
		npu_desc_of(&d[i], &outs[i]);
	return n;
}

static int ref_shapes_ok(const struct npu_tensor *outs, unsigned n)
{
	struct tensor_desc d[NPU_DESC_MAX_OUTPUTS];
	unsigned m;

	if (outs == NULL)
		return 0;
	m = ref_to_desc(outs, n, d, NPU_DESC_MAX_OUTPUTS);
	return blazeface_shapes_ok(d, m);
}

static int ref_run(const struct npu_tensor *outs, unsigned n,
                   struct bf_det *out, int max, struct bf_result *r)
{
	struct tensor_desc d[NPU_DESC_MAX_OUTPUTS];
	unsigned m;

	if (outs == NULL) {
		memset(r, 0, sizeof(*r));
		r->status = BF_ERR_ARG;
		return BF_ERR_ARG;
	}
	m = ref_to_desc(outs, n, d, NPU_DESC_MAX_OUTPUTS);
	return blazeface_decode(&ref_bf, d, m, out, max, r);
}

int main(void)
{
	struct bf_det    ref_det[BF_MAX_DET];
	struct bf_result ref_res;
	int ref_n, n, i;
	unsigned ref_thresh;

	printf("test_plugin_decode\n");

	/* ================================================================
	 * 1.  With no plugin there is NO DECODER (issue #104)
	 * ================================================================
	 *
	 * [!] THE SHIM MUST REFUSE, NOT DECODE.  Nothing on the board should reach
	 * this -- the service adapter picks plugin-or-raw in the one helper both its
	 * console callers go through, and the stream refuses admission before a
	 * camera is lit -- so a shim that quietly decoded here would be invisible
	 * from outside.  It is checked precisely because it is unreachable.
	 */
	pl_loaded = 0;
	reset_tensors();
	put_the_scene();
	publish_geom();

	expect("the reference decoder initialises",
	       blazeface_init(&ref_bf, ref_scratch, sizeof ref_scratch) == BF_OK,
	       "refused");

	expect("no plugin loaded: the shim says so",
	       nn_active_is_plugin() == 0, "claims a plugin");
	expect("and it reads no shape, even one the reference accepts",
	       nn_active_shapes_ok(tens, 4) == 0 && ref_shapes_ok(tens, 4) != 0,
	       "shim %d, reference %d", nn_active_shapes_ok(tens, 4),
	       ref_shapes_ok(tens, 4));

	n = nn_active_decode(tens, 4);
	expect("a decode says no decoder is bound, and does not say 'not a "
	       "detector'", n == BF_ERR_UNINIT, "got %d", n);

	rec_reset();
	nn_active_draw(&rec_painter);
	expect("nothing paints", rec_n == 0u && rec_other == 0u,
	       "%u rect(s), %u other", rec_n, rec_other);
	expect("and a stream would be refused for having nothing to draw",
	       nn_active_can_draw() == 0, "claims it draws");

	cap_reset();
	expect("and reports nothing, successfully",
	       nn_active_report(cap_write, NULL) == 0 && cap_len == 0u,
	       "wrote %zu B", cap_len);

	expect("the threshold is reported absent, not borrowed from anywhere",
	       nn_active_get_thresh_milli() == NN_SVC_THRESH_NONE, "%u",
	       nn_active_get_thresh_milli());
	expect("and setting one is refused as a state, not as a bad value",
	       nn_active_set_thresh_milli(700u) == NN_ACTIVE_THRESH_NO_DECODER,
	       "got %d", nn_active_set_thresh_milli(700u));

	/* Keep the reference for the differential section below, decoded at the
	 * DEFAULT threshold and with the geometry published. */
	ref_n = ref_run(tens, 4, ref_det, BF_MAX_DET, &ref_res);
	expect("the reference decode found more than one face",
	       ref_n > 1, "n %d -- the scene is not exercising order", ref_n);
	ref_thresh = blazeface_get_thresh_milli(&ref_bf);

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
	 * 3.  The plugin decodes, and the caller's boxes stay untouched
	 * ================================================================
	 *
	 * [!] SINCE ISSUE #104 THE SIGNATURE IS WHAT GUARANTEES THAT, not this
	 * check: nn_active_decode() no longer TAKES a box array, because with no
	 * decoder in the firmware nothing is left to fill one.  The poison stays
	 * because it is free and because it pins the property the signature now
	 * carries -- if a box array ever comes back, this says what it may not do.
	 */
	poison_out();
	n = nn_active_decode(tens, 4);
	expect("the plugin decode returns the same count as the reference",
	       n == ref_n, "plugin %d, reference %d", n, ref_n);
	expect("[!] and nothing wrote the caller's boxes or diagnostics",
	       out_still_poisoned(), "something filled them in");

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

		snprintf(want, sizeof want, "thresh %u/1000", ref_thresh);
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
	       nn_active_get_thresh_milli() == ref_thresh,
	       "%u vs %u -- they start equal", nn_active_get_thresh_milli(),
	       ref_thresh);

	expect("the shim accepts a new threshold",
	       nn_active_set_thresh_milli(800u) == NN_ACTIVE_THRESH_OK, "refused");
	expect("and reads it back", nn_active_get_thresh_milli() == 800u,
	       "got %u", nn_active_get_thresh_milli());
	expect("[!] while the reference decoder's threshold has not moved",
	       blazeface_get_thresh_milli(&ref_bf) == ref_thresh,
	       "reference is now %u -- the shim wrote somewhere unexpected",
	       blazeface_get_thresh_milli(&ref_bf));

	/* And it is the number the next decode really applies. */
	cap_reset();
	(void)nn_active_decode(tens, 4);
	(void)nn_active_report(cap_write, NULL);
	expect("[!] and the plugin's next decode applies it",
	       strstr(cap_buf, "thresh 800/1000") != NULL,
	       "the report says otherwise:\n%s", cap_buf);

	expect("an out-of-range threshold is refused through the shim too",
	       nn_active_set_thresh_milli(1000u) == NN_ACTIVE_THRESH_REFUSED,
	       "accepted");
	expect("and changed nothing", nn_active_get_thresh_milli() == 800u,
	       "got %u", nn_active_get_thresh_milli());

	/* [!] AND UNLOADING LEAVES NOTHING HOLDING ONE (issue #104).  It used to
	 * hand the question back to the resident decoder; there is no such thing to
	 * hand it to, and reporting the plugin's last value would describe a setting
	 * nothing would apply. */
	pl_loaded = 0;
	expect("unloaded, the threshold is absent rather than the plugin's last",
	       nn_active_get_thresh_milli() == NN_SVC_THRESH_NONE, "got %u",
	       nn_active_get_thresh_milli());
	pl_loaded = 1;
	(void)nn_active_set_thresh_milli(ref_thresh);

	/* ================================================================
	 * 8.  [!] No geometry: nothing is drawn and the report says so
	 * ================================================================
	 * This is the hardware failure of implementation step 7 -- the plugin's
	 * transform was wired to the stream's geometry, so `nn run`, which
	 * publishes a different one, produced "outside the frame" every time.
	 */
	nn_active_clear_geom();
	(void)nn_active_decode(tens, 4);
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
		ref_n = ref_run(tens, 4, ref_det, BF_MAX_DET, &ref_res);
		pl_loaded = 1;
		poison_out();
		n = nn_active_decode(tens, 4);
		expect("a non-int8 tensor: both refuse with the same code",
		       n == ref_n && n == BF_ERR_MODEL, "plugin %d, reference %d", n,
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
		ref_n = ref_run(tens, 4, ref_det, BF_MAX_DET, &ref_res);
		pl_loaded = 1;
		n = nn_active_decode(tens, 4);
		expect("a rank-0 tensor: both refuse with the same code",
		       n == ref_n && n == BF_ERR_MODEL, "plugin %d, reference %d", n,
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
		 * firmware, which is why the answer has to be checked here: a plugin
		 * cannot defend against it -- to_desc() would walk the pointer -- so the
		 * shim does it ONCE, ahead of the branch.  Both settings of pl_loaded
		 * are exercised because the value of putting the check before the branch
		 * is precisely that the two cannot answer differently; move it into a
		 * branch and this is what notices.
		 *
		 * [!] It is BF_ERR_ARG, not the no-decoder code: the caller's mistake is
		 * a different thing from there being nothing loaded. */
		for (c = 0u; c < 2u; c++) {
			pl_loaded = (int)c;
			n = nn_active_decode(NULL, 4);
			expect("a null tensor array is an argument error either way",
			       n == BF_ERR_ARG, "%s: n %d",
			       c ? "plugin loaded" : "no plugin", n);
			expect("and shapes_ok refuses it either way",
			       nn_active_shapes_ok(NULL, 4) == 0, "%s: accepted",
			       c ? "plugin loaded" : "no plugin");
		}
		pl_loaded = 1;
	}

	/* ================================================================
	 * 10.  An oversized output count is bounded, on both paths
	 * ================================================================ */
	reset_tensors();
	put_the_scene();
	pl_loaded = 0;
	ref_n = ref_run(tens, NPU_DESC_MAX_OUTPUTS + 4u, ref_det, BF_MAX_DET,
	                &ref_res);
	pl_loaded = 1;
	n = nn_active_decode(tens, NPU_DESC_MAX_OUTPUTS + 4u);
	expect("an output count past the descriptor array is clamped, not walked",
	       n == ref_n && n > 0, "plugin %d, reference %d", n, ref_n);

	expect("the plugin never needed the log channel for a good decode",
	       base_logs == 0u, "%u log line(s)", base_logs);

	if (failures) {
		printf("test_plugin_decode: %d failure(s)\n", failures);
		return 1;
	}
	printf("test_plugin_decode: all cases pass\n");
	return 0;
}
