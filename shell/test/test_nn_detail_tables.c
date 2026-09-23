/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Host test: the svc/ SHARED sentence tables fit a board's `nn` explanation
 * (issue #122 P15, review CONCERN 1).
 *
 * A board copies plugin_result_name() and plugin_run_strerror() whole into
 * struct nn_op_result::detail.  A board's OWN tables are checked where they are
 * written, with NN_SVC_DETAIL_LIT -- but svc/plugin_load.c and plugin_exec.c
 * sit below the `nn` contract and must not depend on it.  So their sentences are
 * checked here instead, by ENUMERATION rather than by a list: every integer
 * around the enumerations is asked for its name, so a sentence added to either
 * table is covered without anyone adding it here.  (Listing the enumerators
 * would pass for a new one nobody listed, which is the gap this closes.)
 */
#include <stdio.h>
#include <string.h>

#include "nn_detail.h"
#include "plugin_exec.h"
#include "plugin_load.h"

/* Wider than either enumeration by far; both start at 0 and count up. */
#define PROBE_LO (-256)
#define PROBE_HI 256

static int check(const char *table, int v, const char *s)
{
	if (s == NULL) {
		printf("  FAIL %s(%d) returned NULL\n", table, v);
		return 1;
	}
	if (strlen(s) > (size_t)NN_SVC_DETAIL_MAX) {
		printf("  FAIL %s(%d) is %u characters, over NN_SVC_DETAIL_MAX (%u): "
		       "\"%s\"\n", table, v, (unsigned)strlen(s),
		       (unsigned)NN_SVC_DETAIL_MAX, s);
		return 1;
	}
	return 0;
}

int main(void)
{
	int v, bad = 0, n = 0;
	size_t longest = 0u;

	printf("test_nn_detail_tables:\n");
	for (v = PROBE_LO; v <= PROBE_HI; v++) {
		const char *a = plugin_result_name((enum plugin_result)v);
		const char *b = plugin_run_strerror((enum plugin_run_result)v);

		bad += check("plugin_result_name", v, a);
		bad += check("plugin_run_strerror", v, b);
		if (a != NULL && strlen(a) > longest)
			longest = strlen(a);
		if (b != NULL && strlen(b) > longest)
			longest = strlen(b);
		n += 2;
	}
	if (bad != 0) {
		printf("test_nn_detail_tables: %d failure(s)\n", bad);
		return 1;
	}
	printf("  ok   %d lookups, longest sentence %u of %u\n", n,
	       (unsigned)longest, (unsigned)NN_SVC_DETAIL_MAX);
	printf("test_nn_detail_tables: all passed\n");
	return 0;
}
