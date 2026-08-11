/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    retarget.c
 * @brief   Bounded newlib heap (_sbrk) for the STM32F746G-DISCO shell firmware.
 *
 * _write (stdout/stderr -> the USART1 TX ring) lives in the UART backend
 * (backend/cli_backend_uart.c) so it shares the shell's single TX owner, with a
 * weak polling fallback in src/bsp.c for the earliest boot printf.  This file
 * provides only the heap.
 *
 * It exists because -specs=nosys.specs would otherwise supply libnosys' _sbrk,
 * which has NO upper bound at all: it hands out whatever is asked for and lets
 * the heap walk straight through the top of SRAM.  On this part the addresses
 * past _estack are simply not backed, so the failure is a BusFault at some later
 * unrelated access, or -- worse -- a silent overlap with the main stack that
 * corrupts an interrupt frame.  The shared shell core is required to run without
 * a heap; a board may offer one only if it is bounded, serialised and reports
 * failure (malloc_lock.c is the serialisation half).
 *
 * The bounds:
 *   lower  &end                      the linker's start of the heap region.  A
 *                                    negative incr may return the break to it
 *                                    but never below -- newlib does shrink the
 *                                    break on some free() paths.
 *   upper  &_estack - &_Min_Stack_Size
 *                                    the top of SRAM less the main-stack
 *                                    reservation.  The MSP starts at _estack and
 *                                    grows down, so this keeps the two apart even
 *                                    though nothing at run time would object.
 *
 * All comparisons are done on uintptr_t in a form that cannot overflow or
 * underflow: the magnitude of a negative incr is computed by unsigned negation
 * (INT_MIN has no positive counterpart), and each bound is checked as a
 * difference against the side that is known to be larger.  Integer math also
 * avoids the -Warray-bounds that a bare pointer comparison against a linker
 * symbol triggers.
 */
#include <errno.h>
#include <stdint.h>

extern char end;               /* heap start (linker: PROVIDE(end = .))        */
extern char _estack;           /* top of SRAM = initial MSP                    */
extern char _Min_Stack_Size;   /* bytes reserved below _estack for the MSP     */

void *_sbrk(int incr)
{
	static uintptr_t heap;

	const uintptr_t base  = (uintptr_t)&end;
	const uintptr_t limit = (uintptr_t)&_estack - (uintptr_t)&_Min_Stack_Size;
	uintptr_t prev;

	if (heap == 0u)
		heap = base;

	/* Defensive: the linker script reserves _Min_Heap_Size + _Min_Stack_Size
	 * inside RAM, so a link that got here has base <= limit.  Checking anyway
	 * keeps the unsigned differences below meaningful if that ever changes. */
	if (heap > limit) {
		errno = ENOMEM;
		return (void *)-1;
	}

	prev = heap;

	if (incr >= 0) {
		uintptr_t add = (uintptr_t)incr;

		if (add > limit - heap) {          /* limit - heap cannot underflow */
			errno = ENOMEM;
			return (void *)-1;
		}
		heap += add;
	} else {
		/* Unsigned negation: |incr| for every negative incr, INT_MIN included. */
		uintptr_t sub = (uintptr_t)0 - (uintptr_t)incr;

		if (sub > heap - base) {           /* heap >= base is an invariant */
			errno = ENOMEM;
			return (void *)-1;
		}
		heap -= sub;
	}

	return (void *)prev;
}
