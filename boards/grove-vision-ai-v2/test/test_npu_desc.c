/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    test_npu_desc.c
 * @brief   Host tests for port/npu/npu_desc.c -- npu_tensor -> tensor_desc
 *          (issues #97, #104).
 *
 * This file used to be test_nn_decoder.c and drove a BlazeFace decoder living in
 * the firmware.  Issue #104 removed that decoder; what it tested that still
 * exists is the TRANSLATION, which never had a model in it and which `nn out`,
 * `nn info` and the active-decoder shim all need whatever interprets the
 * tensors -- or whether anything does.
 *
 * It stays a BOARD test rather than moving to the core suite because the point
 * is the REAL headers: `struct npu_tensor` is this port's, and a shimmed copy of
 * npu.h could drift from the firmware's without anything noticing.
 *
 * WHAT THE TRANSLATION CAN GET WRONG, and therefore what is pinned:
 *
 *   - AN UNKNOWN ELEMENT TYPE READ AS int8.  npu_tensor carries an opaque
 *     TfLiteType; only npu_tensor_is_int8() knows what it means.  Mapping
 *     anything else to TENSOR_DTYPE_INT8 would have a reader take a float
 *     buffer for bytes and produce numbers computed from nothing -- and every
 *     shape check downstream would still pass.
 *   - A RANK-0 TENSOR TREATED AS PRESENT.  npu_tflm.cc marks a tensor it cannot
 *     represent by leaving rank 0 rather than truncating it (issue #97), so the
 *     translation must pass that through for a reader to refuse.
 *   - A FIELD LEFT OVER FROM THE PREVIOUS TENSOR.  The descriptors are written
 *     into a caller's array, often the same one twice; anything not written here
 *     would be inherited, and a stale scale reads as a measurement.
 *
 * npu_tensor_is_int8() is DEFINED HERE.  On the board it lives in npu_tflm.cc,
 * the one translation unit that can see TfLiteType, where a C++ static_assert
 * pins the enumerator; this file therefore never needs to know the numeric value
 * -- it tags its tensors with its own constant and supplies the same predicate
 * that reads it, which is exactly the contract the translation relies on.
 */
#include "npu_desc.h"

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

static int8_t buf[64];

static struct npu_tensor mk_tensor(void)
{
	struct npu_tensor t;

	memset(&t, 0, sizeof t);
	t.data       = buf;
	t.bytes      = sizeof buf;
	t.rank       = 3;
	t.dims[0]    = 1;
	t.dims[1]    = 8;
	t.dims[2]    = 8;
	t.type       = (int8_t)T_INT8;
	t.scale      = 0.0369369201f;
	t.zero_point = 49;
	return t;
}

int main(void)
{
	struct npu_tensor t;
	struct tensor_desc d;
	unsigned i;
	int dims_ok;

	printf("test_npu_desc\n");

	/* --- an ordinary int8 tensor ------------------------------------- */
	t = mk_tensor();
	npu_desc_of(&d, &t);
	expect("the buffer and its length pass through",
	       d.data == buf && d.bytes == sizeof buf, "data %p bytes %lu",
	       d.data, (unsigned long)d.bytes);
	expect("int8 is vouched for", d.dtype == (uint8_t)TENSOR_DTYPE_INT8,
	       "dtype %u", (unsigned)d.dtype);
	expect("the quantisation passes through",
	       d.scale == t.scale && d.zero_point == t.zero_point,
	       "scale %f zp %ld", (double)d.scale, (long)d.zero_point);
	dims_ok = (d.rank == t.rank);
	for (i = 0u; i < TENSOR_MAX_DIMS; i++)
		if (d.dims[i] != t.dims[i])
			dims_ok = 0;
	expect("the shape passes through, every dimension", dims_ok != 0,
	       "rank %u", (unsigned)d.rank);

	/* --- an element type the board cannot vouch for ------------------ */
	/*
	 * [!] IT MUST NOT GUESS.  npu_tensor_is_int8() is the only type predicate
	 * the C++ side exports, so anything it rejects has to become UNSUPPORTED.
	 * A reader then refuses the tensor instead of reading its bytes as a type
	 * they are not.
	 */
	t = mk_tensor();
	t.type = (int8_t)T_FLOAT;
	npu_desc_of(&d, &t);
	expect("a non-int8 tensor is marked unsupported, not assumed",
	       d.dtype == (uint8_t)TENSOR_DTYPE_UNSUPPORTED, "dtype %u",
	       (unsigned)d.dtype);
	expect("and it still carries its buffer, for a reader to refuse knowingly",
	       d.data == buf && d.bytes == sizeof buf, "data %p", d.data);

	/* --- rank 0 is npu_tflm.cc's "not representable" marker ---------- */
	t = mk_tensor();
	t.rank = 0;
	npu_desc_of(&d, &t);
	expect("rank 0 passes through rather than being repaired", d.rank == 0,
	       "rank %u", (unsigned)d.rank);

	/* --- nothing is inherited from the previous tensor ---------------- */
	/*
	 * [!] THE DESTINATION IS DIRTIED FIRST, ON PURPOSE.  These land in a
	 * caller's array that is reused across models and across frames.  A member
	 * this function stopped writing would silently keep the last model's value,
	 * and a stale scale is not an obvious failure -- it is a plausible number.
	 */
	memset(&d, 0x5A, sizeof d);
	t = mk_tensor();
	t.rank    = 1;
	t.dims[0] = 4;
	t.dims[1] = 0;
	t.dims[2] = 0;
	t.dims[3] = 0;
	npu_desc_of(&d, &t);
	dims_ok = 1;
	for (i = 1u; i < TENSOR_MAX_DIMS; i++)
		if (d.dims[i] != 0)
			dims_ok = 0;
	expect("a dirty descriptor keeps nothing of its own", dims_ok != 0,
	       "dims %ld %ld %ld", (long)d.dims[1], (long)d.dims[2],
	       (long)d.dims[3]);

	if (failures) {
		printf("test_npu_desc: %d failure(s)\n", failures);
		return 1;
	}
	printf("test_npu_desc: all cases pass\n");
	return 0;
}
