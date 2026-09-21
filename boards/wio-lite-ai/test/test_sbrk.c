/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    test_sbrk.c
 * @brief   The heap's bound, by behaviour (issue #108).
 *
 * Since #108 the top 32 KB of AXI-SRAM is the .plugin reservation, and the heap
 * must stop at its base (__heap_end), not at the end of AXI-SRAM (__ram_end).
 * cmake/check_plugin_reservation.py checks the LINKED _sbrk names the ceiling
 * and no address above it -- but naming a constant is not using it as the
 * bound (the #108 adversarial review: a bound fetched through a helper, or
 * arithmetic on the ceiling, would pass that).  This checks the other half: the
 * REAL src/retarget.c, compiled here, grows the break to exactly the ceiling and
 * not one byte further, with an __ram_end placed above it so that a regression
 * to the old bound links and FAILS rather than failing to link.
 *
 * The three linker symbols are renamed on the command line (-Dend=... etc.) to
 * labels this file lays out in one block, so their ORDER and DISTANCES are
 * fixed: floor, then CEILING_GAP bytes to the ceiling, then more to __ram_end.
 */
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>

#define CEILING_GAP  4096
#define RAM_END_GAP  8192

__asm__(".data\n"
        ".balign 8\n"
        ".globl wio_test_heap_floor\n"
        "wio_test_heap_floor:\n"
        ".space 4096\n"                 /* CEILING_GAP */
        ".globl wio_test_heap_ceiling\n"
        "wio_test_heap_ceiling:\n"
        ".space 4096\n"                 /* RAM_END_GAP - CEILING_GAP */
        ".globl wio_test_ram_end\n"
        "wio_test_ram_end:\n"
        ".space 8\n"
        ".text\n");

extern char wio_test_heap_floor, wio_test_heap_ceiling, wio_test_ram_end;
void *_sbrk(int incr);

static int failures;

#define CHECK(cond, what)                                                     \
	do {                                                                  \
		if (!(cond)) {                                                \
			printf("  FAIL %s\n", what);                          \
			failures++;                                           \
		} else {                                                      \
			printf("  ok   %s\n", what);                          \
		}                                                             \
	} while (0)

int main(void)
{
	char *floor = &wio_test_heap_floor;
	char *prev;

	printf("test_sbrk (src/retarget.c):\n");
	CHECK(&wio_test_heap_ceiling - floor == CEILING_GAP &&
	      &wio_test_ram_end - floor == RAM_END_GAP,
	      "the harness laid out floor < ceiling < __ram_end");

	prev = _sbrk(0);
	CHECK(prev == floor, "the break starts at `end`");

	prev = _sbrk(CEILING_GAP);
	CHECK(prev == floor, "growing to exactly the ceiling succeeds");
	CHECK(_sbrk(0) == &wio_test_heap_ceiling, "the break is now the ceiling");

	errno = 0;
	prev = _sbrk(1);
	CHECK(prev == (void *)-1 && errno == ENOMEM,
	      "one byte past the ceiling is refused -- not bounded by __ram_end");
	CHECK(_sbrk(0) == &wio_test_heap_ceiling, "a refusal does not move the break");

	errno = 0;
	CHECK(_sbrk(INT_MAX) == (void *)-1 && errno == ENOMEM,
	      "INT_MAX is refused without wrapping");

	prev = _sbrk(-CEILING_GAP);
	CHECK(prev == &wio_test_heap_ceiling && _sbrk(0) == floor,
	      "shrinking returns to `end`");
	errno = 0;
	CHECK(_sbrk(-1) == (void *)-1 && errno == ENOMEM,
	      "below `end` is refused");
	errno = 0;
	CHECK(_sbrk(INT_MIN) == (void *)-1 && errno == ENOMEM,
	      "INT_MIN is refused (no positive counterpart)");

	if (failures) {
		printf("test_sbrk: %d failure(s)\n", failures);
		return 1;
	}
	printf("test_sbrk: all passed\n");
	return 0;
}
