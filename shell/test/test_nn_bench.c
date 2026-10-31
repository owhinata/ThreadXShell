/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Host test for the one `nn bench` procedure (issue #163, svc/nn/nn_bench.c).
 *
 * WHY THIS EXISTS.  The procedure's promise is an ORDER: the board's hold is
 * taken once, the inputs are filled and every run is timed under it, and it is
 * given back exactly once on every exit -- and never after an acquire that
 * failed.  An extra release on one exit gives back a hold somebody else now
 * owns; a missing one leaves the model locked until reset.  A console reaches
 * only the common exits, so a fake board records every hook and every model
 * call as one letter and each path is held to its sequence:
 *
 *   A acquire   S clock_start   C cancel asked   B run_begin   N nn_run
 *   E run_end_us   R release   M clock_mhz   I an input filled (under the hold)
 */
#include <stdio.h>
#include <string.h>

#include "nn_bench.h"

static int fails;

static void ok(const char *what, int cond)
{
	printf("  %s %s\n", cond ? "ok  " : "FAIL", what);
	if (!cond)
		fails++;
}

/* ---- the fake model (nn.h) ----------------------------------------------- */

struct nn_model { int unused; };

#define MAX_IN 4
#define BUF    8

static struct {
	struct nn_model model;
	int n_inputs;
	int tensor_null[MAX_IN];        /* nn_input() answers NULL           */
	int data_null[MAX_IN];          /* the tensor's data is NULL         */
	uint32_t bytes[MAX_IN];
	struct nn_tensor t[MAX_IN];
	uint8_t buf[MAX_IN][BUF + 1];   /* +1 guard byte, never filled       */
	int fail_at;                    /* nn_run() fails on this run (1-based) */
	int runs;
	uint32_t cycles[16];
	int cancel_at;                  /* cancel answers 1 on this ask (0-based) */
	int asks;
	/* the board */
	int held;                       /* the fake hold's depth              */
	int releases;
	int input_unheld;               /* an input was touched without the hold */
	int acquire_mode;
	char seq[256];
} f;

static void rec(char c)
{
	size_t n = strlen(f.seq);

	if (n + 1u < sizeof f.seq) {
		f.seq[n] = c;
		f.seq[n + 1u] = '\0';
	}
}

int nn_input_count(const struct nn_model *m)
{
	(void)m;
	if (!f.held)
		f.input_unheld = 1;
	return f.n_inputs;
}

struct nn_tensor *nn_input(struct nn_model *m, int idx)
{
	(void)m;
	if (idx < 0 || idx >= f.n_inputs || f.tensor_null[idx])
		return NULL;
	return &f.t[idx];
}

int nn_run(struct nn_model *m)
{
	(void)m;
	rec('N');
	f.runs++;
	return (f.runs == f.fail_at) ? -1 : 0;
}

uint32_t nn_last_cycles(const struct nn_model *m)
{
	(void)m;
	return f.cycles[(f.runs - 1) & 15];
}

/* ---- the fake board ------------------------------------------------------- */

enum { ACQ_OK, ACQ_BUSY, ACQ_STATE_UNDONE, ACQ_GROVE };

static struct nn_model *b_acquire(struct nn_op_result *res)
{
	rec('A');
	switch (f.acquire_mode) {
	case ACQ_BUSY:          /* the gate / session was taken: nothing held */
		res->status = NN_SVC_ERR_BUSY;
		res->claim = (uint8_t)NN_CLAIM_NONE;
		strcpy(res->detail, "busy words");
		return NULL;
	case ACQ_STATE_UNDONE:  /* took the hold, found no model, gave it back */
		f.held++;
		f.held--;
		res->status = NN_SVC_ERR_STATE;
		res->claim = (uint8_t)NN_CLAIM_NONE;
		strcpy(res->detail, "no model is loaded");
		return NULL;
	case ACQ_GROVE:         /* Grove's rule: no input 0 tensor is "no model" */
		f.held++;
		if (nn_input(&f.model, 0) == NULL) {
			f.held--;
			res->status = NN_SVC_ERR_STATE;
			res->claim = (uint8_t)NN_CLAIM_NONE;
			strcpy(res->detail, "no model is loaded");
			return NULL;
		}
		return &f.model;
	default:
		f.held++;
		return &f.model;
	}
}

static void b_release(void)
{
	rec('R');
	f.held--;
	f.releases++;
}

static void b_clock_start(struct nn_bench_clock *c)
{
	rec('S');
	ok("clock_start sees a zeroed clock, under the hold",
	   c->mhz == 0u && c->t0 == 0u && f.held == 1);
	c->mhz = 100u;
}

static void b_run_begin(struct nn_bench_clock *c)
{
	rec('B');
	c->t0 = 0xC0FFEEu;
}

static uint32_t b_run_end_us(struct nn_bench_clock *c, const struct nn_model *m)
{
	rec('E');
	if (c->t0 != 0xC0FFEEu)
		ok("run_end_us sees what run_begin wrote", 0);
	c->t0 = 0u;
	return nn_last_cycles(m) / c->mhz;
}

/* For the ops with no run_begin: nothing to pair with, a fixed figure. */
static uint32_t b_run_end_bare(struct nn_bench_clock *c,
                               const struct nn_model *m)
{
	(void)m;
	rec('E');
	ok("run_end_us (bare) sees the zeroed clock", c->mhz == 0u && c->t0 == 0u);
	return 7u;
}

static uint32_t b_clock_mhz(const struct nn_bench_clock *c)
{
	rec('M');
	return c->mhz;
}

static const struct nn_bench_ops full = {
	.acquire = b_acquire, .release = b_release, .clock_start = b_clock_start,
	.run_begin = b_run_begin, .run_end_us = b_run_end_us,
	.clock_mhz = b_clock_mhz,
};

/* wio's shape: neither a clock read once nor a mark before the run. */
static const struct nn_bench_ops bare = {
	.acquire = b_acquire, .release = b_release, .clock_start = NULL,
	.run_begin = NULL, .run_end_us = b_run_end_bare, .clock_mhz = NULL,
};

static int cancel_fn(void *ctx)
{
	int r;

	(void)ctx;
	rec('C');
	r = (f.asks == f.cancel_at);
	f.asks++;
	return r;
}

/* ---- the run --------------------------------------------------------------- */

static struct nn_op_result res;
static struct nn_bench_stats st;

static void reset(int n_inputs)
{
	int i;

	memset(&f, 0, sizeof f);
	f.fail_at = -1;
	f.cancel_at = -1;
	f.n_inputs = n_inputs;
	for (i = 0; i < MAX_IN; i++) {
		f.bytes[i] = BUF;
		memset(f.buf[i], 0x11, sizeof f.buf[i]);
	}
}

static void go(const struct nn_bench_ops *ops, uint32_t iters)
{
	int i;

	for (i = 0; i < f.n_inputs; i++) {
		f.t[i].data = f.data_null[i] ? NULL : f.buf[i];
		f.t[i].bytes = f.bytes[i];
	}
	memset(&res, 0, sizeof res);
	strcpy(res.detail, "stale words from before");
	res.claim = 0xEEu;
	memset(&st, 0xA5, sizeof st);
	nn_bench(ops, iters, &st, cancel_fn, NULL, &res);
}

static int filled(int i, uint32_t n)
{
	uint32_t k;

	for (k = 0u; k < n; k++)
		if (f.buf[i][k] != NN_BENCH_FILL)
			return 0;
	return f.buf[i][BUF] == 0x11u;   /* nothing past the tensor's bytes */
}

static int untouched(int i)
{
	uint32_t k;

	for (k = 0u; k <= BUF; k++)
		if (f.buf[i][k] != 0x11u)
			return 0;
	return 1;
}

static void seq_is(const char *what, const char *want)
{
	char line[640];

	snprintf(line, sizeof line, "%s: sequence %s (wanted %s)", what, f.seq,
	         want);
	ok(line, strcmp(f.seq, want) == 0);
}

static void ends_clean(const char *what, int status)
{
	char line[160];

	snprintf(line, sizeof line, "%s: status %d, claim NONE, hold given back",
	         what, status);
	ok(line, res.status == status && res.claim == (uint8_t)NN_CLAIM_NONE &&
	         f.held == 0);
}

int main(void)
{
	printf("nn_bench: an acquire that fails is never released\n");

	reset(1);
	f.acquire_mode = ACQ_BUSY;
	go(&full, 5u);
	seq_is("busy", "A");
	ends_clean("busy", NN_SVC_ERR_BUSY);
	ok("busy: 0 releases, its words kept", f.releases == 0 &&
	   strcmp(res.detail, "busy words") == 0);
	ok("busy: no input touched", untouched(0));

	reset(1);
	f.acquire_mode = ACQ_STATE_UNDONE;
	go(&full, 5u);
	seq_is("no model (partial hold undone by the board)", "A");
	ends_clean("no model", NN_SVC_ERR_STATE);
	ok("no model: 0 releases, no run", f.releases == 0 && f.runs == 0);

	/* Grove: no input 0 tensor is a refusal by the acquire ... */
	reset(1);
	f.acquire_mode = ACQ_GROVE;
	f.tensor_null[0] = 1;
	go(&full, 3u);
	seq_is("Grove, no input 0 tensor", "A");
	ends_clean("Grove, no input 0 tensor", NN_SVC_ERR_STATE);
	ok("Grove, no input 0 tensor: 0 releases", f.releases == 0);

	/* ... and a tensor whose data is NULL is not: the fill skips it. */
	reset(1);
	f.acquire_mode = ACQ_GROVE;
	f.data_null[0] = 1;
	f.cycles[0] = f.cycles[1] = 500u;
	go(&full, 2u);
	seq_is("Grove, input 0 with NULL data", "ASCBNECBNERM");
	ends_clean("Grove, input 0 with NULL data", NN_SVC_OK);
	ok("Grove, input 0 with NULL data: 1 release, 2 runs",
	   f.releases == 1 && st.runs == 2u);

	printf("nn_bench: every exit after a success releases exactly once\n");

	reset(1);
	f.cycles[0] = 300u; f.cycles[1] = 100u; f.cycles[2] = 200u;
	go(&full, 3u);
	seq_is("normal, 3 runs", "ASCBNECBNECBNERM");
	ends_clean("normal", NN_SVC_OK);
	ok("normal: 1 release", f.releases == 1);
	ok("normal: detail cleared", res.detail[0] == '\0');
	ok("normal: runs 3, min 1, avg 2, max 3, total 6 us, @100 MHz",
	   st.runs == 3u && st.min_us == 1u && st.avg_us == 2u &&
	   st.max_us == 3u && st.total_us == 6u && st.clock_mhz == 100u);

	reset(1);
	f.fail_at = 2;
	go(&full, 3u);
	seq_is("inference fails on run 2", "ASCBNECBNR");
	ends_clean("inference fails", NN_SVC_ERR_HW);
	ok("inference fails: 1 release, its words",
	   f.releases == 1 &&
	   strcmp(res.detail, "inference failed on run 2 of 3") == 0);

	reset(1);
	f.cancel_at = 2;
	go(&full, 5u);
	seq_is("cancel before run 3", "ASCBNECBNECR");
	ends_clean("cancel", NN_SVC_ERR_CANCEL);
	ok("cancel: 1 release, its words",
	   f.releases == 1 &&
	   strcmp(res.detail, "cancelled after 2 of 5 run(s)") == 0);

	reset(1);
	f.cancel_at = 0;
	go(&full, 5u);
	seq_is("cancel before the first run", "ASCR");
	ends_clean("cancel at once", NN_SVC_ERR_CANCEL);
	ok("cancel at once: 1 release, no run", f.releases == 1 && f.runs == 0);

	reset(1);
	go(&full, 0u);
	seq_is("zero iterations", "ASRM");
	ends_clean("zero iterations", NN_SVC_OK);
	ok("zero iterations: 1 release, runs 0, min 0, avg 0, total 0",
	   f.releases == 1 && st.runs == 0u && st.min_us == 0u &&
	   st.avg_us == 0u && st.max_us == 0u && st.total_us == 0u);

	printf("nn_bench: the optional hooks\n");

	reset(1);
	f.cycles[0] = 9u;
	go(&bare, 2u);
	seq_is("no clock_start / run_begin / clock_mhz", "ACNECNER");
	ends_clean("bare ops", NN_SVC_OK);
	ok("bare ops: clock reported 0, run_end_us's figure used",
	   st.clock_mhz == 0u && st.runs == 2u && st.min_us == 7u &&
	   st.max_us == 7u && st.total_us == 14u);

	printf("nn_bench: the fill\n");

	reset(4);
	f.bytes[1] = 3u;          /* a shorter tensor: only its bytes */
	f.data_null[2] = 1;       /* storage-less: skipped           */
	f.tensor_null[3] = 1;     /* no tensor at all: skipped       */
	go(&full, 1u);
	ends_clean("four inputs", NN_SVC_OK);
	ok("input 0 filled with 0x5A, nothing past it", filled(0, BUF));
	ok("input 1 filled for its 3 bytes only",
	   filled(1, 3u) && f.buf[1][3] == 0x11u);
	ok("input 2 (data NULL) skipped", untouched(2));
	ok("input 3 (tensor NULL) skipped", untouched(3));
	ok("every input touched under the hold", !f.input_unheld);
	ok("NN_BENCH_FILL is 0x5A on every board", NN_BENCH_FILL == 0x5Au);

	reset(0);
	go(&full, 1u);
	ends_clean("no inputs at all", NN_SVC_OK);
	ok("no inputs at all: still runs once", st.runs == 1u);

	printf("nn_bench: %s (%d failure(s))\n", fails ? "FAIL" : "PASS", fails);
	return fails ? 1 : 0;
}
