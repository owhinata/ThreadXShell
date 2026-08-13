/*
 * newlib heap retargeting for the Wio Lite AI ThreadX shell app.
 *
 * _write (stdout/stderr -> the CDC TX ring) lives in the USB CDC backend
 * (shell/backend/cli_backend_usbcdc.c) so it shares the shell's single TX owner.
 * This file provides only the heap (_sbrk), bounded so a stray allocation cannot
 * walk off the end of AXI-SRAM.  The shared shell core is required to run without
 * a heap; a board may offer one only if it is bounded, serialised and reports
 * failure (malloc_lock.c is the serialisation half).
 *
 * The bounds (issue #8 brings these in line with the f746g-disco port):
 *   lower  &end       the linker's start of the heap region.  A negative incr may
 *                     return the break to it but never below -- newlib does shrink
 *                     the break on some free() paths.
 *   upper  &__ram_end the end of AXI-SRAM.  This used to be _estack, back when the
 *                     main stack sat at the top of AXI-SRAM and growing the heap
 *                     into it was the hazard worth 4 KB of clearance.  Issue #46
 *                     moved the main stack to the top of DTCM, so _estack is now
 *                     0x20020000 -- BELOW the heap.  Keeping it would have made
 *                     every comparison below true and turned _sbrk into an
 *                     unconditional ENOMEM: malloc failing everywhere, in a
 *                     firmware that links without a warning.  __ram_end is the
 *                     linker symbol that means what this needs; nothing lives
 *                     above the heap in AXI-SRAM, so the rest of the region is
 *                     available (see the linker script).
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

extern char end;                 /* heap start (from the linker script) */
extern char __ram_end;           /* end of AXI-SRAM (NOT _estack; see above)  */

void *_sbrk(int incr)
{
  static uintptr_t heap;

  const uintptr_t base  = (uintptr_t)&end;
  const uintptr_t limit = (uintptr_t)&__ram_end;
  uintptr_t prev;

  if (heap == 0u) heap = base;

  /* Defensive: the linker script places the heap inside RAM, so a link that got
   * here has base <= limit.  Checking anyway keeps the unsigned differences below
   * meaningful if that ever changes. */
  if (heap > limit) {
    errno = ENOMEM;
    return (void *) -1;
  }

  prev = heap;

  if (incr >= 0) {
    uintptr_t add = (uintptr_t)incr;

    if (add > limit - heap) {          /* limit - heap cannot underflow */
      errno = ENOMEM;
      return (void *) -1;
    }
    heap += add;
  } else {
    /* Unsigned negation: |incr| for every negative incr, INT_MIN included. */
    uintptr_t sub = (uintptr_t)0 - (uintptr_t)incr;

    if (sub > heap - base) {           /* heap >= base is an invariant */
      errno = ENOMEM;
      return (void *) -1;
    }
    heap -= sub;
  }

  return (void *)prev;
}
