/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    test_nn_desc.c
 * @brief   Host tests for port/nn/nn_desc.c -- nn_tensor -> tensor_desc
 *          (issues #97, #116).
 *
 * This was half of test_nn_decoder.c and drove a BlazeFace decoder with it.  The
 * translation never had a model in it: `nn out`, `nn info` and the
 * active-decoder shim all need it whatever interprets the tensors -- or whether
 * anything does -- so it is tested without one, and NOTHING HERE LINKS
 * svc/blazeface.c.  Linking a decoder would only prove it still compiles.
 *
 * It stays a BOARD test rather than moving to the core suite because the point
 * is the REAL headers: `struct nn_tensor` and `enum nn_dtype` are this port's,
 * and a shimmed copy of nn.h could drift from the firmware's without anything
 * noticing.
 *
 * WHAT THE TRANSLATION CAN GET WRONG, and therefore what is pinned:
 *
 *   - AN UNSUPPORTED dtype READ AS SOMETHING ELSE.  Mapping an unknown
 *     `enum nn_dtype` onto a type a reader does read would make it interpret the
 *     buffer as bytes it is not, while every shape check downstream still passes.
 *   - A RANK ABOVE FOUR TRUNCATED.  A shortened shape can still match a lookup,
 *     so the rank is left at 0 -- "not representable" -- instead.
 *   - A FIELD LEFT OVER FROM THE PREVIOUS TENSOR.  The descriptors are written
 *     into a caller's array, often the same one twice; anything not written here
 *     would be inherited, and a stale scale is not an obvious failure -- it is a
 *     plausible number.
 *   - THE QUANTISATION NOT PASSED THROUGH AS IT IS.  This board publishes scale
 *     0 for an unquantised tensor and its graphs are float32, so a translation
 *     that invented a scale would put every value through an affine form that
 *     multiplies it by zero.
 */
#include "nn_desc.h"

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

static float buf[64];

static struct nn_tensor mk_tensor(void)
{
	struct nn_tensor t;

	memset(&t, 0, sizeof t);
	t.data       = buf;
	t.bytes      = sizeof buf;
	t.ndim       = 3;
	t.dims[0]    = 1;
	t.dims[1]    = 8;
	t.dims[2]    = 8;
	t.dtype      = NN_DTYPE_FLOAT32;
	t.scale      = 0.0369369201f;
	t.zero_point = 49;
	return t;
}

int main(void)
{
	struct nn_tensor t;
	struct tensor_desc d;
	unsigned i;
	int dims_ok;

	printf("test_nn_desc\n");

	/* --- an ordinary tensor ------------------------------------------- */
	t = mk_tensor();
	nn_desc_of(&d, &t);
	expect("the buffer and its length pass through",
	       d.data == (void *)buf && d.bytes == sizeof buf,
	       "data %p bytes %lu", d.data, (unsigned long)d.bytes);
	expect("float32 is named as itself",
	       d.dtype == (uint8_t)TENSOR_DTYPE_FLOAT32, "dtype %u",
	       (unsigned)d.dtype);
	expect("the quantisation passes through as it stands",
	       d.scale == t.scale && d.zero_point == t.zero_point,
	       "scale %f zp %ld", (double)d.scale, (long)d.zero_point);
	dims_ok = (d.rank == t.ndim);
	for (i = 0u; i < t.ndim; i++)
		if (d.dims[i] != (int32_t)t.dims[i])
			dims_ok = 0;
	expect("the shape passes through, every dimension", dims_ok != 0,
	       "rank %u", (unsigned)d.rank);

	/* --- the element types this board can name ------------------------ */
	t = mk_tensor();
	t.dtype = NN_DTYPE_INT8;
	nn_desc_of(&d, &t);
	expect("int8 is vouched for", d.dtype == (uint8_t)TENSOR_DTYPE_INT8,
	       "dtype %u", (unsigned)d.dtype);

	/* --- and the ones it cannot --------------------------------------- */
	/*
	 * [!] IT MUST NOT GUESS.  Mapping a type this board cannot name onto one a
	 * reader does read would have it take the buffer for bytes it is not, and
	 * produce numbers computed from nothing.
	 */
	t = mk_tensor();
	t.dtype = NN_DTYPE_NONE;
	nn_desc_of(&d, &t);
	expect("a tensor of no type is marked unsupported, not assumed",
	       d.dtype == (uint8_t)TENSOR_DTYPE_UNSUPPORTED, "dtype %u",
	       (unsigned)d.dtype);

	t = mk_tensor();
	t.dtype = 200u;             /* not any nn_dtype */
	nn_desc_of(&d, &t);
	expect("an out-of-range dtype is marked unsupported too",
	       d.dtype == (uint8_t)TENSOR_DTYPE_UNSUPPORTED, "dtype %u",
	       (unsigned)d.dtype);
	expect("and it still carries its buffer, for a reader to refuse knowingly",
	       d.data == (void *)buf && d.bytes == sizeof buf, "data %p", d.data);

	/* --- a rank the neutral descriptor cannot hold -------------------- */
	/*
	 * [!] REFUSED, NOT TRUNCATED.  A shape cut down to four dimensions can
	 * still match a lookup, and then a reader takes a tensor it was not looking
	 * for while every check it makes passes.  Rank 0 fails every shape test
	 * downstream, which is the answer wanted.
	 */
	t = mk_tensor();
	t.ndim = TENSOR_MAX_DIMS + 1u;
	nn_desc_of(&d, &t);
	expect("a rank above what a descriptor holds leaves rank 0", d.rank == 0,
	       "rank %u", (unsigned)d.rank);
	dims_ok = 1;
	for (i = 0u; i < TENSOR_MAX_DIMS; i++)
		if (d.dims[i] != 0)
			dims_ok = 0;
	expect("and no dimensions of it are copied", dims_ok != 0,
	       "dims %ld %ld %ld %ld", (long)d.dims[0], (long)d.dims[1],
	       (long)d.dims[2], (long)d.dims[3]);

	/* --- nothing is inherited from the previous tensor ---------------- */
	/*
	 * [!] THE DESTINATION IS DIRTIED FIRST, ON PURPOSE.  These land in a
	 * caller's array that is reused across models and across frames.  A member
	 * this function stopped writing would silently keep the last model's value.
	 */
	memset(&d, 0x5A, sizeof d);
	t = mk_tensor();
	t.ndim       = 1;
	t.dims[0]    = 4;
	t.dims[1]    = 0;
	t.dims[2]    = 0;
	t.scale      = 0.0f;
	t.zero_point = 0;
	nn_desc_of(&d, &t);
	dims_ok = 1;
	for (i = 1u; i < TENSOR_MAX_DIMS; i++)
		if (d.dims[i] != 0)
			dims_ok = 0;
	expect("a dirty descriptor keeps none of its own dimensions", dims_ok != 0,
	       "dims %ld %ld %ld", (long)d.dims[1], (long)d.dims[2],
	       (long)d.dims[3]);
	expect("nor a quantisation from whatever it described before",
	       d.scale == 0.0f && d.zero_point == 0, "scale %f zp %ld",
	       (double)d.scale, (long)d.zero_point);

	if (failures) {
		printf("test_nn_desc: %d failure(s)\n", failures);
		return 1;
	}
	printf("test_nn_desc: all cases pass\n");
	return 0;
}
