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
 *   upper  &__heap_end the base of the .plugin reservation (issue #108), which
 *                     sits at the top of AXI-SRAM.  This used to be _estack, back
 *                     when the main stack sat at the top of AXI-SRAM and growing
 *                     the heap into it was the hazard worth 4 KB of clearance.
 *                     Issue #46 moved the main stack to the top of DTCM, so
 *                     _estack became 0x20020000 -- BELOW the heap -- and the
 *                     ceiling became __ram_end, the end of AXI-SRAM.  Issue #108
 *                     then pinned the plugin reservation at that end, and a heap
 *                     still bounded by __ram_end could have grown straight into a
 *                     plugin's code.  __heap_end is the linker symbol that means
 *                     "how far the heap may grow"; __ram_end still means "where
 *                     AXI-SRAM ends", and the two are different facts again.
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
extern char __heap_end;          /* base of .plugin (NOT __ram_end; see above) */

void *_sbrk(int incr)
{
  static uintptr_t heap;

  const uintptr_t base  = (uintptr_t)&end;
  const uintptr_t limit = (uintptr_t)&__heap_end;
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
