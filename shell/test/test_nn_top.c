/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    test_nn_top.c
 * @brief   Host tests for svc/nn_top.c (issue #121).
 *
 * The walk moved out of the shared command into the worker; what is pinned here
 * is that it still reports what the command used to print -- the order, the raw
 * codes, the dequantised scores, and the refusals.
 */
#include "nn_top.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

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

int main(void)
{
	struct tensor_desc t;
	struct nn_top5 top;
	int8_t q[7] = { -10, 50, 3, 50, 127, -128, 20 };
	float f[3] = { 0.25f, 0.75f, 0.5f };

	printf("test_nn_top\n");

	memset(&t, 0, sizeof t);
	t.data = q;
	t.bytes = sizeof q;
	t.dtype = TENSOR_DTYPE_INT8;
	t.scale = 0.5f;
	t.zero_point = -128;
	memset(&top, 0xA5, sizeof top);
	nn_top_of(&t, &top);
	expect("an int8 vector is read", top.status == (uint8_t)NN_TOP_OK &&
	       top.count == 7u && top.n == 5u, "status %u count %lu n %u",
	       (unsigned)top.status, (unsigned long)top.count, (unsigned)top.n);
	expect("[!] highest first", top.idx[0] == 4 && top.raw[0] == 127,
	       "idx %ld raw %ld", (long)top.idx[0], (long)top.raw[0]);
	expect("[!] ties keep the earlier index first",
	       top.idx[1] == 1 && top.idx[2] == 3, "idx %ld %ld",
	       (long)top.idx[1], (long)top.idx[2]);
	expect("scores are dequantised with the tensor's own scale and zero point",
	       top.v[0] == (127.0f + 128.0f) * 0.5f, "v %.3f", (double)top.v[0]);
	expect("integer scores keep their raw code", top.integer_scored == 1u,
	       "integer_scored %u", (unsigned)top.integer_scored);

	memset(&t, 0, sizeof t);
	t.data = f;
	t.bytes = sizeof f;
	t.dtype = TENSOR_DTYPE_FLOAT32;
	t.scale = 0.0f;                     /* meaningless for float -- ignored */
	nn_top_of(&t, &top);
	expect("[!] a float vector is read without an affine",
	       top.status == (uint8_t)NN_TOP_OK && top.n == 3u &&
	               top.idx[0] == 1 && top.v[0] == 0.75f &&
	               top.integer_scored == 0u,
	       "n %u idx %ld v %.3f", (unsigned)top.n, (long)top.idx[0],
	       (double)top.v[0]);

	t.dtype = TENSOR_DTYPE_INT16;
	nn_top_of(&t, &top);
	expect("[!] a type without a stride here is refused, not guessed at",
	       top.status == (uint8_t)NN_TOP_NO_STRIDE && top.n == 0u &&
	               top.dtype == (uint8_t)TENSOR_DTYPE_INT16,
	       "status %u n %u", (unsigned)top.status, (unsigned)top.n);

	t.data = NULL;
	nn_top_of(&t, &top);
	expect("a bufferless output is unreadable",
	       top.status == (uint8_t)NN_TOP_NO_OUTPUT, "status %u",
	       (unsigned)top.status);
	nn_top_of(NULL, &top);
	expect("and so is no output at all",
	       top.status == (uint8_t)NN_TOP_NO_OUTPUT, "status %u",
	       (unsigned)top.status);
	nn_top_of(&t, NULL);                    /* tolerated */

	if (failures) {
		printf("test_nn_top: %d failure(s)\n", failures);
		return 1;
	}
	printf("test_nn_top: all cases pass\n");
	return 0;
}
