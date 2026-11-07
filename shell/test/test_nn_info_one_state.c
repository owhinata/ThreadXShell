/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Host test: one `nn info` never prints two models (issue #199), through the
 * REAL dispatcher, the REAL shell/cmds/cmd_nn.c and the REAL container lines
 * (svc/plugin/plugin_claims.c), over a stand-in adapter that holds two states.
 *
 * `nn info` copies the model section once and then reads the tensors, the
 * threshold and the container's claims one call at a time.  Another console's
 * `nn model load` can run whole between any two of those calls.  State A and
 * state B differ in every line -- the name, the shape of every tensor, the
 * number of outputs, the threshold, the plugin and where it came from -- so a
 * line of B beside the copy of A is visible.
 *
 * Pinned, for a whole load injected at EACH point a read is made -- after the
 * copy, after in[0], after the output count, after each output, after the
 * threshold, and before the claims are copied:
 *   - no line of B is printed at all (the copy is A's, so any B line would be
 *     the two models mixed);
 *   - every line read before the point is exactly what it is with no load, and
 *     everything after it is busy, in the model line's words: one `tensors :`
 *     line (never one per output, the count being A's), then `thresh  :`, then
 *     `plugin  :`.
 * With no load the output is the same bytes as before issue #199 (written out
 * below).  A read that fails with no load says `unavailable just now` as before;
 * one that fails across a load is busy.  A copy that carries no value (a load
 * held the claim when it was taken) prints no claims of the load that finished
 * after it.  None of this can be typed: two consoles would have to interleave
 * between single calls.
 */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cli_instance.h"
#include "cli_internal.h"
#include "cli_backend_dummy.h"
#include "host_glue.h"
#include "nn_svc.h"
#include "nn_svc_config.h"
#include "plugin_claims.h"

/* ---- the two states -------------------------------------------------------- */

#define N_OUT_A 2
#define N_OUT_B 3

struct state {
	const char         *name;
	struct tensor_desc  in;
	int                 n_out;
	struct tensor_desc  out[N_OUT_B];
	unsigned            thresh;
	struct plugin_claims claims;
};

static struct state st_a, st_b;
static const struct state *open_now;
static uint32_t ctr;                       /* the load order's counter */
static struct plugin_claims_store store;   /* the board's claims       */

static void tensor(struct tensor_desc *t, int d1, int d2, int d3, int dt,
                   size_t bytes)
{
	memset(t, 0, sizeof *t);
	t->rank    = d2 ? 4u : 2u;
	t->dims[0] = 1;
	t->dims[1] = d1;
	t->dims[2] = d2;
	t->dims[3] = d3;
	t->dtype   = (uint8_t)dt;
	t->bytes   = bytes;
	if (dt == TENSOR_DTYPE_INT8) {
		t->scale      = 0.5f;
		t->zero_point = -3;
	}
}

static void claims(struct plugin_claims *c, const char *plugin,
                   const char *from, uint32_t digest)
{
	memset(c, 0, sizeof *c);
	c->view.has_plugin = 1u;
	snprintf(c->view.name, sizeof c->view.name, "%s", plugin);
	snprintf(c->view.build_id, sizeof c->view.build_id, "abc1234");
	c->view.digest    = digest;
	c->view.file_size = 4100u;
	c->view.mem_size  = 8900u;
	c->view.link_addr = 0x20048000u;
	snprintf(c->from, sizeof c->from, "%s", from);
}

static void states(void)
{
	memset(&st_a, 0, sizeof st_a);
	st_a.name = "alpha";
	tensor(&st_a.in, 96, 96, 3, TENSOR_DTYPE_INT8, 27648u);
	st_a.n_out = N_OUT_A;
	tensor(&st_a.out[0], 10, 0, 0, TENSOR_DTYPE_INT8, 10u);
	tensor(&st_a.out[1], 4, 0, 0, TENSOR_DTYPE_INT8, 4u);
	st_a.thresh = 600u;
	claims(&st_a.claims, "alphadec", "slot 1, blob 'alpha'", 0xAAAA1111u);

	memset(&st_b, 0, sizeof st_b);
	st_b.name = "bravo";
	tensor(&st_b.in, 48, 48, 1, TENSOR_DTYPE_FLOAT32, 9216u);
	st_b.n_out = N_OUT_B;
	tensor(&st_b.out[0], 77, 0, 0, TENSOR_DTYPE_FLOAT32, 308u);
	tensor(&st_b.out[1], 55, 0, 0, TENSOR_DTYPE_FLOAT32, 220u);
	tensor(&st_b.out[2], 33, 0, 0, TENSOR_DTYPE_FLOAT32, 132u);
	st_b.thresh = 300u;
	claims(&st_b.claims, "bravodec", "slot 2, blob 'bravo'", 0xBBBB2222u);
}

/* Words only B's lines contain. */
static const char *const b_words[] = {
	"bravo", "48x48", "77", "55", "33", "300/1000", "bbbb2222", "slot 2",
};

/* ---- another console's load, at one chosen read ---------------------------- */

enum point {
	AT_NONE = 0,
	AT_INPUT,      /* after the copy: before in[0] is read     */
	AT_COUNT,      /* after in[0]                              */
	AT_OUT0,       /* after the count                          */
	AT_OUT1,       /* after out[0]                             */
	AT_THRESH,     /* after the last output                    */
	AT_CLAIMS,     /* after the threshold: before the claims   */
};

static enum point inject_at;
static int fail_input, fail_count, fail_out1;
static int b_count_fails;   /* B's output count cannot be read */

/* A whole load of B: odd, the model and the claims replaced, even again. */
static void load_b(void)
{
	ctr++;
	open_now = &st_b;
	plugin_claims_set(&store, &st_b.claims);
	ctr++;
}

static void at(enum point p)
{
	if (inject_at == p) {
		inject_at = AT_NONE;
		load_b();
	}
}

/* ---- the stand-in adapter ---------------------------------------------------- */

static int copy_busy;   /* the copy was taken while a load held the claim */

void nn_svc_info(struct nn_svc_info *out)
{
	memset(out, 0, sizeof *out);
	nn_svc_str(out->backend, sizeof out->backend, "tflm");
	nn_svc_str(out->version, sizeof out->version, "2.17");
	out->arena_bytes = 524288u;
	if (copy_busy) {
		out->avail_identity = (uint8_t)NN_AVAIL_BUSY;
		out->avail_runtime  = (uint8_t)NN_AVAIL_BUSY;
		out->avail_tensors  = (uint8_t)NN_AVAIL_BUSY;
		return;   /* no value: it belongs to no one state */
	}
	nn_svc_str(out->model, sizeof out->model, open_now->name);
	out->model_active = 1u;
	out->arena_used   = 123456u;
	out->seq_known    = 1u;
	out->seq          = ctr;
}
int nn_svc_info_stands(const struct nn_svc_info *info)
{
	return nn_svc_info_seq_stands(info, ctr);
}
int nn_svc_input(struct tensor_desc *o)
{
	at(AT_INPUT);
	if (fail_input)
		return NN_SVC_ERR_STATE;
	*o = open_now->in;
	return NN_SVC_OK;
}
int nn_svc_output_count(void)
{
	at(AT_COUNT);
	if (fail_count || (b_count_fails && open_now == &st_b))
		return NN_SVC_ERR_STATE;
	return open_now->n_out;
}
int nn_svc_output(unsigned i, struct tensor_desc *o)
{
	at(i == 0u ? AT_OUT0 : i == 1u ? AT_OUT1 : AT_NONE);
	if (i == 1u && fail_out1)
		return NN_SVC_ERR_STATE;
	if ((int)i >= open_now->n_out)
		return NN_SVC_ERR_ARG;
	*o = open_now->out[i];
	return NN_SVC_OK;
}
int nn_svc_thresh_get(unsigned *milli)
{
	at(AT_THRESH);
	*milli = open_now->thresh;
	return NN_SVC_OK;
}
void nn_svc_info_extra(const struct nn_svc_info *info, nn_svc_write_fn write,
                       void *ctx)
{
	struct plugin_claims c;
	enum plugin_claims_seen seen;

	at(AT_CLAIMS);
	/* the board's masked section: the counter and the store together */
	seen = plugin_claims_get(&store, ctr, info, &c);
	plugin_claims_info(write, ctx, seen, &c, 1, 0x20048000u, 32768u, 640u);
}

/* Not reached by `nn info`. */
void nn_svc_model_load(const struct nn_spec *spec, struct nn_op_result *res,
                       enum nn_model_state *state)
{
	(void)spec;
	res->status = NN_SVC_ERR_STATE;
	res->claim = (uint8_t)NN_CLAIM_NONE;
	*state = NN_MODEL_EMPTY;
}
void nn_svc_model_unload(struct nn_op_result *res) { res->status = NN_SVC_OK; }
int  nn_svc_tensors_pin(void) { return NN_SVC_ERR_STATE; }
void nn_svc_tensors_unpin(void) { }
void nn_svc_decode_current(struct nn_det_snapshot *snap, struct bf_det *d, int max,
                           struct nn_report_capture *rep,
                           struct nn_result_extra *ext, struct nn_op_result *res)
{
	(void)snap; (void)d; (void)max; (void)rep; (void)ext;
	res->status = NN_SVC_ERR_STATE;
}
int  nn_svc_box_to_frame(const struct bf_det *in, struct bf_det *out) { *out = *in; return NN_SVC_OK; }
int  nn_svc_thresh_set(unsigned milli) { (void)milli; return NN_SVC_ERR_STATE; }

/* ---- what `nn info` prints of A, before issue #199 --------------------------
 *
 * Written out, not rebuilt from the adapter, so a change of wording cannot
 * agree with itself.  Taken from the command as it was at 06a48b3 (before this
 * step) over this same adapter. */

#define HEAD_A                                                               \
	"backend : tflm (2.17)\r\n"                                          \
	"model   : alpha\r\n"                                                \
	"arena   : 524288 B reserved\r\n"                                    \
	"used    : 123456 B (activations)\r\n"
#define IN_A   "  in[0]  1x96x96x3 int8  q(s=0.500000 zp=-3)  27648B\r\n"
#define OUT0_A "  out[0]  1x10 int8  q(s=0.500000 zp=-3)  10B\r\n"
#define OUT1_A "  out[1]  1x4 int8  q(s=0.500000 zp=-3)  4B\r\n"
#define THR_A  "thresh  : 600/1000\r\n"
#define PLUG_A                                                               \
	"plugin  : alphadec (build abc1234, crc aaaa1111), running\r\n"      \
	"  from  : slot 1, blob 'alpha'\r\n"                                 \
	"  image : 4100 B file / 8900 B mem (code 0, data 0, bss 0), link "  \
	"0x20048000\r\n"                                                     \
	"  stack : entry 0  shapes 0  decode 0  draw 0  report 0  param 0/0 B " \
	"needed at c 640 B\r\n"                                              \
	"  decl  : own 0/0/0/0/0/0/0  at a crossing -/-/-/-/-/-/-  sink 0 B\r\n"

#define BUSY_WORDS "-- another nn command is using the model; ask again when " \
	"it returns\r\n"
#define T_BUSY "tensors : " BUSY_WORDS
#define H_BUSY "thresh  : " BUSY_WORDS
#define P_BUSY "plugin  : " BUSY_WORDS

/* ---- harness (the same shape as test_nn_cmd_usage.c) ----------------------- */

CLI_BACKEND_DUMMY_DEFINE(tr0);
static struct cli_instance sh0;

static void reset(void)
{
	memset(&sh0, 0, sizeof sh0);
	sh0.tr = &tr0;
	tr0.sh = &sh0;
	strcpy(sh0.prompt, "> ");
	cli_dummy_clear_output(&tr0);
	cli_dummy_clear_rx(&tr0);
	cli_dummy_reset_stats(&tr0);
	cli_dummy_set_tx_fail(&tr0, 0);
	cli_dummy_set_tx_cap(&tr0, 0);
	cli_test_set_tx_wait_hook(NULL, NULL);
	cli_test_set_sleep_hook(NULL, NULL);
	states();
	ctr = 6u;
	open_now = &st_a;
	plugin_claims_set(&store, &st_a.claims);
	inject_at = AT_NONE;
	fail_input = fail_count = fail_out1 = 0;
	b_count_fails = 0;
	copy_busy = 0;
}

/* `nn info`'s output, from its first line to the prompt that follows. */
static char got[4096];

static const char *nn_info(void)
{
	static const char line[] = "nn info\r";
	const char *o, *start, *end;

	cli_dummy_inject(&tr0, line, strlen(line));
	cli_test_pump(&sh0);
	o = cli_dummy_output_str(&tr0);
	start = strstr(o, "backend : ");
	end = start ? strstr(start, "> ") : NULL;
	got[0] = '\0';
	if (start != NULL) {
		size_t n = end ? (size_t)(end - start) : strlen(start);

		if (n >= sizeof got)
			n = sizeof got - 1u;
		memcpy(got, start, n);
		got[n] = '\0';
	}
	return got;
}

static int failures;

static void expect(const char *what, int ok)
{
	printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
	if (!ok) {
		printf("       output was: \"%s\"\n", got);
		failures++;
	}
}

static int no_b(void)
{
	size_t i;

	for (i = 0; i < sizeof b_words / sizeof b_words[0]; i++)
		if (strstr(got, b_words[i]) != NULL)
			return 0;
	return 1;
}

int main(void)
{
	static const struct {
		enum point  at;
		const char *what;
		const char *want;
	} inj[] = {
		{ AT_INPUT,  "a load after the copy",
		  HEAD_A T_BUSY H_BUSY P_BUSY },
		{ AT_COUNT,  "a load after in[0]",
		  HEAD_A IN_A T_BUSY H_BUSY P_BUSY },
		{ AT_OUT0,   "a load after the output count",
		  HEAD_A IN_A T_BUSY H_BUSY P_BUSY },
		{ AT_OUT1,   "a load after out[0]",
		  HEAD_A IN_A OUT0_A T_BUSY H_BUSY P_BUSY },
		{ AT_THRESH, "a load after out[1]",
		  HEAD_A IN_A OUT0_A OUT1_A H_BUSY P_BUSY },
	};
	char want[2048];
	unsigned i;

	printf("test_nn_info_one_state:\n");

	/* No load: A, every line, the bytes they were before issue #199. */
	reset();
	nn_info();
	expect("no load: A's lines, the bytes they were before",
	       strcmp(got, HEAD_A IN_A OUT0_A OUT1_A THR_A PLUG_A) == 0);
	expect("no load: no word of B", no_b());

	/* A whole load at each read: no line of B; before it A, after it busy. */
	for (i = 0; i < sizeof inj / sizeof inj[0]; i++) {
		reset();
		inject_at = inj[i].at;
		nn_info();
		expect(inj[i].what, strcmp(got, inj[i].want) == 0 && no_b() &&
		       inject_at == AT_NONE);
	}
	/* ...and before the claims are copied: the threshold is still A's, the
	 * container's line busy. */
	reset();
	inject_at = AT_CLAIMS;
	nn_info();
	expect("a load after the threshold: the claims are busy, not B's",
	       strcmp(got, HEAD_A IN_A OUT0_A OUT1_A THR_A P_BUSY) == 0 &&
	       no_b() && inject_at == AT_NONE);

	/* [!] ONE busy line for the tensors, never one per output: B has three
	 * outputs and A's count said two. */
	reset();
	inject_at = AT_OUT0;
	nn_info();
	{
		const char *p = got;
		int n = 0;

		while ((p = strstr(p, "tensors : ")) != NULL) {
			n++;
			p++;
		}
		expect("a load after the count: one tensors line", n == 1 &&
		       strstr(got, "out[") == NULL);
	}

	/* A read that fails with no load: as before, `unavailable just now`. */
	reset();
	fail_input = 1;
	nn_info();
	snprintf(want, sizeof want, "%s  in[0]   unavailable just now\r\n%s%s%s",
	         HEAD_A, OUT0_A, OUT1_A, THR_A);
	expect("in[0] unreadable, no load: unavailable just now",
	       strncmp(got, want, strlen(want)) == 0);
	reset();
	fail_count = 1;
	nn_info();
	snprintf(want, sizeof want, "%s%s  out[]   unavailable just now\r\n%s",
	         HEAD_A, IN_A, THR_A);
	expect("the count unreadable, no load: unavailable just now",
	       strncmp(got, want, strlen(want)) == 0);
	reset();
	fail_out1 = 1;
	nn_info();
	snprintf(want, sizeof want, "%s%s%s  out[1]  unavailable just now\r\n%s",
	         HEAD_A, IN_A, OUT0_A, THR_A);
	expect("out[1] unreadable, no load: unavailable just now",
	       strncmp(got, want, strlen(want)) == 0);
	/* ...and one that fails across a load: busy, not unavailable. */
	reset();
	fail_input = 1;
	inject_at = AT_INPUT;
	nn_info();
	expect("in[0] unreadable across a load: busy",
	       strcmp(got, HEAD_A T_BUSY H_BUSY P_BUSY) == 0 &&
	       strstr(got, "unavailable") == NULL);

	/* [!] THE COUNT IS CHECKED ITSELF, not only through the outputs read
	 * after it: B's count is unreadable, so no output is read to catch the
	 * load, and "out[] unavailable" would be a fact of B. */
	reset();
	b_count_fails = 1;
	inject_at = AT_COUNT;
	nn_info();
	expect("the count unreadable across a load: busy",
	       strcmp(got, HEAD_A IN_A T_BUSY H_BUSY P_BUSY) == 0);

	/* [!] A COPY WITH NO VALUE: the model line busy, and a load that finished
	 * after it does not put B's claims (or B's threshold) beside it. */
	reset();
	copy_busy = 1;
	load_b();
	nn_info();
	expect("a busy copy, a load finished since: nothing of B",
	       no_b() && strstr(got, "model   : " BUSY_WORDS) != NULL &&
	       strstr(got, "tensors : " BUSY_WORDS) != NULL &&
	       strstr(got, H_BUSY) != NULL && strstr(got, P_BUSY) != NULL);

	if (failures != 0) {
		printf("test_nn_info_one_state: %d failure(s)\n", failures);
		return 1;
	}
	printf("test_nn_info_one_state: all passed\n");
	return 0;
}
