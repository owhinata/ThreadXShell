/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Host tests for svc/nn/nn_active_lease.c (issue #159): the lease-checked
 * entries into the plugin's decoder slots, moved out of wio-lite-ai's
 * port/nn/nn_active.c so that f746g-disco takes the same ones.
 *
 * The plugin is a set of counting stubs and the lease is a variable: what is
 * pinned is the GATE -- every entry refuses a caller without the lease with
 * NN_ACTIVE_NOT_HELD, enters nothing, counts the refusal exactly once, and with
 * the lease held goes in -- plus the descriptor array the entries build from the
 * board's model (count, cap, a hole read as a zeroed descriptor).  The routing
 * below the gate is svc/nn/nn_active_core.c's and test_nn_active_core.c's; the
 * board's own test (wio-lite-ai test_nn_active.c) drives the same entries with
 * its real facts and the real blazeface plugin, and its check_lease_entries.py
 * holds the entry list derived from this file's object.
 */
#include "nn_active_lease.h"
#include "plugin_lease_api.h"

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

/* ---- the lease ------------------------------------------------------------ */

static int      held = 1;
static unsigned unheld;

int plugin_lease_held(void)
{
	return held;
}

void plugin_lease_note_unheld(void)
{
	unheld++;
}

/* ---- the plugin: counting stubs ------------------------------------------ */

static unsigned calls[PLUGIN_SLOT_COUNT];
static unsigned notes;
static struct tensor_desc seen_d[NN_ACTIVE_MAX_OUTPUTS + 4u];
static unsigned seen_n;
static uint32_t thresh = 500u;

static int s_shapes(const struct tensor_desc *d, unsigned n)
{
	calls[PLUGIN_SLOT_SHAPES_OK]++;
	seen_n = n;
	memcpy(seen_d, d, n * sizeof d[0]);
	return 1;
}

static int s_decode(const struct tensor_desc *d, unsigned n)
{
	calls[PLUGIN_SLOT_DECODE]++;
	seen_n = n;
	memcpy(seen_d, d, n * sizeof d[0]);
	return 3;
}

static void s_draw(const struct plugin_painter *p)
{
	(void)p;
	calls[PLUGIN_SLOT_DRAW]++;
}

static int s_report(const struct plugin_printer *out)
{
	calls[PLUGIN_SLOT_REPORT]++;
	return out->write(out->ctx, "x", 1u);
}

static int s_param_set(uint32_t id, uint32_t v)
{
	(void)id;
	calls[PLUGIN_SLOT_PARAM_SET]++;
	thresh = v;
	return 0;
}

static int s_param_get(uint32_t id, uint32_t *v)
{
	(void)id;
	calls[PLUGIN_SLOT_PARAM_GET]++;
	*v = thresh;
	return 0;
}

static int loaded = 1;

static int c_active(void)
{
	return loaded;
}

static void *c_slot(unsigned slot)
{
	if (!loaded)
		return NULL;
	switch (slot) {
	case PLUGIN_SLOT_SHAPES_OK: return (void *)(uintptr_t)s_shapes;
	case PLUGIN_SLOT_DECODE:    return (void *)(uintptr_t)s_decode;
	case PLUGIN_SLOT_DRAW:      return (void *)(uintptr_t)s_draw;
	case PLUGIN_SLOT_REPORT:    return (void *)(uintptr_t)s_report;
	case PLUGIN_SLOT_PARAM_SET: return (void *)(uintptr_t)s_param_set;
	case PLUGIN_SLOT_PARAM_GET: return (void *)(uintptr_t)s_param_get;
	default:                    return NULL;
	}
}

static void c_note(unsigned slot, uintptr_t sp)
{
	(void)slot;
	(void)sp;
	notes++;
}

static const struct nn_active_board core = {
	.active = c_active,
	.slot   = c_slot,
	.note   = c_note,
	.shapes_without_plugin = 1,
};

/* ---- the board's model ----------------------------------------------------- */

struct nn_model {
	int      n;            /* what output_count() answers           */
	unsigned hole;         /* output index that is a hole, or ~0u   */
};

static struct nn_model model = { 4, ~0u };
static unsigned desc_calls;

static int f_count(struct nn_model *m)
{
	return m->n;
}

/* Output i: rank 1, dims[0] = 100 + i, so a descriptor says which it was. */
static int f_desc(struct nn_model *m, unsigned i, struct tensor_desc *d)
{
	desc_calls++;
	/* Garbage first: a hole must come back ZEROED, not as this residue. */
	memset(d, 0xA5, sizeof *d);
	if (i == m->hole)
		return -1;
	memset(d, 0, sizeof *d);
	d->rank    = 1u;
	d->dims[0] = (int32_t)(100u + i);
	d->dtype   = (uint8_t)TENSOR_DTYPE_FLOAT32;
	return 0;
}

const struct nn_active_lease_board nn_active_facts = {
	.core         = &core,
	.output_count = f_count,
	.output_desc  = f_desc,
};

/* ---- the entries ------------------------------------------------------------ */

static size_t wrote;

static int sink(void *ctx, const char *s, size_t len)
{
	(void)ctx;
	(void)s;
	wrote += len;
	return (int)len;
}

static const struct plugin_painter painter = {
	PLUGIN_ABI_VERSION, sizeof(struct plugin_painter), NULL, NULL, NULL, NULL
};

enum entry_id {
	E_SHAPES_OK, E_DECODE, E_DRAW, E_CAN_DRAW, E_CAN_REPORT, E_REPORT,
	E_THRESH_GET, E_THRESH_SET, E_COUNT
};

static const char *const entry_name[E_COUNT] = {
	"nn_active_shapes_ok", "nn_active_decode", "nn_active_draw",
	"nn_active_can_draw", "nn_active_can_report", "nn_active_report",
	"nn_active_get_thresh_milli", "nn_active_set_thresh_milli",
};

static int call_entry(enum entry_id e, unsigned *th)
{
	switch (e) {
	case E_SHAPES_OK:  return nn_active_shapes_ok(&model);
	case E_DECODE:     return nn_active_decode(&model);
	case E_DRAW:       return nn_active_draw(&painter);
	case E_CAN_DRAW:   return nn_active_can_draw();
	case E_CAN_REPORT: return nn_active_can_report();
	case E_REPORT:     return nn_active_report(sink, NULL);
	case E_THRESH_GET: *th = 12345u; return nn_active_get_thresh_milli(th);
	case E_THRESH_SET: return nn_active_set_thresh_milli(650u);
	default:           break;
	}
	return -9999;
}

static unsigned total_calls(void)
{
	unsigned i, t = 0u;

	for (i = 0u; i < (unsigned)PLUGIN_SLOT_COUNT; i++)
		t += calls[i];
	return t;
}

static void reset(void)
{
	memset(calls, 0, sizeof calls);
	notes = 0u;
	wrote = 0u;
	desc_calls = 0u;
	seen_n = 0u;
}

static void test_not_held(void)
{
	unsigned e, l;
	char what[160];

	printf("every entry refuses a caller without the lease:\n");
	for (e = 0u; e < (unsigned)E_COUNT; e++) {
		for (l = 0u; l < 2u; l++) {
			unsigned th = 0u, u0;
			uint32_t t0 = thresh;
			int ans;

			loaded = (int)l;
			reset();
			held = 0;
			u0 = unheld;
			ans = call_entry((enum entry_id)e, &th);
			held = 1;
			snprintf(what, sizeof what, "%s (%s): NOT_HELD, nothing "
			         "entered, nothing read, counted once", entry_name[e],
			         l ? "plugin" : "no plugin");
			ok(what, ans == NN_ACTIVE_NOT_HELD && total_calls() == 0u &&
			   notes == 0u && wrote == 0u && desc_calls == 0u &&
			   thresh == t0 && unheld == u0 + 1u);
			if (e == (unsigned)E_THRESH_GET)
				ok("  ...and reads back no threshold",
				   th == NN_SVC_THRESH_NONE);
		}
	}
	loaded = 1;
	{
		unsigned u0 = unheld;
		int ans;

		held = 0;
		ans = nn_active_is_plugin();
		held = 1;
		ok("is_plugin is the one question not checked (it decides whether "
		   "to take the lease)", ans == 1 && unheld == u0);
	}
}

static void test_held(void)
{
	unsigned th = 0u, u0 = unheld;

	printf("with the lease held, each goes in:\n");
	loaded = 1;
	reset();
	ok("shapes_ok asks the plugin", nn_active_shapes_ok(&model) == 1 &&
	   calls[PLUGIN_SLOT_SHAPES_OK] == 1u && notes == 1u);
	reset();
	ok("decode returns the plugin's count", nn_active_decode(&model) == 3 &&
	   calls[PLUGIN_SLOT_DECODE] == 1u);
	reset();
	ok("draw paints", nn_active_draw(&painter) == 0 &&
	   calls[PLUGIN_SLOT_DRAW] == 1u);
	ok("can_draw / can_report answer yes", nn_active_can_draw() == 1 &&
	   nn_active_can_report() == 1);
	reset();
	ok("report writes", nn_active_report(sink, NULL) == 1 && wrote == 1u);
	ok("set reaches the plugin's threshold",
	   nn_active_set_thresh_milli(650u) == NN_ACTIVE_THRESH_OK &&
	   thresh == 650u);
	ok("get reads it back", nn_active_get_thresh_milli(&th) ==
	   NN_ACTIVE_THRESH_OK && th == 650u);
	ok("nothing was counted as unheld", unheld == u0);

	printf("with no plugin, nobody:\n");
	loaded = 0;
	reset();
	ok("is_plugin says no", nn_active_is_plugin() == 0);
	ok("decode: BF_ERR_UNINIT, not BF_ERR_MODEL",
	   nn_active_decode(&model) == BF_ERR_UNINIT);
	ok("shapes: the board's answer (yes)", nn_active_shapes_ok(&model) == 1);
	ok("can_draw / can_report: no", nn_active_can_draw() == 0 &&
	   nn_active_can_report() == 0);
	ok("threshold: none, and setting it is a state",
	   nn_active_get_thresh_milli(&th) == NN_ACTIVE_THRESH_OK &&
	   th == NN_SVC_THRESH_NONE &&
	   nn_active_set_thresh_milli(700u) == NN_ACTIVE_THRESH_NO_DECODER);
	ok("and nothing was entered", total_calls() == 0u && notes == 0u);
	loaded = 1;
}

static void test_args(void)
{
	unsigned u0 = unheld;

	printf("arguments answered before the lease is asked:\n");
	held = 0;
	ok("a NULL model: shapes 0, decode BF_ERR_ARG",
	   nn_active_shapes_ok(NULL) == 0 && nn_active_decode(NULL) == BF_ERR_ARG);
	ok("a NULL threshold out-parameter: REFUSED",
	   nn_active_get_thresh_milli(NULL) == NN_ACTIVE_THRESH_REFUSED);
	ok("...none of them counted as unheld", unheld == u0);
	held = 1;
}

static void test_descriptors(void)
{
	unsigned i;
	int zero;

	printf("the descriptors handed to the plugin:\n");
	loaded = 1;

	model.n = 4; model.hole = ~0u;
	reset();
	(void)nn_active_shapes_ok(&model);
	ok("four outputs, in order", seen_n == 4u && seen_d[0].dims[0] == 100 &&
	   seen_d[3].dims[0] == 103);

	model.hole = 1u;
	reset();
	(void)nn_active_decode(&model);
	zero = 1;
	for (i = 0u; i < sizeof seen_d[1]; i++)
		if (((const unsigned char *)&seen_d[1])[i] != 0u)
			zero = 0;
	ok("[!] a hole is a ZEROED descriptor, not the board's residue",
	   seen_n == 4u && zero && seen_d[2].dims[0] == 102);
	ok("...and it is still counted, so the decoder decides",
	   seen_d[1].rank == 0u && seen_d[1].dtype == 0u);
	model.hole = ~0u;

	model.n = (int)NN_ACTIVE_MAX_OUTPUTS + 3;
	reset();
	(void)nn_active_shapes_ok(&model);
	ok("more outputs than the cap: the cap, and no more read",
	   seen_n == NN_ACTIVE_MAX_OUTPUTS &&
	   desc_calls == NN_ACTIVE_MAX_OUTPUTS);

	model.n = -2;
	reset();
	(void)nn_active_shapes_ok(&model);
	ok("a negative count is none", seen_n == 0u && desc_calls == 0u &&
	   calls[PLUGIN_SLOT_SHAPES_OK] == 1u);
	model.n = 4;
}

int main(void)
{
	printf("test_nn_active_lease\n");
	test_not_held();
	test_held();
	test_args();
	test_descriptors();
	if (failures != 0) {
		printf("test_nn_active_lease: %d FAILURE(S)\n", failures);
		return 1;
	}
	printf("test_nn_active_lease: all passed\n");
	return 0;
}
