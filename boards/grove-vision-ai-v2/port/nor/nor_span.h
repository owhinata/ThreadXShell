/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    nor_span.h
 * @brief   What a write request destroys, and how it is split (issue #88).
 *
 * WHY THIS IS ITS OWN FILE, like nor_state.h and cam_state.h before it: these
 * are the parts of the writer a board cannot be made to demonstrate.  Every
 * caller is first-party code written to satisfy them, and each case that
 * matters -- a length that would run past the writable interval, an address
 * whose erase unit rounds outside it, a request near 2^32 -- would cost a flash
 * cycle of a part whose endurance is not documented (issue #89) to try on
 * hardware, and most of them cannot be produced at all.  test/test_nor_write.c
 * walks them here instead.
 *
 * THE ONE THING THIS FILE IS REALLY ABOUT: an erase destroys a UNIT, not a
 * byte.  A caller naming [0x200800, 0x200810) loses the whole 4 KB sector that
 * range sits in, and the interval check has to be applied to THAT footprint
 * rather than to what was asked for.  Rounding after checking would check the
 * wrong span; rounding before checking would round a value that has not been
 * proved not to overflow.  So the order below is fixed, and it is the order the
 * host test walks.
 *
 * [!] THE ARITHMETIC IS SUBTRACTION-BASED, and `addr + len` is not formed until
 * after `len <= hi - addr` has been established.  With hi <= 16 MB that proof
 * is also what makes the rounding safe: nothing here can wrap.
 *
 * These functions know nothing about the lifecycle.  Whether a write may run at
 * all is nor_state.h's question, and the seam asks it again at the door
 * (nor_seam.h) -- this file only answers "which bytes".
 */
#ifndef NOR_SPAN_H
#define NOR_SPAN_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * The part's program page.
 *
 * [!] MEASURED, not chosen: hx_lib_qspi_eeprom_write splits its payload at
 * `256 - (addr % 256)` and issues one page-program per piece (issue #88, by
 * disassembly).  The writer splits on the same boundary rather than handing the
 * vendor one long buffer, so that the piece the vendor is copying out of is
 * one this port owns -- see nor_write.h for why that matters.
 */
#define NOR_PROGRAM_PAGE   256u

/** A run of flash: a byte offset and a length. */
struct nor_span {
	uint32_t addr;
	uint32_t len;
};

/** Why a request does or does not name a span this port may act on. */
enum nor_span_verdict {
	NOR_SPAN_OK = 0,
	NOR_SPAN_EMPTY,        /**< zero length names no bytes              */
	NOR_SPAN_OUTSIDE,      /**< the footprint is not wholly inside      */
	NOR_SPAN_BAD_UNIT,     /**< erase unit zero or not a power of two   */
	NOR_SPAN_BAD_INTERVAL, /**< the writable interval is not usable     */
};

/** Short name, for the refusal message and the host test's diagnostics. */
const char *nor_span_verdict_name(enum nor_span_verdict v);

/**
 * @brief  The span a program of [addr, addr+len) writes.
 *
 * @param lo    first writable flash offset
 * @param hi    one past the last; exclusive
 * @param addr  flash offset to program
 * @param len   payload length in bytes
 * @param out   receives the span; untouched unless NOR_SPAN_OK
 *
 * A program touches exactly what it names -- no rounding -- so this is the
 * bounds check and nothing else.  It is deliberately the same shape as
 * nor_seam_check_write(): the writer asks here before it claims the part, the
 * seam asks again at the door, and the two must not disagree about an edge.
 */
enum nor_span_verdict nor_span_program(uint32_t lo, uint32_t hi, uint32_t addr,
                                       uint32_t len, struct nor_span *out);

/**
 * @brief  The span an erase of [addr, addr+len) destroys.
 *
 * @param unit  bytes one permitted erase destroys; a power of two
 *
 * The footprint is [addr rounded DOWN to a unit, addr+len rounded UP), which is
 * what the resident 2nd bootloader's range eraser also destroys (issue #88).
 * It is the ROUNDED span that must lie inside [lo, hi), and @p out carries it
 * so the caller can say what it is about to lose rather than what it asked for.
 *
 * [!] The rounded edges cannot escape an interval whose own edges are unit
 * aligned, and NOR_SPAN_BAD_INTERVAL is what establishes that.  They are
 * checked anyway: the cost is two comparisons, and the alternative is a reader
 * who has to re-derive the alignment argument to believe the containment.
 */
enum nor_span_verdict nor_span_erase(uint32_t lo, uint32_t hi, uint32_t unit,
                                     uint32_t addr, uint32_t len,
                                     struct nor_span *out);

/**
 * @brief  How much of a program may go in the next vendor call.
 *
 * @param addr       where the next piece starts
 * @param remaining  bytes still to send
 * @param cap        the writer's own buffer size; must not be zero
 * @return the piece length, or 0 when there is nothing more to send
 *
 * The piece never crosses a NOR_PROGRAM_PAGE boundary and never exceeds @p cap.
 *
 * [!] A ZERO RETURN IS TERMINAL FOR THE CALLER, not a signal to try again.  It
 * means either that nothing is left or that the cap was zero, and a loop that
 * treated it as "skip this one" would not advance.  The writer stops on it.
 */
uint32_t nor_span_page_chunk(uint32_t addr, uint32_t remaining, uint32_t cap);

#ifdef __cplusplus
}
#endif

#endif /* NOR_SPAN_H */
