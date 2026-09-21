/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Host tests for svc/nn_report.c (issue #110 = #78 Step 3b).
 *
 * The thing under test is a contract about OUTCOMES, not about bytes: an
 * external decoder's report is captured while its result is still protected,
 * and by the time anybody prints it the protection is gone -- so the capture
 * has to carry everything the consumer needs to say the right thing.  Zero
 * bytes is a legal report; so is "this decoder has no report to give"; so is
 * "it ran out of buffer"; so is "it refused part way".  Four different lines
 * for an operator, and a consumer that inferred them from the length would
 * print the wrong one.
 *
 * [!] AND THE OWNERSHIP IS THE POINT.  The buffer belongs to the caller's
 * frame, so two consoles capturing at once cannot overwrite each other.  The
 * last case demonstrates that rather than asserting it in a comment, because
 * a board-owned capture slot -- the obvious first design -- passes every other
 * test in this file.
 */
#include "nn_report.h"

#include <stdio.h>
#include <string.h>

static int fails;

#define CHECK(cond, what)                                                     \
	do {                                                                      \
		if (cond) {                                                           \
			printf("  ok   %s\n", (what));                                    \
		} else {                                                              \
			printf("  FAIL %s  (%s:%d)\n", (what), __FILE__, __LINE__);       \
			fails++;                                                          \
		}                                                                     \
	} while (0)

static int say(struct nn_report_capture *r, const char *s)
{
	return nn_report_write(r, s, strlen(s));
}

static int body_is(const struct nn_report_capture *r, const char *s)
{
	return r->len == (uint32_t)strlen(s) && memcmp(r->buf, s, r->len) == 0;
}

int main(void)
{
	char buf[16];
	struct nn_report_capture r = { buf, (uint32_t)sizeof buf, 0u, 0u };

	printf("test_nn_report (svc/nn_report.c):\n");

	printf("the ordinary case:\n");
	nn_report_begin(&r);
	CHECK(r.len == 0u && r.status == (uint8_t)NN_REPORT_NONE,
	      "a fresh capture holds nothing and claims nothing");
	CHECK(say(&r, "two ") == 4 && say(&r, "faces") == 5,
	      "writes report what they took");
	nn_report_end(&r, 0);
	CHECK(body_is(&r, "two faces") && r.status == (uint8_t)NN_REPORT_OK,
	      "and the bytes arrive intact");

	printf("[!] length is not a status:\n");
	nn_report_begin(&r);
	nn_report_end(&r, 0);
	CHECK(r.len == 0u && r.status == (uint8_t)NN_REPORT_OK,
	      "a decoder that said nothing SUCCEEDED at saying nothing");
	nn_report_begin(&r);
	nn_report_set(&r, NN_REPORT_UNSUPPORTED);
	CHECK(r.len == 0u && r.status == (uint8_t)NN_REPORT_UNSUPPORTED,
	      "which is a different answer from having no report to give");
	nn_report_begin(&r);
	nn_report_set(&r, NN_REPORT_STALE);
	CHECK(r.status == (uint8_t)NN_REPORT_STALE,
	      "and from the result having gone before it could be read");

	printf("refusal:\n");
	nn_report_begin(&r);
	(void)say(&r, "half");
	nn_report_end(&r, -1);
	CHECK(body_is(&r, "half") && r.status == (uint8_t)NN_REPORT_REFUSED,
	      "a decoder that stopped part way keeps what it wrote and says so");

	printf("[!] overflow latches, and outranks refusal:\n");
	nn_report_begin(&r);
	CHECK(say(&r, "0123456789") == 10, "ten bytes fit");
	CHECK(say(&r, "abcdefghij") < 0,
	      "the next ten do not, and the write says so");
	CHECK(r.len == 16u && memcmp(r.buf, "0123456789abcdef", 16u) == 0,
	      "[!] the prefix that FITS is kept -- a truncated first line is more "
	      "use than none");
	CHECK(say(&r, "z") < 0 && r.len == 16u,
	      "[!] and it has said no once and for all: a decoder that kept "
	      "writing cannot leave a hole in the middle");
	nn_report_end(&r, -1);
	CHECK(r.status == (uint8_t)NN_REPORT_TRUNCATED,
	      "[!] the negative return was THIS sink's doing, so it is not "
	      "reported as the decoder failing");
	nn_report_end(&r, 0);
	CHECK(r.status == (uint8_t)NN_REPORT_TRUNCATED,
	      "and a later success does not overwrite it either");

	printf("degenerate sinks:\n");
	{
		struct nn_report_capture n = { NULL, 0u, 0u, 0u };

		nn_report_begin(&n);
		CHECK(say(&n, "x") < 0 && n.len == 0u,
		      "a capture with no buffer refuses rather than writing");
		nn_report_end(&n, 0);
		CHECK(n.status == (uint8_t)NN_REPORT_TRUNCATED,
		      "and reports that nothing could be taken");
	}
	nn_report_begin(&r);
	CHECK(nn_report_write(&r, "x", 0u) == 0 && r.len == 0u,
	      "a zero-length write is not an overflow");
	CHECK(nn_report_write(NULL, "x", 1u) < 0 && nn_report_write(&r, NULL, 1u) < 0,
	      "nulls are refused");
	nn_report_begin(NULL);
	nn_report_end(NULL, 0);
	nn_report_set(NULL, NN_REPORT_OK);
	CHECK(1, "and none of the helpers mind a null capture");

	printf("[!] two callers, two frames:\n");
	{
		char a_buf[16], b_buf[16];
		struct nn_report_capture a = { a_buf, 16u, 0u, 0u };
		struct nn_report_capture b = { b_buf, 16u, 0u, 0u };

		/* Interleaved exactly as two consoles would: A captures, B captures
		 * over the top, A prints.  With a board-owned slot A would print B's
		 * bytes; the buffer is the CALLER's, so it cannot. */
		nn_report_begin(&a);
		(void)say(&a, "console A");
		nn_report_end(&a, 0);

		nn_report_begin(&b);
		(void)say(&b, "console B");
		nn_report_end(&b, 0);

		CHECK(body_is(&a, "console A") && body_is(&b, "console B"),
		      "[!] each caller prints its own capture, whatever the other did "
		      "in between");
		CHECK(a.status == (uint8_t)NN_REPORT_OK &&
		              b.status == (uint8_t)NN_REPORT_OK,
		      "and its own outcome with it");
	}

	if (fails != 0) {
		printf("test_nn_report: %d FAILED\n", fails);
		return 1;
	}
	printf("test_nn_report: all passed\n");
	return 0;
}
