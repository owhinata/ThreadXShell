/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Host test for the display name a TFLM-based backend keeps (issue #201):
 * svc/nn/tflm/nn_tflm_glue.h's nn_tflm_set_model_name() writing into the
 * backend's own name field, which is what nn_model_name() and so `nn info`'s
 * `model :` line print.
 *
 * A blob name may be BLOB_NAME_MAX (64) characters and the backend shows it as
 * it is.  The field held 63 characters and the copy cut the rest without
 * saying so; this pins that the longest legal name comes back whole.
 *
 * Built by the board host tests, against the board's own TFLM tree: for the
 * tflm backend's state (svc/nn/tflm/nn_tflm_state.h, wio-lite-ai's tree) and,
 * with -DTEST_ETHOSU, for the Ethos-U backend's
 * (svc/nn/ethosu/nn_ethosu_state.h, grove-vision-ai-v2's SDK tree).  The state
 * is this file's own object of the shared type -- the size of the field is a
 * property of the type, not of a board's instance.
 */
#include "nn_tflm_ops.h"
#include "blob_state.h"   /* BLOB_NAME_MAX: the longest name a model can have */

#if defined(TEST_ETHOSU)
#include "nn_ethosu_state.h"
#define TEST_STATE_T nn_ethosu_state
#define TEST_STATE   nn_ethosu_board_state
#define TEST_WHAT    "ethosu"
#else
#include "nn_tflm_state.h"
#define TEST_STATE_T nn_tflm_state
#define TEST_STATE   nn_tflm_board_state
#define TEST_WHAT    "tflm"
#endif

#include <cstdio>
#include <cstring>

extern "C" {
TEST_STATE_T TEST_STATE;
}

#define NN_TFLM_GLUE_STATE TEST_STATE
#include "nn_tflm_glue.h"

static int fails;

#define CHECK(cond, ...)                                                    \
	do {                                                                \
		if (!(cond)) {                                              \
			fails++;                                            \
			std::printf("FAIL %s:%d: ", __FILE__, __LINE__);    \
			std::printf(__VA_ARGS__);                           \
			std::printf("\n");                                  \
		}                                                           \
	} while (0)

int main()
{
	/* Sized from the rule, not from the field under test: a field too small
	 * must not shrink what the test expects of it. */
	char name[BLOB_NAME_MAX + 1u];
	const char *got;

	/* The longest legal blob name, its last character distinct so a cut by one
	 * shows. */
	std::memset(name, 'a', BLOB_NAME_MAX);
	name[BLOB_NAME_MAX - 1u] = 'z';
	name[BLOB_NAME_MAX] = '\0';

	nn_tflm_set_model_name(name);
	got = TEST_STATE.tm.name;
	CHECK(std::strcmp(got, name) == 0,
	      "%s: a %u-character name reads back as %u characters (\"...%s\")",
	      TEST_WHAT, (unsigned)BLOB_NAME_MAX, (unsigned)std::strlen(got),
	      got + (std::strlen(got) > 4u ? std::strlen(got) - 4u : 0u));
	CHECK(got == TEST_STATE.model_name,
	      "%s: the handle does not point at the backend's field", TEST_WHAT);

	/* No model reads "(none)", in place of the long name. */
	nn_tflm_set_model_name(nullptr);
	CHECK(std::strcmp(TEST_STATE.tm.name, "(none)") == 0,
	      "%s: no model reads \"%s\", want \"(none)\"", TEST_WHAT,
	      TEST_STATE.tm.name);
	nn_tflm_set_model_name("");
	CHECK(std::strcmp(TEST_STATE.tm.name, "(none)") == 0,
	      "%s: an empty name reads \"%s\", want \"(none)\"", TEST_WHAT,
	      TEST_STATE.tm.name);

	/* A short name after the long one. */
	nn_tflm_set_model_name("det");
	CHECK(std::strcmp(TEST_STATE.tm.name, "det") == 0,
	      "%s: \"det\" reads \"%s\"", TEST_WHAT, TEST_STATE.tm.name);

	if (fails) {
		std::printf("test_nn_model_name (%s): %d failure(s)\n", TEST_WHAT, fails);
		return 1;
	}
	std::printf("test_nn_model_name (%s): OK\n", TEST_WHAT);
	return 0;
}
