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
 *
 * And which state the copy belongs to (issue #199): a copy taken under the
 * session carries the counter read BEFORE the session went back, so a load
 * that runs whole right after the release fails the check; one taken without
 * it carries the value only when both reads agree and it is even; a BUSY copy
 * carries none and never stands; and a build with no load (the counter at 0
 * for ever) always stands.
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
/* [!] A LIVE COUNTER (issue #199): when `live` is set the counter is
 * live_ctr, and the session's release first lets a whole load run when
 * `load_on_release` asks -- another console's `nn model load` landing in the
 * moment after `nn info` gave the session back. */
static int live, load_on_release;
static uint32_t live_ctr;
/* A failed open gives its initialisation back, and another console may open
 * again and run a whole load before `nn info` reads the counter: the open
 * moves the live counter by a whole load just before it fails. */
static int load_in_failed_open;

const struct nn_backend_info *nn_backend(void) { return &bi; }
uint32_t nn_arena_reserved(void) { return 4096u; }
int nn_model_open(struct nn_model **out)
{
	if (open_rc != 0) {
		if (load_in_failed_open) {
			load_in_failed_open = 0;
			live_ctr += 2u;
		}
		return open_rc;
	}
	*out = &the_model;
	return 0;
}
int nn_session_try_acquire(void)
{
	if (session_rc == 0)
		session_held = 1;
	return session_rc;
}
void nn_session_release(void)
{
	session_held = 0;
	if (load_on_release) {
		load_on_release = 0;
		live_ctr += 2u;   /* odd, then even again: one whole load */
	}
}
int nn_model_present(const struct nn_model *m) { return m->present; }
static const char *model_name = "det";
const char *nn_model_name(const struct nn_model *m)
{
	(void)m;
	return model_name;
}
uint32_t nn_activations_bytes(const struct nn_model *m)
{
	(void)m;
	return 1234u;
}

static uint32_t life_seq(void)
{
	if (live)
		return live_ctr;
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
		/* (c) a BUSY copy belongs to no one state (issue #199) */
		CHECK(out.seq_known == 0u, line);
		return;
	}
	CHECK(out.avail_identity == NN_AVAIL_OK &&
	      out.avail_runtime == NN_AVAIL_OK &&
	      out.avail_tensors == (present ? NN_AVAIL_OK : NN_AVAIL_NA) &&
	      out.model_active == (present ? 1u : 0u) &&
	      out.arena_used == (present ? 1234u : 0u) &&
	      strcmp(out.model, "det") == 0, line);
	/* The value is the second read: taken under the session when it was
	 * free, and equal to the first when it was not (issue #199). */
	CHECK(out.seq_known == 1u && out.seq == s1, line);
}

/*
 * ---- nn info: which state the copy belongs to (issue #199) ----------------
 *
 * One copy against a live counter, then the check after the counter has (or
 * has not) moved.
 */
static void copy_at(struct nn_svc_info *out, int src, uint32_t ctr,
                    int load_after_release)
{
	open_rc = 0;
	session_rc = src;
	session_held = 0;
	the_model.present = 1;
	live = 1;
	live_ctr = ctr;
	load_on_release = load_after_release;
	memset(out, 0x5A, sizeof *out);
	nn_svc_common_info(out, life_seq);
}

static int stands_at(const struct nn_svc_info *in, uint32_t now)
{
	live = 1;
	live_ctr = now;
	return nn_svc_common_info_stands(in, life_seq);
}

static void test_info_stands(void)
{
	struct nn_svc_info out;

	printf("nn_svc_common_info: which state the copy belongs to\n");

	/* (a) [!] The session taken: the value is read before it goes back, so a
	 * whole load right after the release fails the check.  A value read
	 * after the release would be the load's, and pass. */
	copy_at(&out, 0, 4u, 1);
	CHECK(out.avail_identity == NN_AVAIL_OK && out.seq_known == 1u &&
	      out.seq == 4u, "(a) session free: the copy is of state 4");
	CHECK(live_ctr == 6u, "(a) the load ran after the release");
	CHECK(!nn_svc_common_info_stands(&out, life_seq),
	      "(a) a load between the release and the next read: does not stand");
	copy_at(&out, 0, 4u, 0);
	CHECK(nn_svc_common_info_stands(&out, life_seq),
	      "(a) no load after the release: stands");
	CHECK(!stands_at(&out, 5u), "(a) a load in flight now: does not stand");
	CHECK(!stands_at(&out, 6u), "(a) a load ran whole since: does not stand");
	/* An odd counter under the session cannot happen (a load holds it); if
	 * it ever did, the copy is shown as before but carries no value. */
	copy_at(&out, 0, 5u, 0);
	CHECK(out.avail_identity == NN_AVAIL_OK && out.seq_known == 0u &&
	      !stands_at(&out, 5u), "(a) session free, odd counter: no value");

	/* (b) The session held (a stream): the value only when both reads agree
	 * and are even. */
	live = 0;
	info_case("(b) held, both reads 4", 0, -1, 1, 4u, 4u, 0);
	copy_at(&out, -1, 4u, 0);
	CHECK(out.seq_known == 1u && out.seq == 4u && stands_at(&out, 4u),
	      "(b) held, counter still: the copy is of state 4 and stands");
	CHECK(!stands_at(&out, 6u), "(b) held, then a load ran: does not stand");
	live = 0;
	info_case("(b) held, 4 then 6: busy, no value", 0, -1, 1, 4u, 6u, 1);
	info_case("(b) held, 5 then 5: busy, no value", 0, -1, 1, 5u, 5u, 1);
	info_case("(b) held, 5 then 7: busy, no value", 0, -1, 1, 5u, 7u, 1);

	/* (c) A BUSY copy never stands -- not even at the counter value its
	 * cleared field happens to read. */
	live = 0;
	open_rc = NN_ERR_SCRATCH_HELD;
	memset(&out, 0x5A, sizeof out);
	nn_svc_common_info(&out, life_seq);
	CHECK(out.seq_known == 0u && !stands_at(&out, out.seq) &&
	      !stands_at(&out, 0u), "(c) scratch held: no value, never stands");
	seqs[0] = 4u;
	seqs[1] = 6u;
	seq_reads = 0;
	live = 0;
	open_rc = 0;
	session_rc = -1;
	memset(&out, 0x5A, sizeof out);
	nn_svc_common_info(&out, life_seq);
	CHECK(out.avail_identity == NN_AVAIL_BUSY && out.seq_known == 0u &&
	      !stands_at(&out, 4u) && !stands_at(&out, 6u),
	      "(c) the counter moved: no value, never stands");
	/* The open failed (the backend never came up): "nothing loaded", not
	 * BUSY, so the copy carries the counter read right after -- when even --
	 * and a load that lands later fails the check. */
	open_rc = -1;
	live = 1;
	live_ctr = 4u;
	memset(&out, 0x5A, sizeof out);
	nn_svc_common_info(&out, life_seq);
	CHECK(out.avail_identity == NN_AVAIL_OK && out.seq_known == 1u &&
	      out.seq == 4u && stands_at(&out, 4u) && !stands_at(&out, 6u),
	      "the open failed, counter even: the value, stands until a load");
	live_ctr = 5u;
	memset(&out, 0x5A, sizeof out);
	nn_svc_common_info(&out, life_seq);
	CHECK(out.avail_identity == NN_AVAIL_OK && out.seq_known == 0u &&
	      !stands_at(&out, 5u), "the open failed, counter odd: no value");
	/* [!] A whole load between the failed open and the read after it: the
	 * "nothing loaded" copy must not take the load's value (the threshold
	 * and the container lines would then pass as the new model's). */
	live_ctr = 4u;
	load_in_failed_open = 1;
	memset(&out, 0x5A, sizeof out);
	nn_svc_common_info(&out, life_seq);
	CHECK(live_ctr == 6u && out.model_active == 0u && out.seq_known == 0u &&
	      !stands_at(&out, 6u),
	      "the open failed, a load ran before the second read: no value");
	live = 0;

	/* (d) A build with no load: the counter is 0 for ever, and every copy --
	 * the session free or held -- stands. */
	copy_at(&out, 0, 0u, 0);
	CHECK(out.seq_known == 1u && out.seq == 0u && stands_at(&out, 0u),
	      "(d) no load in this build, session free: stands");
	copy_at(&out, -1, 0u, 0);
	CHECK(out.seq_known == 1u && out.seq == 0u && stands_at(&out, 0u),
	      "(d) no load in this build, session held: stands");

	/* The shared comparison itself: no value, an odd value, a different
	 * value. */
	memset(&out, 0, sizeof out);
	out.seq_known = 1u;
	out.seq = 7u;
	CHECK(!nn_svc_info_seq_stands(&out, 7u), "an odd value never stands");
	out.seq = 8u;
	CHECK(nn_svc_info_seq_stands(&out, 8u) &&
	      !nn_svc_info_seq_stands(&out, 10u), "an even value: equal only");
	out.seq_known = 0u;
	CHECK(!nn_svc_info_seq_stands(&out, 8u), "no value never stands");
	CHECK(!nn_svc_info_seq_stands(NULL, 0u), "NULL never stands");

	live = 0;
	open_rc = 0;
	session_rc = 0;
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
	/* (the first read is before the open since issue #199; the copy carries
	 * no value -- checked in test_info_stands()) */

	/* the open failed for another reason: nothing but the backend and arena */
	open_rc = -1;
	memset(&out, 0x5A, sizeof out);
	nn_svc_common_info(&out, life_seq);
	CHECK(out.model[0] == '\0' && out.model_active == 0u &&
	      out.avail_identity == NN_AVAIL_OK && out.source[0] == '\0',
	      "info, open failed: zeroed, as before");

	/* The longest name a backend can hand out -- the longest blob name -- is
	 * copied whole (issue #201). */
	{
		char longest[BLOB_NAME_MAX + 1u];

		memset(longest, 'a', BLOB_NAME_MAX);
		longest[BLOB_NAME_MAX - 1u] = 'z';
		longest[BLOB_NAME_MAX] = '\0';
		model_name = longest;
		open_rc = 0;
		session_rc = 0;
		seqs[0] = seqs[1] = 2u;
		seq_reads = 0;
		the_model.present = 1;
		memset(&out, 0x5A, sizeof out);
		nn_svc_common_info(&out, life_seq);
		CHECK(out.model_active == 1u && strcmp(out.model, longest) == 0,
		      "info, the longest name: copied whole");
		model_name = "det";
	}

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
	test_info_stands();
	if (fails) {
		printf("test_nn_svc_common: %d of %d checks FAILED\n", fails, checks);
		return 1;
	}
	printf("test_nn_svc_common: all %d checks passed\n", checks);
	return 0;
}
