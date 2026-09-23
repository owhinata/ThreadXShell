/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Host test: the C++ spelling of the build-time wording check (svc/nn_detail.h,
 * issue #122).  A board's C++ table -- grove-vision-ai-v2's npu_status_name() --
 * cannot use the C form (a struct cannot be defined inside sizeof in C++), so
 * nn_detail.h gives it a template.  Same shape as test_nn_detail_check.c:
 * run_host_tests.sh builds this plainly, where the boundary must pass, and once
 * per NN_DC_* macro, where it must fail ON THE GATE.
 */
#include <cstdio>
#include <cstring>

#include "nn_detail.h"

#define TEN "0123456789"
#define S159 TEN TEN TEN TEN TEN TEN TEN TEN TEN TEN TEN TEN TEN TEN TEN "012345678"

static const char *table(int i)
{
	switch (i) {
	case 0:
		return NN_SVC_DETAIL_LIT(S159);
#if defined(NN_DC_OVER_LIT)
	case 1:
		return NN_SVC_DETAIL_LIT(S159 "x");   /* must not compile */
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

int main()
{
	if (std::strlen(table(0)) != (size_t)NN_SVC_DETAIL_MAX) {
		std::printf("test_nn_detail_check_cxx: the boundary is not %u\n",
		            (unsigned)NN_SVC_DETAIL_MAX);
		return 1;
	}
	std::printf("test_nn_detail_check_cxx: 159 characters accepted (C++)\n");
	return 0;
}
