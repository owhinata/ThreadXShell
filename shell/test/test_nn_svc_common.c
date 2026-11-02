/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Host test for svc/nn/nn_svc_common.c (issue #174): `nn thresh` on all three
 * boards and `nn info`'s model section on wio-lite-ai and f746g-disco.
 *
 * `nn thresh`: the ORDER on every exit -- count in -> lease -> the entry ->
 * lease back -> count out (grove-vision-ai-v2, whose console calls into the
 * plugin are counted from outside its gate), or lease -> entry -> lease back
 * (the other two) -- with a refused count entering nothing and a refused lease
 * counting back out; and every answer of the decoder's entries mapped to the
 * status the three boards gave it (an undocumented one to ARG on a set and BUSY
 * on a get, as before).  A board with no decoder answers NONE / STATE and calls
 * nothing.  None of this can be typed: a lease that times out, a count refused
 * by a load on another console, an entry reached unleased.
 *
 * `nn info`: the copy stands when the session was free, or when the counter
 * shows no load between its two reads; it is BUSY otherwise, and BUSY when the
 * backend's memory is held by another command (NN_ERR_SCRATCH_HELD).  The
 * inference API is stubbed here.
 */
#include <stdio.h>
#include <string.h>

#include "nn.h"
#include "nn_active_core.h"
#include "nn_svc.h"
#include "nn_svc_common.h"

static int fails, checks;

#define CHECK(cond, what)                                                   \
	do {                                                                \
		checks++;                                                   \
		if (!(cond)) {                                              \
			printf("  FAIL %s  (%s:%d)\n", what, __FILE__, __LINE__); \
			fails++;                                            \
		}                                                           \
	} while (0)

/* ---- nn thresh: a board that records the order --------------------------- */

#define NOT_HELD (-64)

static char trace[256];
static int  r_enter, r_lease, r_get, r_set;
static unsigned v_get;
static int  counted, leased;   /* what is held right now */

static void put(const char *s)
{
	if (trace[0] != '\0')
		strncat(trace, " ", sizeof trace - strlen(trace) - 1u);
	strncat(trace, s, sizeof trace - strlen(trace) - 1u);
}
static int  h_enter(void) { put("enter"); if (r_enter) counted = 1; return r_enter; }
static void h_leave(void) { put("leave"); counted = 0; }
static int  h_lease(void) { put("lease"); if (r_lease) leased = 1; return r_lease; }
static void h_give(void)  { put("give"); leased = 0; }
static int  h_get(unsigned *m)
{
	put(leased ? "get" : "get(UNLEASED)");
	if (r_get == NN_ACTIVE_THRESH_OK)
		*m = v_get;
	return r_get;
}
static int  h_set(unsigned m)
{
	(void)m;
	put(leased ? "set" : "set(UNLEASED)");
	return r_set;
}

static const struct nn_svc_thresh_ops counted_ops = {
	.enter = h_enter, .leave = h_leave, .lease_take = h_lease,
	.lease_give = h_give, .get = h_get, .set = h_set, .not_held = NOT_HELD,
};
static const struct nn_svc_thresh_ops lease_ops = {
	.lease_take = h_lease, .lease_give = h_give, .get = h_get, .set = h_set,
	.not_held = NOT_HELD,
};
static const struct nn_svc_thresh_ops no_decoder_ops = { .not_held = 0 };
static const struct nn_svc_thresh_ops no_lease_ops = {
	.get = h_get, .set = h_set, .not_held = NOT_HELD,
};
static const struct nn_svc_thresh_ops enter_only_ops = {
	.enter = h_enter, .lease_take = h_lease, .lease_give = h_give,
	.get = h_get, .set = h_set, .not_held = NOT_HELD,
};
static const struct nn_svc_thresh_ops leave_only_ops = {
	.leave = h_leave, .lease_take = h_lease, .lease_give = h_give,
	.get = h_get, .set = h_set, .not_held = NOT_HELD,
};

static void arm(int enter, int lease, int get, int set)
{
	trace[0] = '\0';
	r_enter = enter;
	r_lease = lease;
	r_get = get;
	r_set = set;
	v_get = 700u;
	counted = leased = 0;
}

static void get_case(const char *what, const struct nn_svc_thresh_ops *o,
                     int enter, int lease, int r, int want, unsigned want_v,
                     const char *want_trace)
{
	unsigned milli = 12345u;
	int got;
	char line[640];

	arm(enter, lease, r, 0);
	got = nn_svc_common_thresh_get(o, &milli);
	snprintf(line, sizeof line, "get, %s: status %d (want %d), milli %u "
	         "(want %u), \"%s\" (want \"%s\")", what, got, want, milli,
	         want_v, trace, want_trace);
	CHECK(got == want && milli == want_v &&
	      strcmp(trace, want_trace) == 0 && !counted && !leased, line);
}

static void set_case(const char *what, const struct nn_svc_thresh_ops *o,
                     int enter, int lease, int r, int want,
                     const char *want_trace)
{
	int got;
	char line[640];

	arm(enter, lease, 0, r);
	got = nn_svc_common_thresh_set(o, 700u);
	snprintf(line, sizeof line, "set, %s: status %d (want %d), \"%s\" "
	         "(want \"%s\")", what, got, want, trace, want_trace);
	CHECK(got == want && strcmp(trace, want_trace) == 0 && !counted &&
	      !leased, line);
}

static void test_thresh(void)
{
	static const struct { int r, want; } set_map[] = {
		{ NN_ACTIVE_THRESH_OK,          NN_SVC_OK },
		{ NN_ACTIVE_THRESH_NO_DECODER,  NN_SVC_ERR_STATE },
		{ NOT_HELD,                     NN_SVC_ERR_BUSY },
		{ NN_ACTIVE_THRESH_REFUSED,     NN_SVC_ERR_ARG },
		{ -99,                          NN_SVC_ERR_ARG },   /* undocumented */
		{ 7,                            NN_SVC_ERR_ARG },
	};
	size_t i;

	printf("nn_svc_common_thresh: order and answers\n");
	/* counted board (grove-vision-ai-v2) */
	get_case("counted, ok", &counted_ops, 1, 1, NN_ACTIVE_THRESH_OK,
	         NN_SVC_OK, 700u, "enter lease get give leave");
	get_case("counted, count refused", &counted_ops, 0, 1,
	         NN_ACTIVE_THRESH_OK, NN_SVC_ERR_BUSY, NN_SVC_THRESH_NONE,
	         "enter");
	get_case("counted, lease refused", &counted_ops, 1, 0,
	         NN_ACTIVE_THRESH_OK, NN_SVC_ERR_BUSY, NN_SVC_THRESH_NONE,
	         "enter lease leave");
	get_case("counted, not held", &counted_ops, 1, 1, NOT_HELD,
	         NN_SVC_ERR_BUSY, NN_SVC_THRESH_NONE,
	         "enter lease get give leave");
	set_case("counted, count refused", &counted_ops, 0, 1,
	         NN_ACTIVE_THRESH_OK, NN_SVC_ERR_BUSY, "enter");
	set_case("counted, lease refused", &counted_ops, 1, 0,
	         NN_ACTIVE_THRESH_OK, NN_SVC_ERR_BUSY, "enter lease leave");
	for (i = 0; i < sizeof set_map / sizeof set_map[0]; i++) {
		char what[64];

		snprintf(what, sizeof what, "counted, entry answers %d",
		         set_map[i].r);
		set_case(what, &counted_ops, 1, 1, set_map[i].r,
		         set_map[i].want, "enter lease set give leave");
		snprintf(what, sizeof what, "lease only, entry answers %d",
		         set_map[i].r);
		set_case(what, &lease_ops, 1, 1, set_map[i].r, set_map[i].want,
		         "lease set give");
	}
	/* lease-only boards (wio-lite-ai, f746g-disco) */
	get_case("lease only, ok", &lease_ops, 1, 1, NN_ACTIVE_THRESH_OK,
	         NN_SVC_OK, 700u, "lease get give");
	get_case("lease only, lease refused", &lease_ops, 1, 0,
	         NN_ACTIVE_THRESH_OK, NN_SVC_ERR_BUSY, NN_SVC_THRESH_NONE,
	         "lease");
	get_case("lease only, undocumented answer", &lease_ops, 1, 1, -99,
	         NN_SVC_ERR_BUSY, NN_SVC_THRESH_NONE, "lease get give");
	set_case("lease only, lease refused", &lease_ops, 1, 0,
	         NN_ACTIVE_THRESH_OK, NN_SVC_ERR_BUSY, "lease");
	/* no decoder (the null backends): the answers of "none loaded" */
	get_case("no decoder", &no_decoder_ops, 1, 1, NN_ACTIVE_THRESH_OK,
	         NN_SVC_OK, NN_SVC_THRESH_NONE, "");
	set_case("no decoder", &no_decoder_ops, 1, 1, NN_ACTIVE_THRESH_OK,
	         NN_SVC_ERR_STATE, "");
	get_case("NULL ops", NULL, 1, 1, NN_ACTIVE_THRESH_OK, NN_SVC_OK,
	         NN_SVC_THRESH_NONE, "");
	set_case("NULL ops", NULL, 1, 1, NN_ACTIVE_THRESH_OK,
	         NN_SVC_ERR_STATE, "");
	/* [!] an entry with no lease hook is never called: fail-closed */
	get_case("entries but no lease hook", &no_lease_ops, 1, 1,
	         NN_ACTIVE_THRESH_OK, NN_SVC_ERR_BUSY, NN_SVC_THRESH_NONE, "");
	set_case("entries but no lease hook", &no_lease_ops, 1, 1,
	         NN_ACTIVE_THRESH_OK, NN_SVC_ERR_BUSY, "");
	/* [!] a count with only one end is refused before anything is called */
	get_case("enter without leave", &enter_only_ops, 1, 1,
	         NN_ACTIVE_THRESH_OK, NN_SVC_ERR_BUSY, NN_SVC_THRESH_NONE, "");
	set_case("enter without leave", &enter_only_ops, 1, 1,
	         NN_ACTIVE_THRESH_OK, NN_SVC_ERR_BUSY, "");
	get_case("leave without enter", &leave_only_ops, 1, 1,
	         NN_ACTIVE_THRESH_OK, NN_SVC_ERR_BUSY, NN_SVC_THRESH_NONE, "");
	set_case("leave without enter", &leave_only_ops, 1, 1,
	         NN_ACTIVE_THRESH_OK, NN_SVC_ERR_BUSY, "");
	/* a NULL out-parameter is an argument error, before anything */
	arm(1, 1, NN_ACTIVE_THRESH_OK, 0);
	CHECK(nn_svc_common_thresh_get(&counted_ops, NULL) == NN_SVC_ERR_ARG &&
	      trace[0] == '\0', "get, NULL milli: ARG, nothing entered");
}

/* ---- nn info: the inference API, stubbed ---------------------------------- */

struct nn_model { int present; };

static struct nn_model the_model;
static struct nn_backend_info bi = { "tflm", "2.17" };
static int open_rc, session_rc, session_held;
static uint32_t seqs[2];
static int seq_reads;

const struct nn_backend_info *nn_backend(void) { return &bi; }
uint32_t nn_arena_reserved(void) { return 4096u; }
int nn_model_open(struct nn_model **out)
{
	if (open_rc != 0)
		return open_rc;
	*out = &the_model;
	return 0;
}
int nn_session_try_acquire(void)
{
	if (session_rc == 0)
		session_held = 1;
	return session_rc;
}
void nn_session_release(void) { session_held = 0; }
int nn_model_present(const struct nn_model *m) { return m->present; }
const char *nn_model_name(const struct nn_model *m)
{
	(void)m;
	return "det";
}
uint32_t nn_activations_bytes(const struct nn_model *m)
{
	(void)m;
	return 1234u;
}

static uint32_t life_seq(void)
{
	return seqs[seq_reads++ & 1];
}

static void info_case(const char *what, int orc, int src, int present,
                      uint32_t s0, uint32_t s1, int want_busy)
{
	struct nn_svc_info out;
	char line[160];

	open_rc = orc;
	session_rc = src;
	session_held = 0;
	the_model.present = present;
	seqs[0] = s0;
	seqs[1] = s1;
	seq_reads = 0;
	memset(&out, 0x5A, sizeof out);
	nn_svc_common_info(&out, life_seq);
	snprintf(line, sizeof line, "info, %s", what);
	CHECK(strcmp(out.backend, bi.name) == 0 && out.arena_bytes == 4096u &&
	      !session_held, line);
	if (want_busy) {
		CHECK(out.avail_identity == NN_AVAIL_BUSY &&
		      out.avail_runtime == NN_AVAIL_BUSY &&
		      out.avail_tensors == NN_AVAIL_BUSY &&
		      out.model[0] == '\0' && out.model_active == 0u &&
		      out.arena_used == 0u, line);
		return;
	}
	CHECK(out.avail_identity == NN_AVAIL_OK &&
	      out.avail_runtime == NN_AVAIL_OK &&
	      out.avail_tensors == (present ? NN_AVAIL_OK : NN_AVAIL_NA) &&
	      out.model_active == (present ? 1u : 0u) &&
	      out.arena_used == (present ? 1234u : 0u) &&
	      strcmp(out.model, "det") == 0, line);
}

static void test_info(void)
{
	struct nn_svc_info out;

	printf("nn_svc_common_info: when the copy stands\n");
	info_case("session free", 0, 0, 1, 4u, 4u, 0);
	info_case("session free, a load between (still stands)", 0, 0, 1, 4u,
	          6u, 0);
	info_case("session free, no model", 0, 0, 0, 4u, 4u, 0);
	info_case("session held (a stream), counter still", 0, -1, 1, 4u, 4u, 0);
	info_case("session held, a load in flight", 0, -1, 1, 5u, 5u, 1);
	info_case("session held, a load ran between", 0, -1, 1, 4u, 6u, 1);
	info_case("scratch held (f746g-disco's sdram test)",
	          NN_ERR_SCRATCH_HELD, 0, 1, 4u, 4u, 1);
	CHECK(seq_reads == 0, "scratch held: the counter is not read");

	/* the open failed for another reason: nothing but the backend and arena */
	open_rc = -1;
	memset(&out, 0x5A, sizeof out);
	nn_svc_common_info(&out, life_seq);
	CHECK(out.model[0] == '\0' && out.model_active == 0u &&
	      out.avail_identity == NN_AVAIL_OK && out.source[0] == '\0',
	      "info, open failed: zeroed, as before");

	/* the null backend says so */
	bi.name = "null";
	open_rc = 0;
	session_rc = 0;
	seqs[0] = seqs[1] = 2u;
	nn_svc_common_info(&out, life_seq);
	CHECK(strcmp(out.source, "synthetic workload, not inference") == 0,
	      "info, null backend: the source line says so");
	bi.name = "tflm";
}

int main(void)
{
	test_thresh();
	test_info();
	if (fails) {
		printf("test_nn_svc_common: %d of %d checks FAILED\n", fails, checks);
		return 1;
	}
	printf("test_nn_svc_common: all %d checks passed\n", checks);
	return 0;
}
