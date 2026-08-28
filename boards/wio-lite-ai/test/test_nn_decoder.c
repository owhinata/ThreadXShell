/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    test_nn_decoder.c
 * @brief   Host tests for port/nn/nn_decoder.c, this board's adapter onto the
 *          shared BlazeFace decoder (issue #97).
 *
 * The decoder's arithmetic is covered by shell/test/test_blazeface.c, which is
 * board-independent.  What lives HERE is the half that cannot be: `nn_tensor` ->
 * `tensor_desc`, compiled against this board's REAL headers.
 *
 * [!] AND AGAINST THE REAL mem_sections.h.  The candidate scratch carries
 * PSRAM_AI, and building this test with the firmware's own definition of that
 * macro is what keeps a shimmed copy from drifting away from it unnoticed -- the
 * same reason the decoder test used to be built this way before the decoder
 * became shared.  check_psram_ai_residency.py names `nn_dec_scratch` on the
 * linked image; this is the other end of that.
 *
 * WHAT THE TRANSLATION CAN GET WRONG, and therefore what is pinned:
 *
 *   - AN UNSUPPORTED dtype READ AS SOMETHING ELSE.  Mapping an unknown
 *     `enum nn_dtype` onto a type the decoder does read would make it interpret
 *     the buffer as bytes it is not.
 *   - float32 PUT THROUGH THE AFFINE FORM.  This board publishes scale 0 for an
 *     unquantised tensor, so a decoder that dequantised unconditionally would
 *     multiply every value by zero -- and this is the board where that would
 *     actually happen, since its graphs are float32.
 *   - A RANK ABOVE FOUR TRUNCATED.  A shortened shape can still match a lookup.
 *   - A HOLE IN THE OUTPUT SET reported as a model-shape problem.
 *
 * struct nn_model is opaque in nn.h (it is defined in nn.c), so the accessors the
 * adapter uses are stubbed here -- which is also what makes the adapter testable
 * at all.
 */
#include "nn_decoder.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define A512    512
#define A384    384
#define STRIDE  16

/* --- the model handle the adapter pulls tensors from --------------------- */

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

/* --- tensors ------------------------------------------------------------- */

static float box512[A512 * STRIDE];
static float scr512[A512];
static float box384[A384 * STRIDE];
static float scr384[A384];

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
	/* scale/zero_point left at 0, which is exactly what this board publishes
	 * for an unquantised tensor.  A decoder that demanded a usable scale before
	 * looking at the type would reject every output this board produces. */
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

/* One face at anchor 0: raw score well over the default threshold, and a box
 * with no offset so it lands on the anchor centre. */
static void put_one_face(void)
{
	scr512[0] = 2.0f;
	box512[0] = 0.0f;
	box512[1] = 0.0f;
	box512[2] = 20.0f;
	box512[3] = 20.0f;
}

int main(void)
{
	struct bf_det det[BF_MAX_DET];
	struct bf_result res;
	int n;

	printf("test_nn_decoder\n");

	/* --- the float32 path works end to end --------------------------- */
	reset_model();
	put_one_face();
	n = nn_decoder_run(&stub, det, BF_MAX_DET, &res);
	expect("a float32 model decodes through the adapter", n == 1, "got %d", n);
	expect("scale 0 was not applied to it",
	       res.max_score > 1.9f && res.max_score < 2.1f,
	       "peak %.4f, expected ~2.0", (double)res.max_score);
	expect("the result carries the applied threshold",
	       res.thresh_milli == nn_decoder_get_thresh_milli(),
	       "result %u, current %u", res.thresh_milli,
	       nn_decoder_get_thresh_milli());
	expect("init on first use left the default threshold",
	       nn_decoder_get_thresh_milli() == BF_DEFAULT_THRESH_MILLI,
	       "thresh %u", nn_decoder_get_thresh_milli());

	/* --- an unsupported element type --------------------------------- */
	reset_model();
	put_one_face();
	stub.out[1].dtype = NN_DTYPE_NONE;
	n = nn_decoder_run(&stub, det, BF_MAX_DET, &res);
	expect("an unsupported dtype makes it not-BlazeFace, not a decoder fault",
	       n == BF_ERR_MODEL, "got %d", n);

	reset_model();
	put_one_face();
	stub.out[1].dtype = 200u;   /* not any nn_dtype */
	n = nn_decoder_run(&stub, det, BF_MAX_DET, &res);
	expect("an out-of-range dtype is refused too", n == BF_ERR_MODEL,
	       "got %d", n);

	/* --- rank above four --------------------------------------------- */
	reset_model();
	put_one_face();
	stub.out[1].ndim = 5;
	n = nn_decoder_run(&stub, det, BF_MAX_DET, &res);
	expect("a rank above four is refused, not truncated into a match",
	       n == BF_ERR_MODEL, "got %d", n);

	/* --- a hole in the output set ------------------------------------ */
	reset_model();
	put_one_face();
	stub.out[1].data  = NULL;
	stub.out[1].bytes = 0u;      /* nn_output() returns NULL for this one */
	n = nn_decoder_run(&stub, det, BF_MAX_DET, &res);
	expect("a missing output tensor is a model error, not a crash",
	       n == BF_ERR_MODEL, "got %d", n);

	/* --- more outputs than the model handle can hold ------------------ */
	reset_model();
	put_one_face();
	stub.n = NN_MAX_IO;          /* the extras are zeroed, i.e. holes */
	n = nn_decoder_run(&stub, det, BF_MAX_DET, &res);
	expect("extra empty outputs do not stop the four being found", n == 1,
	       "got %d", n);

	/* --- null handle -------------------------------------------------- */
	n = nn_decoder_run(NULL, det, BF_MAX_DET, &res);
	expect("a null model is an argument error, not a model error",
	       n == BF_ERR_ARG, "got %d", n);

	/* --- threshold plumbing ------------------------------------------- */
	expect("the threshold can be set through the adapter",
	       nn_decoder_set_thresh_milli(500u) == BF_OK, "refused");
	expect("and reads back", nn_decoder_get_thresh_milli() == 500u,
	       "got %u", nn_decoder_get_thresh_milli());
	expect("an out-of-range threshold is refused",
	       nn_decoder_set_thresh_milli(1000u) == BF_ERR_ARG, "accepted");
	expect("and changed nothing", nn_decoder_get_thresh_milli() == 500u,
	       "got %u", nn_decoder_get_thresh_milli());

	reset_model();
	put_one_face();
	n = nn_decoder_run(&stub, det, BF_MAX_DET, &res);
	expect("the next decode reports the threshold it used",
	       n >= 1 && res.thresh_milli == 500u, "n %d thresh %u", n,
	       res.thresh_milli);

	(void)nn_decoder_set_thresh_milli(BF_DEFAULT_THRESH_MILLI);

	if (failures) {
		printf("test_nn_decoder: %d failure(s)\n", failures);
		return 1;
	}
	printf("test_nn_decoder: all cases pass\n");
	return 0;
}
