/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Host test for the build-time length check on a board's `nn` wording
 * (svc/nn_svc.h: NN_SVC_DETAIL_CHECK / _LIT / _CHECK_FMT, issue #122 P15).
 *
 * WHY A COMPILE TEST.  The check fires in the compiler, so the only way to see
 * it refuse is to compile something it must refuse.  run_host_tests.sh builds
 * this file once plainly -- the boundary, exactly NN_SVC_DETAIL_MAX characters,
 * must be ACCEPTED and must survive the copy whole -- and once per NN_DC_OVER_*
 * macro below, each of which must FAIL to compile, naming the assertion.  A gate
 * nobody has seen fail is not a gate: one of wio's refusals had been 164
 * characters for as long as it existed, and on hardware it ended "or see `d".
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "nn_svc.h"

/* Ten characters, so the boundary is written out by counting rather than by
 * using the limit itself -- a test in the limit's own terms passes whatever
 * the limit is. */
#define TEN "0123456789"
/* 159 = NN_SVC_DETAIL_MAX. */
#define S159 TEN TEN TEN TEN TEN TEN TEN TEN TEN TEN TEN TEN TEN TEN TEN "012345678"
/* 160: one over. */
#define S160 S159 "x"

/* The shape every board adapter uses: the format is checked, then formatted. */
static void detail_to(char *dst, size_t cap, const char *fmt, const char *arg)
{
	(void)snprintf(dst, cap, fmt, arg);
}
#define detail_set(...)                                                     \
	((void)NN_SVC_DETAIL_CHECK_FMT(__VA_ARGS__),                        \
	 detail_to(res.detail, sizeof res.detail, __VA_ARGS__))

static const char *table(int i)
{
	switch (i) {
	case 0:
		return NN_SVC_DETAIL_LIT(S159);
#if defined(NN_DC_OVER_LIT)
	case 1:
		return NN_SVC_DETAIL_LIT(S160);       /* must not compile */
#endif
#if defined(NN_DC_NOT_LITERAL)
	case 2: {
		const char *p = "short";

		return NN_SVC_DETAIL_LIT(p);          /* must not compile */
	}
#endif
	default:
		return NN_SVC_DETAIL_LIT("x");
	}
}

int main(void)
{
	struct nn_op_result res;

	printf("test_nn_detail_check:\n");

	/* The boundary is accepted, and it is the boundary: the field holds it
	 * whole, so the check and the copy agree about what "too long" is. */
	assert(strlen(S159) == (size_t)NN_SVC_DETAIL_MAX);
	assert(strlen(table(0)) == (size_t)NN_SVC_DETAIL_MAX);
	assert(sizeof res.detail == (size_t)NN_SVC_DETAIL_MAX + 1u);

	memset(&res, 0, sizeof res);
	detail_set("%s", S159);
	assert(strcmp(res.detail, S159) == 0);

	/* A format with conversions is checked by its own length: 157 fixed
	 * characters and a "%s" are 159, and pass. */
	detail_set(TEN TEN TEN TEN TEN TEN TEN TEN TEN TEN TEN TEN TEN TEN TEN
	           "0123456" "%s", "");
	assert(strlen(res.detail) == 157u);

#if defined(NN_DC_OVER_FMT)
	detail_set(S160 "%s", "");                     /* must not compile */
#endif

	printf("  159 characters accepted and copied whole                 ok\n");
	printf("test_nn_detail_check: all passed\n");
	return 0;
}
