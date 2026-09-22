/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Host unit test for port/npu/nn_probe.c -- the pure half of the plugin stack
 * probe (issue #119).
 *
 * WHY THIS FILE.  What the probe reports becomes the input to a stack allowance
 * (port/npu/nn_plugin_stack.h), and every way it can be wrong is wrong in the
 * UNSAFE direction or is invisible on the board:
 *
 *   - a sample taken on a stack it was not attributed to -- an exception
 *     handler, a thread nobody named, a stack pointer outside the thread's
 *     stack -- would become a depth that is simply false.  Those must be
 *     counted as invalid and change no depth;
 *   - a fresh record is all zeros, and a minimum that starts at 0 never moves:
 *     "least left" would read 0 for ever, or -- the other way round -- a first
 *     observation would be compared against a 0 that was never observed;
 *   - coverage is per thread.  A console sample must not mark the background
 *     job as measured, or the report says a path was walked when it was not;
 *   - the report line is one of at most twelve, 96 B each, and the caller stops
 *     at the first empty one.  "Not measured" must still be a line, and the
 *     longest line the board can produce must fit.
 *
 * None of these can be staged on the board on purpose.
 */
#include "nn_probe.h"
#include "nn_svc.h"      /* NN_STREAM_LINE_MAX */

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

/* Four contexts told apart by priority AND stack, like the board's. */
static const struct nn_probe_thread_class tab[NN_PROBE_CTX_COUNT] = {
	[NN_PROBE_PRODUCER] = { 10u, 8192u },
	[NN_PROBE_PANEL]    = {  9u, 2048u },
	[NN_PROBE_CONSOLE]  = { 16u, 4096u },
	[NN_PROBE_BG]       = { 17u, 4096u },
};

static void test_classify(void)
{
	printf("classify:\n");
	expect("the producer", nn_probe_classify(tab, 10u, 8192u) ==
	       NN_PROBE_PRODUCER, "no");
	expect("the panel", nn_probe_classify(tab, 9u, 2048u) == NN_PROBE_PANEL,
	       "no");
	expect("a console", nn_probe_classify(tab, 16u, 4096u) ==
	       NN_PROBE_CONSOLE, "no");
	expect("[!] a background job -- same stack as a console, told apart by "
	       "priority", nn_probe_classify(tab, 17u, 4096u) == NN_PROBE_BG,
	       "no");
	expect("[!] a console's priority on another stack is nobody",
	       nn_probe_classify(tab, 16u, 2048u) == NN_PROBE_UNKNOWN, "matched");
	expect("and the producer's stack at another priority is nobody",
	       nn_probe_classify(tab, 15u, 8192u) == NN_PROBE_UNKNOWN, "matched");
	expect("no table is nobody",
	       nn_probe_classify(NULL, 16u, 4096u) == NN_PROBE_UNKNOWN, "matched");
}

static void test_measure(void)
{
	const uintptr_t lo = 0x20001000u;
	const uint32_t size = 4096u;
	uint32_t d = 0xAAAAu, l = 0xBBBBu;

	printf("measure:\n");
	expect("inside: depth is from the TOP of the stack",
	       nn_probe_measure(lo + size - 1000u, lo, size, 0u, &d, &l) ==
	       NN_PROBE_OK && d == 1000u && l == 3096u, "d %u l %u", d, l);
	expect("and the bytes still to be pushed are added to it",
	       nn_probe_measure(lo + size - 1000u, lo, size, 56u, &d, &l) ==
	       NN_PROBE_OK && d == 1056u && l == 3040u, "d %u l %u", d, l);
	expect("at the bottom: the whole stack spent, nothing left",
	       nn_probe_measure(lo, lo, size, 0u, &d, &l) == NN_PROBE_OK &&
	       d == size && l == 0u, "d %u l %u", d, l);
	expect("[!] past the bottom once the extra is added: still a measurement, "
	       "left 0", nn_probe_measure(lo + 8u, lo, size, 64u, &d, &l) ==
	       NN_PROBE_OK && d == size + 56u && l == 0u, "d %u l %u", d, l);
	expect("at the top: nothing spent but the extra",
	       nn_probe_measure(lo + size, lo, size, 8u, &d, &l) == NN_PROBE_OK &&
	       d == 8u && l == size - 8u, "d %u l %u", d, l);

	d = 0xAAAAu; l = 0xBBBBu;
	expect("[!] below the stack: invalid, and neither output is touched",
	       nn_probe_measure(lo - 4u, lo, size, 0u, &d, &l) ==
	       NN_PROBE_OUT_OF_RANGE && d == 0xAAAAu && l == 0xBBBBu,
	       "d %#x l %#x", d, l);
	expect("above the stack: invalid",
	       nn_probe_measure(lo + size + 4u, lo, size, 0u, &d, &l) ==
	       NN_PROBE_OUT_OF_RANGE && d == 0xAAAAu, "d %#x", d);
	expect("a host build's NN_PROBE_SP() (0) is in nobody's stack",
	       nn_probe_measure(NN_PROBE_SP(), lo, size, 0u, &d, &l) ==
	       NN_PROBE_OUT_OF_RANGE, "measured");
	expect("a zero-size stack is not a stack",
	       nn_probe_measure(lo, lo, 0u, 0u, &d, &l) == NN_PROBE_BAD_STACK,
	       "measured");
	expect("nor is one that wraps the address space",
	       nn_probe_measure(UINTPTR_MAX - 4u, UINTPTR_MAX - 8u, 4096u, 0u, &d,
	                        &l) == NN_PROBE_BAD_STACK, "measured");
	expect("an extra that would wrap saturates",
	       nn_probe_measure(lo, lo, size, UINT32_MAX, &d, &l) == NN_PROBE_OK &&
	       d == UINT32_MAX && l == 0u, "d %u", d);
	expect("no place to put the answer is refused",
	       nn_probe_measure(lo + 8u, lo, size, 0u, NULL, &l) ==
	       NN_PROBE_BAD_STACK, "measured");
}

static void test_record(void)
{
	struct nn_probe_row r;

	printf("record:\n");
	memset(&r, 0, sizeof r);
	nn_probe_record(&r, NN_PROBE_CONSOLE, 1200u, 2896u, 4096u);
	expect("[!] the first observation sets the minimum -- the zero it starts "
	       "at was never observed",
	       r.cell[NN_PROBE_CONSOLE].left_min == 2896u &&
	       r.cell[NN_PROBE_CONSOLE].depth_hw == 1200u &&
	       r.cell[NN_PROBE_CONSOLE].stack == 4096u &&
	       r.cell[NN_PROBE_CONSOLE].hits == 1u,
	       "hw %u left %u stack %u hits %u",
	       r.cell[NN_PROBE_CONSOLE].depth_hw,
	       r.cell[NN_PROBE_CONSOLE].left_min, r.cell[NN_PROBE_CONSOLE].stack,
	       r.cell[NN_PROBE_CONSOLE].hits);

	nn_probe_record(&r, NN_PROBE_CONSOLE, 900u, 3196u, 4096u);
	expect("a shallower one lowers neither the high-water nor the minimum",
	       r.cell[NN_PROBE_CONSOLE].depth_hw == 1200u &&
	       r.cell[NN_PROBE_CONSOLE].left_min == 2896u &&
	       r.cell[NN_PROBE_CONSOLE].hits == 2u, "hw %u left %u",
	       r.cell[NN_PROBE_CONSOLE].depth_hw,
	       r.cell[NN_PROBE_CONSOLE].left_min);
	nn_probe_record(&r, NN_PROBE_CONSOLE, 1500u, 2596u, 4096u);
	expect("a deeper one raises the high-water and lowers the minimum",
	       r.cell[NN_PROBE_CONSOLE].depth_hw == 1500u &&
	       r.cell[NN_PROBE_CONSOLE].left_min == 2596u, "hw %u left %u",
	       r.cell[NN_PROBE_CONSOLE].depth_hw,
	       r.cell[NN_PROBE_CONSOLE].left_min);
	nn_probe_record(&r, NN_PROBE_CONSOLE, 1300u, 700u, 2000u);
	expect("[!] the minimum is its own fact: a smaller stack can leave less "
	       "at a shallower depth, and the stack it was out of goes with it",
	       r.cell[NN_PROBE_CONSOLE].left_min == 700u &&
	       r.cell[NN_PROBE_CONSOLE].stack == 2000u &&
	       r.cell[NN_PROBE_CONSOLE].depth_hw == 1500u,
	       "left %u stack %u hw %u", r.cell[NN_PROBE_CONSOLE].left_min,
	       r.cell[NN_PROBE_CONSOLE].stack, r.cell[NN_PROBE_CONSOLE].depth_hw);

	expect("[!] coverage is per thread: a console sample does not measure "
	       "the background job", r.cell[NN_PROBE_BG].hits == 0u &&
	       r.cell[NN_PROBE_PRODUCER].hits == 0u &&
	       r.cell[NN_PROBE_PANEL].hits == 0u, "bg %u", r.cell[NN_PROBE_BG].hits);

	nn_probe_record(&r, NN_PROBE_UNKNOWN, 64u, 0u, 4096u);
	expect("[!] an unattributed context is counted invalid and changes no depth",
	       r.invalid == 1u && r.cell[NN_PROBE_CONSOLE].hits == 4u &&
	       r.cell[NN_PROBE_CONSOLE].left_min == 700u &&
	       r.cell[NN_PROBE_BG].hits == 0u, "invalid %u", r.invalid);
	nn_probe_reject(&r);
	expect("and so is a rejected sample", r.invalid == 2u, "invalid %u",
	       r.invalid);

	r.cell[NN_PROBE_BG].hits = UINT32_MAX;
	r.invalid = UINT32_MAX;
	nn_probe_record(&r, NN_PROBE_BG, 10u, 10u, 4096u);
	nn_probe_reject(&r);
	expect("the counts saturate rather than wrap to 'never observed'",
	       r.cell[NN_PROBE_BG].hits == UINT32_MAX && r.invalid == UINT32_MAX,
	       "hits %u invalid %u", r.cell[NN_PROBE_BG].hits, r.invalid);
	nn_probe_record(NULL, NN_PROBE_BG, 1u, 1u, 1u);
	nn_probe_reject(NULL);
	expect("and no record is no crash", 1, "");
}

#define ON_PROD   (1u << NN_PROBE_PRODUCER)
#define ON_PANEL  (1u << NN_PROBE_PANEL)
#define ON_SHELL  ((1u << NN_PROBE_CONSOLE) | (1u << NN_PROBE_BG))

static void test_line(void)
{
	struct nn_probe_row r;
	char buf[NN_STREAM_LINE_MAX];
	int n;

	printf("line:\n");
	memset(&r, 0, sizeof r);
	n = nn_probe_line(buf, sizeof buf, "entry", &r, ON_SHELL, NULL);
	expect("[!] nothing observed is still a line, and says so",
	       n > 0 && strcmp(buf, "entry    : con -- bg --; not measured") == 0,
	       "'%s'", buf);

	nn_probe_record(&r, NN_PROBE_CONSOLE, 1840u, 2256u, 4096u);
	nn_probe_record(&r, NN_PROBE_PRODUCER, 1072u, 7120u, 8192u);
	n = nn_probe_line(buf, sizeof buf, "decode", &r, ON_PROD | ON_SHELL,
	                  NULL);
	expect("observed threads as depth/stack, an unobserved one as --, and "
	       "the least left over what was seen",
	       n > 0 && strcmp(buf, "decode   : prod 1072/8192 con 1840/4096 "
	                            "bg --; left 2256") == 0, "'%s'", buf);

	nn_probe_record(&r, NN_PROBE_PANEL, 200u, 1848u, 2048u);
	nn_probe_reject(&r);
	n = nn_probe_line(buf, sizeof buf, "decode", &r, ON_PROD | ON_SHELL,
	                  "(note)");
	expect("[!] a thread the slot is not supposed to run on is SHOWN and "
	       "flagged, then the note, then the invalid count",
	       strcmp(buf, "decode   : prod 1072/8192 !panel 200/2048 con "
	                   "1840/4096 bg --; left 1848 (note) inv 1") == 0,
	       "'%s'", buf);

	/*
	 * [!] THE WIDEST LINES THE BOARD CAN MAKE WITHOUT A BROKEN TABLE.  Every
	 * stack here is at most 8,192 B, so a depth or a stack is at most four
	 * digits -- five only past the bottom of the stack.  The two cases are the
	 * widest slot of each kind: decode on all four threads (one of them the
	 * table's mistake) with a five-digit invalid count, and entry, which is
	 * the slot with a note.
	 */
	memset(&r, 0, sizeof r);
	r.cell[NN_PROBE_PRODUCER] = (struct nn_probe_cell){ 8192u, 9999u,
	                                                     8192u, 1u };
	r.cell[NN_PROBE_PANEL]    = r.cell[NN_PROBE_PRODUCER];
	r.cell[NN_PROBE_CONSOLE]  = r.cell[NN_PROBE_PRODUCER];
	r.cell[NN_PROBE_BG]       = r.cell[NN_PROBE_PRODUCER];
	r.invalid = 99999u;
	n = nn_probe_line(buf, sizeof buf, "shapes_ok", &r, ON_PROD | ON_SHELL,
	                  NULL);
	expect("the widest noteless line -- four threads, four-digit numbers, a "
	       "five-digit invalid count -- fits the caller's 96 B whole",
	       n == (int)strlen(buf) && n < (int)sizeof buf - 1 &&
	       strcmp(buf + n - 10, " inv 99999") == 0,
	       "%zu B: '%s'", strlen(buf), buf);
	r.cell[NN_PROBE_PRODUCER].hits = 0u;
	r.cell[NN_PROBE_PANEL].hits = 0u;
	n = nn_probe_line(buf, sizeof buf, "entry", &r, ON_SHELL,
	                  "(+56 loader)");
	expect("and so does the widest line with a note",
	       n < (int)sizeof buf - 1 &&
	       strcmp(buf + n - 22, "(+56 loader) inv 99999") == 0,
	       "%zu B: '%s'", strlen(buf), buf);

	r.cell[NN_PROBE_PRODUCER] = (struct nn_probe_cell){ 99999u, 99999u,
	                                                     99999u, 1u };
	r.cell[NN_PROBE_PANEL]    = r.cell[NN_PROBE_PRODUCER];
	r.cell[NN_PROBE_CONSOLE]  = r.cell[NN_PROBE_PRODUCER];
	r.cell[NN_PROBE_BG]       = r.cell[NN_PROBE_PRODUCER];
	r.invalid = UINT32_MAX;
	n = nn_probe_line(buf, sizeof buf, "shapes_ok", &r, ON_PROD | ON_SHELL,
	                  "(+56 loader)");
	expect("past that it is cut at the caller's buffer, and terminated",
	       n == (int)sizeof buf - 1 && strlen(buf) == sizeof buf - 1u,
	       "n %d len %zu", n, strlen(buf));

	n = nn_probe_line(buf, 8u, "entry", &r, ON_SHELL, NULL);
	expect("a short buffer is truncated and still terminated",
	       n == 7 && strlen(buf) == 7u, "n %d len %zu", n, strlen(buf));
	buf[0] = 'x';
	n = nn_probe_line(buf, 1u, "entry", &r, ON_SHELL, NULL);
	expect("and a one-byte buffer holds just the terminator",
	       n == 0 && buf[0] == '\0', "n %d", n);
	n = nn_probe_line(buf, sizeof buf, "entry", NULL, ON_SHELL, NULL);
	expect("no record is 'not measured', still a line",
	       n > 0 && strstr(buf, "not measured") != NULL, "'%s'", buf);
}

int main(void)
{
	printf("test_nn_probe\n");
	test_classify();
	test_measure();
	test_record();
	test_line();
	if (failures) {
		printf("test_nn_probe: %d failure(s)\n", failures);
		return 1;
	}
	printf("test_nn_probe: all cases pass\n");
	return 0;
}
