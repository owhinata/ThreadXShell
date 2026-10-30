/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    test_sbrk.c
 * @brief   The heap's bound, by behaviour (issue #159; wio-lite-ai's of the same
 *          name since its issue #108).
 *
 * Since #159 the top 32 KB of internal SRAM is the .plugin reservation, and the
 * heap must stop below its base: src/retarget.c bounds the break at
 * `_estack - _Min_Stack_Size`, and the linker script sets _estack to the
 * reservation's base.  cmake/check_f746_layout.py checks that the LINKED _sbrk
 * names _estack and nothing inside the reservation -- but naming a constant is
 * not using it as the bound: `_estack + _Min_Stack_Size` materialises the same
 * constants and lets the heap run 1 KB into plugin space (the #159 adversarial
 * review reproduced exactly that, gate green).  This checks the other half: the
 * REAL src/retarget.c, compiled here, grows the break to exactly
 * `_estack - _Min_Stack_Size` and not one byte further, with memory laid out
 * ABOVE _estack so that a bound past it links and FAILS rather than failing to
 * link.
 *
 * The linker symbols are renamed on the command line (-Dend=... etc.) to labels
 * this file lays out in one block -- floor, CEILING_GAP bytes to the ceiling,
 * RESERVE bytes to _estack, then more -- and _Min_Stack_Size is, as in the real
 * linker script, an ABSOLUTE symbol whose address is the byte count.
 */
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>

#define CEILING_GAP 4096
#define RESERVE     1024   /* f746's _Min_Stack_Size (0x400) */

__asm__(".data\n"
        ".balign 8\n"
        ".globl f746_test_heap_floor\n"
        "f746_test_heap_floor:\n"
        ".space 4096\n"                 /* CEILING_GAP */
        ".globl f746_test_heap_ceiling\n"
        "f746_test_heap_ceiling:\n"
        ".space 1024\n"                 /* RESERVE */
        ".globl f746_test_estack\n"
        "f746_test_estack:\n"
        ".space 4096\n"                 /* plugin space above _estack */
        ".globl f746_test_min_stack\n"
        ".set f746_test_min_stack, 1024\n"
        ".text\n");

extern char f746_test_heap_floor, f746_test_heap_ceiling, f746_test_estack;
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
	char *floor = &f746_test_heap_floor;
	char *prev;

	printf("test_sbrk (src/retarget.c):\n");
	/* (_Min_Stack_Size's value is not compared here: the compiler may fold
	 * "the address of an object is 1024" to false.  The two refusals below
	 * are what show retarget.c reads it as 1024.) */
	CHECK(&f746_test_heap_ceiling - floor == CEILING_GAP &&
	      &f746_test_estack - &f746_test_heap_ceiling == RESERVE,
	      "the harness laid out floor < ceiling = _estack - _Min_Stack_Size");

	prev = _sbrk(0);
	CHECK(prev == floor, "the break starts at `end`");

	prev = _sbrk(CEILING_GAP);
	CHECK(prev == floor, "growing to exactly _estack - _Min_Stack_Size succeeds");
	CHECK(_sbrk(0) == &f746_test_heap_ceiling, "the break is now the ceiling");

	errno = 0;
	prev = _sbrk(1);
	CHECK(prev == (void *)-1 && errno == ENOMEM,
	      "one byte past the ceiling is refused -- the main-stack reserve "
	      "and the plugin space above _estack stay out of the heap");
	CHECK(_sbrk(0) == &f746_test_heap_ceiling,
	      "a refusal does not move the break");

	errno = 0;
	CHECK(_sbrk(RESERVE + 1) == (void *)-1 && errno == ENOMEM,
	      "reaching past _estack is refused");

	errno = 0;
	CHECK(_sbrk(INT_MAX) == (void *)-1 && errno == ENOMEM,
	      "INT_MAX is refused without wrapping");

	prev = _sbrk(-CEILING_GAP);
	CHECK(prev == &f746_test_heap_ceiling && _sbrk(0) == floor,
	      "shrinking returns to `end`");
	errno = 0;
	CHECK(_sbrk(-1) == (void *)-1 && errno == ENOMEM, "below `end` is refused");
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
