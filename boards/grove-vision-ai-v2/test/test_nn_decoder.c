/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    test_nn_decoder.c
 * @brief   Host tests for port/npu/nn_decoder.c, this board's adapter onto the
 *          shared BlazeFace decoder (issue #97).
 *
 * The decoder's own arithmetic is covered by shell/test/test_blazeface.c, which
 * is board-independent.  What lives HERE is the part that cannot: the
 * translation from `struct npu_tensor` into `struct tensor_desc`, compiled
 * against this board's REAL headers.  A shimmed copy of npu.h could drift from
 * the firmware's without anything noticing, which is the whole reason board
 * tests exist separately from the core suite.
 *
 * WHAT THE TRANSLATION CAN GET WRONG, and therefore what is pinned:
 *
 *   - AN UNKNOWN ELEMENT TYPE READ AS int8.  npu_tensor carries an opaque
 *     TfLiteType; only npu_tensor_is_int8() knows what it means.  Mapping
 *     anything else to TENSOR_DTYPE_INT8 would make the decoder read a float
 *     buffer as bytes and return boxes computed from nothing.
 *   - A RANK-0 TENSOR TREATED AS PRESENT.  npu_tflm.cc marks a tensor it cannot
 *     represent by leaving rank 0 rather than truncating it (issue #97), so the
 *     adapter must pass that through and the decoder must refuse it.
 *   - MORE OUTPUTS THAN THE ARRAY HOLDS.  The descriptor array is fixed; a model
 *     with more outputs must be bounded, not written past.
 *
 * npu_tensor_is_int8() and log_write() are DEFINED HERE.  On the board the first
 * lives in npu_tflm.cc, the one translation unit that can see TfLiteType, and a
 * C++ static_assert there pins the enumerator; this file therefore never needs
 * to know the numeric value -- it tags its tensors with its own constant and
 * supplies the same predicate that reads it, which is exactly the contract the
 * adapter relies on.
 */
#include "nn_decoder.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

/* The tag this file uses for "int8".  Its numeric identity is the firmware's
 * business (see the file comment); nothing here depends on the value. */
#define T_INT8   9
#define T_FLOAT  1

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

/* --- the real model's quantisation (as in the core decoder test) ---------- */
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

/* Quiet tensors: the zero point, not zero -- a zero byte in the box tensors
 * dequantises to +14.4 pixels here. */
static void reset_tensors(void)
{
	for (int i = 0; i < A512; i++)
		scr512[i] = (int8_t)Z_SCR512;
	for (int i = 0; i < A384; i++)
		scr384[i] = (int8_t)Z_SCR384;
	memset(box512, (int8_t)Z_BOX512, sizeof box512);
	memset(box384, (int8_t)Z_BOX384, sizeof box384);

	memset(tens, 0, sizeof tens);
	set_tensor(&tens[0], box512, sizeof box512, A512, STRIDE, S_BOX512, Z_BOX512);
	set_tensor(&tens[1], scr512, sizeof scr512, A512, 1,      S_SCR512, Z_SCR512);
	set_tensor(&tens[2], box384, sizeof box384, A384, STRIDE, S_BOX384, Z_BOX384);
	set_tensor(&tens[3], scr384, sizeof scr384, A384, 1,      S_SCR384, Z_SCR384);
}

/* One face at anchor 0, strong enough to clear the default threshold. */
static void put_one_face(void)
{
	scr512[0] = 127;
	for (int k = 0; k < 4; k++)
		box512[0 * STRIDE + k] = 40;
}

int main(void)
{
	struct bf_det det[BF_MAX_DET];
	struct bf_result res;
	int n;

	printf("test_nn_decoder\n");

	/* --- the translation works at all ------------------------------- */
	reset_tensors();
	put_one_face();
	expect("real int8 tensors are recognised as BlazeFace",
	       nn_decoder_shapes_ok(tens, 4) != 0, "rejected");
	n = nn_decoder_run(tens, 4, det, BF_MAX_DET, &res);
	expect("and decode through the adapter", n == 1, "got %d", n);
	expect("the result carries the applied threshold",
	       res.thresh_milli == nn_decoder_get_thresh_milli(),
	       "result %u, current %u", res.thresh_milli,
	       nn_decoder_get_thresh_milli());
	expect("init on first use left the default threshold",
	       nn_decoder_get_thresh_milli() == BF_DEFAULT_THRESH_MILLI,
	       "thresh %u", nn_decoder_get_thresh_milli());

	/* --- an element type the board cannot vouch for ------------------ */
	/*
	 * [!] The adapter must not guess.  npu_tensor_is_int8() is the only type
	 * predicate the C++ side exports, so anything it rejects has to become
	 * UNSUPPORTED -- reading a float32 buffer as int8 would return boxes
	 * computed from the wrong bytes, and every shape check would still pass.
	 */
	reset_tensors();
	put_one_face();
	tens[1].type = (int8_t)T_FLOAT;
	expect("a non-int8 tensor is not vouched for",
	       nn_decoder_shapes_ok(tens, 4) == 0, "accepted");
	n = nn_decoder_run(tens, 4, det, BF_MAX_DET, &res);
	expect("and a decode reports a model error, not a decoder fault",
	       n == BF_ERR_MODEL, "got %d", n);

	/* --- rank 0 is npu_tflm.cc's "not representable" marker ---------- */
	reset_tensors();
	put_one_face();
	tens[1].rank = 0;
	expect("a rank-0 tensor is refused",
	       nn_decoder_shapes_ok(tens, 4) == 0, "accepted");
	n = nn_decoder_run(tens, 4, det, BF_MAX_DET, &res);
	expect("and it is a model error", n == BF_ERR_MODEL, "got %d", n);

	/* --- more outputs than the descriptor array holds ---------------- */
	/*
	 * The four the decoder wants are in the first slots, so bounding must not
	 * lose them; what matters is that a count beyond the array is clamped
	 * rather than walked.
	 */
	reset_tensors();
	put_one_face();
	n = nn_decoder_run(tens, NN_DECODER_MAX_OUTPUTS + 4u, det, BF_MAX_DET, &res);
	expect("an oversized output count is bounded, not walked past",
	       n == 1, "got %d", n);

	/* --- null and threshold plumbing --------------------------------- */
	expect("a null tensor array is refused by shapes_ok",
	       nn_decoder_shapes_ok(NULL, 4) == 0, "accepted");
	n = nn_decoder_run(NULL, 4, det, BF_MAX_DET, &res);
	expect("a null tensor array is an argument error, not a model error",
	       n == BF_ERR_ARG, "got %d", n);

	expect("the threshold can be set through the adapter",
	       nn_decoder_set_thresh_milli(500u) == BF_OK, "refused");
	expect("and reads back", nn_decoder_get_thresh_milli() == 500u,
	       "got %u", nn_decoder_get_thresh_milli());
	expect("an out-of-range threshold is refused",
	       nn_decoder_set_thresh_milli(0u) == BF_ERR_ARG, "accepted");
	expect("and changed nothing", nn_decoder_get_thresh_milli() == 500u,
	       "got %u", nn_decoder_get_thresh_milli());

	reset_tensors();
	put_one_face();
	n = nn_decoder_run(tens, 4, det, BF_MAX_DET, &res);
	expect("a lowered threshold is what the next decode reports",
	       n >= 1 && res.thresh_milli == 500u, "n %d thresh %u",
	       n, res.thresh_milli);

	(void)nn_decoder_set_thresh_milli(BF_DEFAULT_THRESH_MILLI);

	if (failures) {
		printf("test_nn_decoder: %d failure(s)\n", failures);
		return 1;
	}
	printf("test_nn_decoder: all cases pass\n");
	return 0;
}
