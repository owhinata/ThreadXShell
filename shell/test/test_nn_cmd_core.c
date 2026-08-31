/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Host unit test for shell/cmds/nn_cmd_core.c -- the parts of the one shared `nn`
 * command that are pure logic (issue #50).
 *
 * WHY THESE PARTS AND NOT OTHERS.  Everything else in cmd_nn.c either prints or
 * calls a board, and a console can be read to see whether a line looks right.
 * These cannot be checked that way:
 *
 *   A. THE NUMBER FORMATTING.  This firmware has no %f, so fractional values are
 *      printed as scaled integers.  wio carried a form that printed 1.5 as
 *      "0.1500000" -- silently wrong for every scale >= 1, which output tensors
 *      routinely have -- and it survived because a quantisation scale looks
 *      plausible either way.  Case A pins the values that distinguish them.
 *
 *   B. THE ARGUMENT GRAMMAR.  A parser that accepts a bare string would have to
 *      guess which namespace it meant, differently per board.  Case C asserts
 *      the bare string is REFUSED, which is the property that keeps board
 *      knowledge out of shell/ -- and it is the assertion someone deletes when
 *      they "helpfully" make the command friendlier.
 *
 *   C. THE TWO REFUSALS ARE DIFFERENT.  A tag this grammar does not have at all
 *      and a tag with a malformed operand send a reader to different places, so
 *      they must not collapse into one code.
 *
 *   D. WHAT `nn stream stats` SAYS ON ITS `last` LINE (issue #105).  Nothing in
 *      this project gates what a command PRINTS -- the board README says so in
 *      as many words -- and this line has four cases that send an operator to
 *      four different places: wait, look at the picture, load a different model,
 *      or read the count.  Two of them were introduced because folding them
 *      together had already misled someone (issues #97, #99), and the noun in
 *      the fourth stopped being "face(s)" when a classifier plugin could hold
 *      the panel.  A build that compiles proves none of that, so the choice was
 *      made a pure function and the sentences are pinned here.
 */
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "nn_cmd_core.h"

static int last_failures;

/* ---- A: scaled-integer float decomposition ------------------------------- */

static void expect_parts(float v, const char *sign, uint32_t ip, uint32_t frac)
{
	const char *got_sign = NULL;
	uint32_t got_ip = 0u, got_frac = 0u;
	int rc = nn_f32_parts(v, &got_sign, &got_ip, &got_frac);

	assert(rc == 0);
	assert(strcmp(got_sign, sign) == 0);
	assert(got_ip == ip);
	assert(got_frac == frac);
}

static void test_f32_parts(void)
{
	expect_parts(0.0f,      "",  0u, 0u);
	expect_parts(0.5f,      "",  0u, 500000u);

	/* [!] THE REGRESSION.  A scale of 1.5 must be 1.500000, not 0.1500000.
	 * This is the exact value that made the old parts-per-million form wrong,
	 * and >= 1 scales are ordinary on output tensors. */
	expect_parts(1.5f,      "",  1u, 500000u);
	expect_parts(2.0f,      "",  2u, 0u);
	expect_parts(1.2247f,   "",  1u, 224700u);

	/* A dequantised output can be negative, and for a logit the sign is the
	 * whole answer -- a scale never can be, which is why one function has to
	 * carry the sign that the scale-only copy did not. */
	expect_parts(-0.25f,    "-", 0u, 250000u);
	expect_parts(-3.5f,     "-", 3u, 500000u);

	/* Saturation, not undefined behaviour: the clamp is before the cast. */
	expect_parts(2000000.0f, "", 999999u, 999999u);
	expect_parts(1e30f,      "", 999999u, 999999u);
	expect_parts(-1e30f,   "-",  999999u, 999999u);

	/* NaN is a diagnosis, reported as such rather than as a number. */
	{
		const char *sign = NULL;
		uint32_t ip = 0u, frac = 0u;
		float nan = 0.0f / 0.0f;

		assert(nn_f32_parts(nan, &sign, &ip, &frac) == -1);
	}

	/* The rounding carry must move the integer part, not print ".1000000". */
	expect_parts(0.9999999f, "", 1u, 0u);

	printf("  A. float decomposition (incl. the >= 1 scale regression)  ok\n");
}

/* ---- B: shape strings ---------------------------------------------------- */

static void test_shape(void)
{
	struct tensor_desc t;
	char buf[32];

	memset(&t, 0, sizeof t);
	t.rank = 4u;
	t.dims[0] = 1; t.dims[1] = 128; t.dims[2] = 128; t.dims[3] = 3;
	assert(strcmp(nn_shape_str(&t, buf, sizeof buf), "1x128x128x3") == 0);

	memset(&t, 0, sizeof t);
	t.rank = 2u;
	t.dims[0] = 1; t.dims[1] = 896;
	assert(strcmp(nn_shape_str(&t, buf, sizeof buf), "1x896") == 0);

	/* [!] Rank 0 means "not representable", because the boards refuse to
	 * truncate a higher-rank tensor into four dimensions.  It must not print
	 * as a plausible empty shape that a reader would take for a measurement. */
	memset(&t, 0, sizeof t);
	assert(strcmp(nn_shape_str(&t, buf, sizeof buf), "?") == 0);
	assert(strcmp(nn_shape_str(NULL, buf, sizeof buf), "?") == 0);

	/* A buffer too small truncates at a dimension boundary and stays
	   NUL-terminated -- it never runs off the end. */
	memset(&t, 0, sizeof t);
	t.rank = 4u;
	t.dims[0] = 1000; t.dims[1] = 1000; t.dims[2] = 1000; t.dims[3] = 1000;
	assert(nn_shape_str(&t, buf, 6u)[5] == '\0' || strlen(buf) < 6u);
	assert(strlen(nn_shape_str(&t, buf, 6u)) < 6u);

	printf("  B. shape strings                                          ok\n");
}

/* ---- C: the model spec grammar ------------------------------------------- */

static int parse(struct nn_spec *out, int argc, const char *a0, const char *a1,
                 const char *a2)
{
	char *argv[3];

	argv[0] = (char *)a0;
	argv[1] = (char *)a1;
	argv[2] = (char *)a2;
	return nn_spec_parse(argc, argv, out);
}

static void test_spec_accepts(void)
{
	struct nn_spec s;

	assert(parse(&s, 2, "--name", "cls", NULL) == NN_SVC_OK);
	assert(s.tag == NN_SPEC_NAME && strcmp(s.name, "cls") == 0);

	assert(parse(&s, 2, "--slot", "9", NULL) == NN_SVC_OK);
	assert(s.tag == NN_SPEC_SLOT && s.slot == 9u);

	assert(parse(&s, 2, "--path", "/model.tflite", NULL) == NN_SVC_OK);
	assert(s.tag == NN_SPEC_PATH && strcmp(s.path, "/model.tflite") == 0);

	assert(parse(&s, 1, "builtin", NULL, NULL) == NN_SVC_OK);
	assert(s.tag == NN_SPEC_BUILTIN);

	assert(parse(&s, 3, "--addr", "0x3AE81000", "164512") == NN_SVC_OK);
	assert(s.tag == NN_SPEC_ADDR && s.addr == 0x3AE81000u && s.len == 164512u);

	printf("  C. every source form parses                               ok\n");
}

static void test_spec_refuses(void)
{
	struct nn_spec s;

	/* [!] THE ONE THAT MATTERS.  A bare word is not a spec: it would mean a
	 * blob name on Grove, a path on f746 and nothing on wio.  Accepting it
	 * here is how board knowledge gets back into shell/. */
	assert(parse(&s, 1, "cls", NULL, NULL) == NN_SVC_ERR_SPEC);
	assert(parse(&s, 1, "/model.tflite", NULL, NULL) == NN_SVC_ERR_SPEC);
	assert(parse(&s, 1, "9", NULL, NULL) == NN_SVC_ERR_SPEC);

	/* Nothing at all is a malformed invocation, not an unknown namespace. */
	assert(parse(&s, 0, NULL, NULL, NULL) == NN_SVC_ERR_ARG);

	/* A known tag with a missing or unusable operand is ERR_ARG -- a
	   different answer from ERR_SPEC, because it points somewhere else. */
	assert(parse(&s, 1, "--name", NULL, NULL) == NN_SVC_ERR_ARG);
	assert(parse(&s, 2, "--name", "", NULL) == NN_SVC_ERR_ARG);
	assert(parse(&s, 1, "--slot", NULL, NULL) == NN_SVC_ERR_ARG);
	assert(parse(&s, 2, "--slot", "nine", NULL) == NN_SVC_ERR_ARG);
	assert(parse(&s, 2, "--path", "", NULL) == NN_SVC_ERR_ARG);
	assert(parse(&s, 2, "builtin", "extra", NULL) == NN_SVC_ERR_ARG);

	/* [!] THE RAW FORM WITHOUT ITS LENGTH IS REFUSED.  The length is the
	 * bound the FlatBuffer verifier is given; "the rest of the window" is not
	 * a bound and this repo forbids it.  Refusing here rather than leaving a
	 * board to discover it keeps every board's answer the same. */
	assert(parse(&s, 2, "--addr", "0x3AE81000", NULL) == NN_SVC_ERR_ARG);
	assert(parse(&s, 3, "--addr", "0x3AE81000", "0") == NN_SVC_ERR_ARG);
	assert(parse(&s, 3, "--addr", "0x3AE81000", "no") == NN_SVC_ERR_ARG);

	/* A name too long to hold is refused, never truncated: a truncated name
	   can still match a DIFFERENT entry, which is worse than not matching. */
	{
		char longname[NN_SPEC_NAME_MAX + 8];

		memset(longname, 'a', sizeof longname - 1u);
		longname[sizeof longname - 1u] = '\0';
		assert(parse(&s, 2, "--name", longname, NULL) == NN_SVC_ERR_ARG);
	}

	printf("  D. refusals stay distinct (bare word, operand, length)    ok\n");
}

/* ---- E: the names a command prints --------------------------------------- */

static void test_names(void)
{
	/* Every enumerator has a name, and an out-of-range value does not read as
	   one of the real ones. */
	assert(strcmp(nn_claim_name(NN_CLAIM_NONE), "none") == 0);
	assert(strcmp(nn_claim_name(NN_CLAIM_CALLER), "caller") == 0);
	assert(strcmp(nn_claim_name(NN_CLAIM_RETRYABLE), "retryable") == 0);
	assert(strcmp(nn_claim_name(NN_CLAIM_TERMINAL), "terminal") == 0);
	assert(strcmp(nn_claim_name(99u), "?") == 0);

	assert(strcmp(nn_model_state_name(NN_MODEL_EMPTY), "empty") == 0);
	assert(strcmp(nn_model_state_name(NN_MODEL_NEW), "new") == 0);
	assert(strcmp(nn_model_state_name(NN_MODEL_PREVIOUS), "previous") == 0);
	assert(strcmp(nn_model_state_name(99u), "?") == 0);

	assert(nn_status_name(NN_SVC_OK) != NULL);
	assert(nn_status_name(-12345) != NULL);

	/* Types: a tag this build does not know must read as unknown, not as
	   int8 -- a consumer that believed it would read the buffer wrongly. */
	assert(strcmp(nn_dtype_name(TENSOR_DTYPE_INT8), "int8") == 0);
	assert(strcmp(nn_dtype_name(TENSOR_DTYPE_FLOAT32), "f32") == 0);
	assert(strcmp(nn_dtype_name(TENSOR_DTYPE_UNSUPPORTED), "?") == 0);
	assert(nn_dtype_size(TENSOR_DTYPE_UNSUPPORTED) == 0u);
	assert(nn_dtype_size(TENSOR_DTYPE_INT8) == 1u);
	assert(nn_dtype_size(TENSOR_DTYPE_FLOAT32) == 4u);

	printf("  E. names and element sizes                                ok\n");
}

/* ---- D: the `last` line of `nn stream stats` ----------------------------- */

static void expect_last(const char *what, uint8_t valid, uint32_t infers,
                        int32_t ndet, enum nn_last_kind kind, const char *text)
{
	enum nn_last_kind got = nn_last_kind_of(valid, infers, ndet);
	const char *got_text = nn_last_text(got);

	if (got != kind) {
		printf("  FAIL %s: kind %d, wanted %d\n", what, (int)got, (int)kind);
		last_failures++;
		return;
	}
	if (text == NULL) {
		if (got_text != NULL) {
			printf("  FAIL %s: expected no sentence, got \"%s\"\n",
			       what, got_text);
			last_failures++;
			return;
		}
	} else if (got_text == NULL || strcmp(got_text, text) != 0) {
		printf("  FAIL %s: \"%s\"\n", what,
		       got_text ? got_text : "(none)");
		last_failures++;
		return;
	}
	printf("  ok   %s\n", what);
}

static void test_last_line(void)
{
	printf(" case: the `last` line of `nn stream stats`\n");

	/* Nothing has decoded yet.  "0 result(s)" here would read as a working
	 * decoder that found nothing, which is the confusion issue #97 named. */
	expect_last("a stream that has not finished its first frame",
	            0u, 0u, 0, NN_LAST_NEVER, "nothing decoded yet");

	/*
	 * [!] AND THE SAME FIELDS AFTER A RUN MEAN SOMETHING ELSE.  One board
	 * retires its result when a stream stops, deliberately, so that a stopped
	 * stream does not leave a stale annotation on view.  Saying "nothing
	 * decoded yet" after hundreds of inferences reads as a broken decoder.
	 */
	expect_last("a stopped stream that did infer",
	            0u, 412u, 0, NN_LAST_RETIRED,
	            "the result is dropped when a stream stops");

	/*
	 * [!] A NEGATIVE COUNT IS NOT A COUNT.  It is the decoder saying the open
	 * model is not one it recognises -- load a different model, rather than
	 * look at the picture -- and printing "-1 result(s)" hands the reader
	 * arithmetic to do on a sentinel.
	 */
	expect_last("a model the decoder does not recognise",
	            1u, 40u, -1, NN_LAST_UNRECOGNISED,
	            "this model is not one the decoder recognises");
	expect_last("and any other negative sentinel, not just -1",
	            1u, 40u, -3, NN_LAST_UNRECOGNISED,
	            "this model is not one the decoder recognises");

	/* A real count, including zero -- which is a measurement, not an absence. */
	expect_last("a decode that produced nothing is still a decode",
	            1u, 40u, 0, NN_LAST_COUNT, NULL);
	expect_last("and one that produced items", 1u, 40u, 3, NN_LAST_COUNT, NULL);

	/*
	 * [!] VALIDITY IS ASKED FIRST.  An invalid reading's ndet field means
	 * nothing at all, so a negative value sitting in it must not be reported as
	 * "the decoder does not recognise this model" -- that would send someone to
	 * re-flash a model over a stream that had simply not decoded yet.
	 */
	expect_last("an invalid reading is never read for a sentinel",
	            0u, 0u, -1, NN_LAST_NEVER, "nothing decoded yet");
}

int main(void)
{
	printf("test_nn_cmd_core:\n");
	test_f32_parts();
	test_shape();
	test_spec_accepts();
	test_spec_refuses();
	test_names();
	test_last_line();
	if (last_failures != 0) {
		printf("test_nn_cmd_core: %d failure(s)\n", last_failures);
		return 1;
	}
	printf("test_nn_cmd_core: all passed\n");
	return 0;
}
