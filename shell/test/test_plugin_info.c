/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Host unit test for svc/plugin_info.c -- `nn info`'s plugin stack lines
 * (issue #111).  No gate compares what a command prints, so the two lines are
 * pinned here BYTE FOR BYTE, from the numbers Grove's blazeface container
 * carries (c 256) and a classifier with absent param slots, plus the widest
 * line the fields can make, which must still end in its CRLF.
 */
#include "plugin_info.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static char out[1024];
static size_t out_len;

static int cap(void *ctx, const char *s, size_t len)
{
	(void)ctx;
	assert(out_len + len < sizeof out);
	memcpy(out + out_len, s, len);
	out_len += len;
	out[out_len] = '\0';
	return (int)len;
}

static void view(struct plugin_view *v, const uint32_t own[7],
                 const int32_t cr[7], uint32_t present, uint32_t c,
                 uint32_t sink)
{
	unsigned i;
	uint32_t ch = c > sink ? c : sink;

	memset(v, 0, sizeof *v);
	for (i = 0u; i < PLUGIN_SLOT_COUNT; i++) {
		v->slot[i] = ((present >> i) & 1u) ? 0x21u : PLUGIN_SLOT_ABSENT;
		v->stack_own[i] = own[i];
		v->stack[i] = own[i];
		if (cr[i] >= 0) {
			v->stack_crossing |= 1u << i;
			v->stack_cross[i] = (uint32_t)cr[i];
			if ((uint32_t)cr[i] + ch > own[i])
				v->stack[i] = (uint32_t)cr[i] + ch;
		}
	}
	v->stack_sink = sink;
}

static void expect(const char *what, const char *want)
{
	if (strcmp(out, want) != 0) {
		printf("  FAIL: %s\n  got:\n%s  want:\n%s", what, out, want);
		assert(0);
	}
	printf("  %s\n", what);
}

int main(void)
{
	static const uint32_t bf_own[7] = { 8, 40, 272, 76, 96, 8, 16 };
	static const int32_t  bf_cr[7]  = { -1, -1, 136, 76, 88, -1, -1 };
	static const uint32_t cl_own[7] = { 0, 16, 232, 36, 88, 0, 0 };
	static const int32_t  cl_cr[7]  = { -1, -1, 176, 36, 80, -1, -1 };
	static const uint32_t w_own[7]  = { 1024, 1024, 1024, 1024, 1024, 1024,
	                                    1024 };
	static const int32_t  w_cr[7]   = { 1000, 1000, 1000, 1000, 1000, 1000,
	                                    1000 };
	struct plugin_view v;

	printf("test_plugin_info (svc/plugin_info.c):\n");

	view(&v, bf_own, bf_cr, 0x7Fu, 256u, 16u);
	out_len = 0u;
	assert(plugin_info_stack(cap, NULL, &v, 256u) == 0);
	expect("a detector, every slot present",
	       "  stack : entry 8  shapes 40  decode 392  draw 332  report 344  "
	       "param 8/16 B needed at c 256 B\r\n"
	       "  decl  : own 8/40/272/76/96/8/16  at a crossing "
	       "-/-/136/76/88/-/-  sink 16 B\r\n");

	view(&v, cl_own, cl_cr, 0x1Fu, 256u, 16u);
	out_len = 0u;
	assert(plugin_info_stack(cap, NULL, &v, 256u) == 0);
	expect("a classifier, param slots absent",
	       "  stack : entry 0  shapes 16  decode 432  draw 292  report 336  "
	       "param 0/0 B needed at c 256 B\r\n"
	       "  decl  : own 0/16/232/36/88/0/0  at a crossing "
	       "-/-/176/36/80/-/-  sink 16 B\r\n");

	/* The widest the fields make: still under the 128 B line, CRLF intact. */
	view(&v, w_own, w_cr, 0x7Fu, 4294967295u, 4294967295u);
	out_len = 0u;
	assert(plugin_info_stack(cap, NULL, &v, 4294967295u) == 0);
	{
		const char *nl = strstr(out, "\r\n");

		assert(nl != NULL && (size_t)(nl - out) + 2u < 128u);
		assert(out_len - (size_t)(nl + 2 - out) < 128u);
		assert(out[out_len - 2] == '\r' && out[out_len - 1] == '\n');
		printf("  the widest lines (%u and %u B) keep their CRLF\n",
		       (unsigned)(nl + 2 - out),
		       (unsigned)(out_len - (size_t)(nl + 2 - out)));
	}

	printf("test_plugin_info: all passed\n");
	return 0;
}
