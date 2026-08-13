/*
 * newlib heap retargeting for the Grove Vision AI V2 ThreadX shell app.
 *
 * _write (stdout/stderr -> the UART TX ring) lives in the UART backend
 * (backend/cli_backend_uart.c) so it shares the shell's single TX owner.  This
 * file provides only the heap (_sbrk), bounded to the linker's heap region so
 * a stray allocation cannot walk into the MSP stack above it.  The shared
 * shell core runs without a heap; newlib itself is the only client here
 * (printf working buffers etc.).
 *
 * Bounds are the ldscript's .heap region symbols:
 *   lower  __HeapBase    start of the reserved heap span in DTCM
 *   upper  __HeapLimit   end of that span (the MSP stack sits far above, at
 *                        the top of DTCM, with .noinit between)
 *
 * All comparisons are done on uintptr_t in a form that cannot overflow or
 * underflow (same discipline as the other boards' retarget.c).
 */
#include <errno.h>
#include <stdint.h>

extern char __HeapBase;          /* from the linker script */
extern char __HeapLimit;

void *_sbrk(int incr)
{
	static uintptr_t heap;

	const uintptr_t base  = (uintptr_t)&__HeapBase;
	const uintptr_t limit = (uintptr_t)&__HeapLimit;
	uintptr_t prev;

	if (heap == 0u)
		heap = base;

	if (heap > limit) {
		errno = ENOMEM;
		return (void *)-1;
	}

	prev = heap;

	if (incr >= 0) {
		uintptr_t inc = (uintptr_t)incr;
		if (limit - heap < inc) {
			errno = ENOMEM;
			return (void *)-1;
		}
		heap += inc;
	} else {
		uintptr_t dec = (uintptr_t)-(uintptr_t)incr;
		if (heap - base < dec) {
			errno = ENOMEM;
			return (void *)-1;
		}
		heap -= dec;
	}

	return (void *)prev;
}
