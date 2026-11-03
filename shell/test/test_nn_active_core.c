/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Host tests for svc/nn/nn_active_core.c (issue #126): the one place that decides
 * whether a plugin decodes, shared by grove-vision-ai-v2 and wio-lite-ai.
 *
 * The plugin here is a set of counting stubs, not a real decoder: what is
 * pinned is the ROUTING -- which slot is called, with what, how often, and
 * what is answered when none is.  Each board's own test drives its port
 * nn_active.c through this file with the real blazeface plugin
 * (grove-vision-ai-v2 test_plugin_decode.c, wio-lite-ai test_nn_active.c).
 *
 * [!] THE NO-PLUGIN ANSWER TO shapes_ok IS THE BOARD'S, AND IS CHECKED BOTH
 * WAYS.  grove-vision-ai-v2 refuses and wio-lite-ai accepts, on purpose; a
 * shared file that hard-coded either would pass half of this.  And with a
 * plugin loaded neither board's answer may leak through: the plugin's is the
 * only one.
 *
 * [!] EVERY CALL INTO A SLOT IS PRECEDED BY EXACTLY ONE DEPTH SAMPLE FOR THAT
 * SLOT, AND A QUESTION THAT CALLS NOTHING SAMPLES NOTHING.  A sample of a call
 * that did not happen is a depth nobody was ever at.
 */
#include "nn_active_core.h"
#include "plugin_bindings.h"

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

/* ---- the plugin: counting stubs ------------------------------------------ */

static unsigned calls[PLUGIN_SLOT_COUNT];
static unsigned notes[PLUGIN_SLOT_COUNT];
static int noted_before;          /* the slot's note came before its call */
static int answer;                /* what the next slot call returns      */
static const struct tensor_desc *seen_d;
static unsigned seen_n;
static const void *seen_p;
static uint32_t seen_id, seen_value;
static struct plugin_printer seen_out;

static void called(unsigned slot)
{
	calls[slot]++;
	noted_before = notes[slot] == calls[slot];
}

static int s_shapes(const struct tensor_desc *d, unsigned n)
{
	called(PLUGIN_SLOT_SHAPES_OK);
	seen_d = d; seen_n = n;
	return answer;
}

static int s_decode(const struct tensor_desc *d, unsigned n)
{
	called(PLUGIN_SLOT_DECODE);
	seen_d = d; seen_n = n;
	return answer;
}

static struct plugin_painter seen_paint;

static void s_draw(const struct plugin_painter *p)
{
	called(PLUGIN_SLOT_DRAW);
	seen_p = p;
	seen_paint = *p;         /* it lives in the shared draw's frame */
}

static int s_report(const struct plugin_printer *out)
{
	called(PLUGIN_SLOT_REPORT);
	seen_out = *out;
	return answer;
}

static int s_set(uint32_t id, uint32_t value)
{
	called(PLUGIN_SLOT_PARAM_SET);
	seen_id = id; seen_value = value;
	return answer;
}

static int s_get(uint32_t id, uint32_t *value)
{
	called(PLUGIN_SLOT_PARAM_GET);
	seen_id = id;
	*value = 642u;
	return answer;
}

/* ---- what a board hands the shared file ---------------------------------- */

static int   live;                               /* plugin_run_active()   */
static void *table[PLUGIN_SLOT_COUNT];           /* plugin_run_slot()     */

static int b_active(void)
{
	return live;
}

static void *b_slot(unsigned slot)
{
	return slot < (unsigned)PLUGIN_SLOT_COUNT ? table[slot] : NULL;
}

static void b_note(unsigned slot, uintptr_t sp)
{
	(void)sp;
	if (slot < (unsigned)PLUGIN_SLOT_COUNT)
		notes[slot]++;
}

static const struct nn_active_board refuses = {   /* grove-vision-ai-v2 */
	b_active, b_slot, b_note, 0
};
static const struct nn_active_board accepts = {   /* wio-lite-ai */
	b_active, b_slot, b_note, 1
};

static void load_all(void)
{
	memset(table, 0, sizeof table);
	table[PLUGIN_SLOT_SHAPES_OK] = (void *)s_shapes;
	table[PLUGIN_SLOT_DECODE]    = (void *)s_decode;
	table[PLUGIN_SLOT_DRAW]      = (void *)s_draw;
	table[PLUGIN_SLOT_REPORT]    = (void *)s_report;
	table[PLUGIN_SLOT_PARAM_SET] = (void *)s_set;
	table[PLUGIN_SLOT_PARAM_GET] = (void *)s_get;
	live = 1;
}

static void reset(void)
{
	memset(calls, 0, sizeof calls);
	memset(notes, 0, sizeof notes);
	noted_before = 0;
	seen_d = NULL; seen_n = 0u; seen_p = NULL;
	seen_id = 0xFFFFFFFFu; seen_value = 0u;
	memset(&seen_out, 0, sizeof seen_out);
}

static unsigned total(const unsigned *v)
{
	unsigned i, n = 0u;

	for (i = 0u; i < (unsigned)PLUGIN_SLOT_COUNT; i++)
		n += v[i];
	return n;
}

/* Exactly one call of @p slot, one note of it before, and nothing else. */
static int only(unsigned slot)
{
	unsigned i;

	for (i = 0u; i < (unsigned)PLUGIN_SLOT_COUNT; i++)
		if (calls[i] != (i == slot ? 1u : 0u) ||
		    notes[i] != (i == slot ? 1u : 0u))
			return 0;
	return noted_before;
}

static int w_write(void *ctx, const char *s, size_t len)
{
	(void)ctx; (void)s;
	return (int)len;
}

static void t_rect(void *c, const struct plugin_rect *r, uint16_t a,
                   uint16_t b)
{ (void)c; (void)r; (void)a; (void)b; }
static void t_fill(void *c, const struct plugin_rect *r, uint16_t a)
{ (void)c; (void)r; (void)a; }
static void t_blit(void *c, const struct plugin_rect *r, const uint16_t *s,
                   uint32_t st, int32_t k)
{ (void)c; (void)r; (void)s; (void)st; (void)k; }

/* The board's table (issue #165): the printer's write and the painter's three
 * come from here. */
const struct plugin_bindings plugin_board_bindings = {
	.rect = t_rect, .fill_rect = t_fill, .blit = t_blit, .write = w_write,
};

/* The drawing target a board would hand (plugin_paint_target()). */
static int canvas;

static struct tensor_desc d[4];

/* ---- nothing to call ----------------------------------------------------- */

static void test_nobody(const char *state)
{
	const struct nn_active_board *const bs[2] = { &refuses, &accepts };
	char what[160];
	unsigned i;

	for (i = 0u; i < 2u; i++) {
		const struct nn_active_board *b = bs[i];
		const char *who = b->shapes_without_plugin ? "accepts" : "refuses";

		reset();
		snprintf(what, sizeof what, "%s: not a plugin (board that %s)",
		         state, who);
		ok(what, nn_active_core_is_plugin(b) == 0);

		snprintf(what, sizeof what, "[!] %s: shapes_ok is the board's own "
		         "answer (%s)", state, who);
		ok(what, nn_active_core_shapes_ok(b, d, 4u) ==
		         b->shapes_without_plugin);

		snprintf(what, sizeof what, "%s: decode says nothing is bound, not "
		         "'not a detector' (%s)", state, who);
		ok(what, nn_active_core_decode(b, d, 4u) == BF_ERR_UNINIT);

		nn_active_core_draw_on(b, &canvas);
		snprintf(what, sizeof what, "%s: nothing draws or reports, and says "
		         "so first (%s)", state, who);
		ok(what, nn_active_core_can_draw(b) == 0 &&
		         nn_active_core_can_report(b) == 0 &&
		         nn_active_core_report_to(b, NULL) == 0);

		snprintf(what, sizeof what, "%s: no threshold, and setting one is "
		         "'nothing holds it', not a refused value (%s)", state, who);
		ok(what, nn_active_core_get_thresh_milli(b) == NN_SVC_THRESH_NONE &&
		         nn_active_core_set_thresh_milli(b, 700u) ==
		                 NN_ACTIVE_THRESH_NO_DECODER);

		snprintf(what, sizeof what, "[!] %s: no slot was called and no "
		         "depth sampled (%s)", state, who);
		ok(what, total(calls) == 0u && total(notes) == 0u);
	}
}

/* ---- a plugin in force --------------------------------------------------- */

static void test_plugin(void)
{
	const struct nn_active_board *const bs[2] = { &refuses, &accepts };
	char what[160];
	unsigned i;

	printf("a plugin in force:\n");
	load_all();
	for (i = 0u; i < 2u; i++) {
		const struct nn_active_board *b = bs[i];
		const char *who = b->shapes_without_plugin ? "accepts" : "refuses";

		reset();
		ok("it is a plugin", nn_active_core_is_plugin(b) == 1);

		answer = !b->shapes_without_plugin;
		reset();
		snprintf(what, sizeof what, "[!] shapes_ok is the PLUGIN's answer, "
		         "the opposite of the board's (%s)", who);
		ok(what, nn_active_core_shapes_ok(b, d, 3u) == answer &&
		         seen_d == d && seen_n == 3u && only(PLUGIN_SLOT_SHAPES_OK));
	}

	answer = 7;
	reset();
	ok("decode hands the descriptors through and returns the plugin's count",
	   nn_active_core_decode(&refuses, d, 4u) == 7 && seen_d == d &&
	   seen_n == 4u && only(PLUGIN_SLOT_DECODE));
	answer = BF_ERR_MODEL;
	reset();
	ok("and its refusal, unchanged",
	   nn_active_core_decode(&accepts, d, 4u) == BF_ERR_MODEL);

	reset();
	nn_active_core_draw_on(&refuses, &canvas);
	ok("[!] draw builds the painter itself: versioned and sized, over the "
	   "caller's context and the board table's three (issue #165)",
	   seen_p != NULL && seen_paint.version == PLUGIN_ABI_VERSION &&
	   seen_paint.size == sizeof(struct plugin_painter) &&
	   seen_paint.ctx == &canvas && seen_paint.rect == t_rect &&
	   seen_paint.fill_rect == t_fill && seen_paint.blit == t_blit &&
	   only(PLUGIN_SLOT_DRAW));
	reset();
	ok("and asking whether it draws or reports calls nothing",
	   nn_active_core_can_draw(&refuses) == 1 &&
	   nn_active_core_can_report(&refuses) == 1 &&
	   total(calls) == 0u && total(notes) == 0u);

	answer = 12;
	reset();
	ok("report gets a versioned, sized printer over the caller's context and "
	   "the board table's writer (issue #165)",
	   nn_active_core_report_to(&accepts, (void *)&answer) == 12 &&
	   seen_out.version == PLUGIN_ABI_VERSION &&
	   seen_out.size == sizeof(struct plugin_printer) &&
	   seen_out.ctx == (void *)&answer && seen_out.write == w_write &&
	   only(PLUGIN_SLOT_REPORT));

	answer = 0;
	reset();
	ok("the threshold is the plugin's, as parameter 0",
	   nn_active_core_get_thresh_milli(&refuses) == 642u && seen_id == 0u &&
	   only(PLUGIN_SLOT_PARAM_GET));
	answer = -1;
	reset();
	ok("one it declines to give is none, not a number",
	   nn_active_core_get_thresh_milli(&refuses) == NN_SVC_THRESH_NONE);
	answer = 0;
	reset();
	ok("setting reaches the plugin as parameter 0",
	   nn_active_core_set_thresh_milli(&accepts, 800u) == NN_ACTIVE_THRESH_OK &&
	   seen_id == 0u && seen_value == 800u && only(PLUGIN_SLOT_PARAM_SET));
	answer = -1;
	reset();
	ok("[!] and a value it refuses is REFUSED, not 'nothing holds it'",
	   nn_active_core_set_thresh_milli(&accepts, 1000u) ==
	           NN_ACTIVE_THRESH_REFUSED);

	printf("a plugin with slots missing:\n");
	table[PLUGIN_SLOT_DRAW] = NULL;
	table[PLUGIN_SLOT_REPORT] = NULL;
	table[PLUGIN_SLOT_PARAM_GET] = NULL;
	table[PLUGIN_SLOT_PARAM_SET] = NULL;
	table[PLUGIN_SLOT_SHAPES_OK] = NULL;
	reset();
	nn_active_core_draw_on(&refuses, &canvas);
	ok("no draw slot: nothing drawn, and it says so",
	   nn_active_core_can_draw(&refuses) == 0 && total(calls) == 0u);
	ok("no report slot: nothing reported, and it says so",
	   nn_active_core_can_report(&refuses) == 0 &&
	   nn_active_core_report_to(&refuses, NULL) == 0);
	ok("[!] no parameters (a classifier): no threshold, and NO_DECODER",
	   nn_active_core_get_thresh_milli(&refuses) == NN_SVC_THRESH_NONE &&
	   nn_active_core_set_thresh_milli(&refuses, 500u) ==
	           NN_ACTIVE_THRESH_NO_DECODER);
	ok("no shapes_ok slot: the board's answer, both ways",
	   nn_active_core_shapes_ok(&refuses, d, 4u) == 0 &&
	   nn_active_core_shapes_ok(&accepts, d, 4u) == 1);
	ok("and none of it called or sampled anything",
	   total(calls) == 0u && total(notes) == 0u);

	printf("arguments:\n");
	load_all();
	reset();
	ok("[!] a NULL descriptor array is refused even by the board that accepts",
	   nn_active_core_shapes_ok(&accepts, NULL, 4u) == 0);
	ok("and is an argument error to decode",
	   nn_active_core_decode(&accepts, NULL, 4u) == BF_ERR_ARG);
	nn_active_core_draw_on(&accepts, NULL);
	/* No writer is no longer an argument: the write is the board table's,
	 * and the build refuses a table with a NULL word (issue #165). */
	ok("no context, no draw", total(calls) == 0u);
	ok("and none of them called or sampled anything",
	   total(calls) == 0u && total(notes) == 0u);
}

int main(void)
{
	printf("test_nn_active_core (svc/nn/nn_active_core.c):\n");
	memset(d, 0, sizeof d);

	printf("no plugin loaded:\n");
	live = 0;
	memset(table, 0, sizeof table);
	test_nobody("none loaded");

	/* A live image with slots but no decode is not a decoder -- and the
	 * slots it has must not be reached. */
	printf("a plugin with no decode slot:\n");
	load_all();
	table[PLUGIN_SLOT_DECODE] = NULL;
	test_nobody("no decode slot");

	/* Slots left in the table by an image no longer live. */
	printf("a table left behind by an unloaded plugin:\n");
	load_all();
	live = 0;
	test_nobody("unloaded");

	test_plugin();

	if (failures) {
		printf("test_nn_active_core: %d FAILED\n", failures);
		return 1;
	}
	printf("test_nn_active_core: all passed\n");
	return 0;
}
