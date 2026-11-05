/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Host test for the shared blob store results (svc/blob/blob_err.h, issue
 * #194).
 *
 * [!] THE NUMBERS ARE PINNED HERE, WRITTEN OUT, not taken from the header.
 * Every board's store returns them and every caller compares against them by
 * name, so a renumbering compiles cleanly and passes every test that uses the
 * names -- while the boards that print a result raw ("failed (%d)") start
 * saying something else, and two results that collide become one answer.  So
 * the values are written out below, and every pair is checked to differ.
 */
#include <stdio.h>

#include "blob_err.h"

static int failures;

#define CHECK(cond, ...)                                                       \
	do {                                                                   \
		if (!(cond)) {                                                 \
			printf("FAIL %s:%d: ", __FILE__, __LINE__);            \
			printf(__VA_ARGS__);                                   \
			printf("\n");                                          \
			failures++;                                            \
		}                                                              \
	} while (0)

static const struct { int got; int want; const char *name; } pinned[] = {
	{ BLOB_OK,              0, "BLOB_OK"            },
	{ BLOB_ERR_PARAM,      -1, "BLOB_ERR_PARAM"     },
	{ BLOB_ERR_IO,         -2, "BLOB_ERR_IO"        },
	{ BLOB_ERR_STATE,      -3, "BLOB_ERR_STATE"     },
	{ BLOB_ERR_BUSY,       -4, "BLOB_ERR_BUSY"      },
	{ BLOB_ERR_EMPTY,      -5, "BLOB_ERR_EMPTY"     },
	{ BLOB_ERR_CRC,        -6, "BLOB_ERR_CRC"       },
	{ BLOB_ERR_MAP,        -7, "BLOB_ERR_MAP"       },
	{ BLOB_ERR_CANCEL,     -8, "BLOB_ERR_CANCEL"    },
	{ BLOB_ERR_DUPLICATE,  -9, "BLOB_ERR_DUPLICATE" },
	{ BLOB_ERR_FAULT,     -10, "BLOB_ERR_FAULT"     },
};

#define N_PINNED (sizeof pinned / sizeof pinned[0])

int main(void)
{
	unsigned i, j;

	for (i = 0u; i < N_PINNED; i++) {
		CHECK(pinned[i].got == pinned[i].want, "%s is %d, pinned at %d",
		      pinned[i].name, pinned[i].got, pinned[i].want);
		/* Every failure is negative, so a caller's `rc != BLOB_OK` and a
		 * board's `rc < 0` agree. */
		CHECK(i == 0u || pinned[i].got < 0, "%s is not negative",
		      pinned[i].name);
		for (j = i + 1u; j < N_PINNED; j++)
			CHECK(pinned[i].got != pinned[j].got,
			      "%s and %s are both %d", pinned[i].name,
			      pinned[j].name, pinned[i].got);
	}

	if (failures != 0) {
		printf("test_blob_err: %d failure(s)\n", failures);
		return 1;
	}
	printf("test_blob_err: all passed\n");
	return 0;
}
