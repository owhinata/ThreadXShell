/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    nor_span.c
 * @brief   The write request arithmetic, as pure functions (issue #88).
 *
 * No hardware, no ThreadX, no logging -- so test/test_nor_write.c compiles this
 * exact file on the host.  See nor_span.h for why the order of the tests is the
 * subject rather than an implementation detail.
 */
#include "nor_span.h"

#include <stddef.h>

const char *nor_span_verdict_name(enum nor_span_verdict v)
{
	switch (v) {
	case NOR_SPAN_OK:           return "ok";
	case NOR_SPAN_EMPTY:        return "zero length";
	case NOR_SPAN_OUTSIDE:      return "outside the writable interval";
	case NOR_SPAN_BAD_UNIT:     return "erase unit is not a power of two";
	case NOR_SPAN_BAD_INTERVAL: return "the writable interval is not usable";
	case NOR_SPAN_BAD_ALIGN:    return "program address is not word aligned";
	default:                    break;
	}
	return "?";
}

enum nor_span_verdict nor_span_program(uint32_t lo, uint32_t hi, uint32_t addr,
                                       uint32_t len, struct nor_span *out)
{
	if (out == NULL)
		return NOR_SPAN_BAD_INTERVAL;
	if (lo >= hi)
		return NOR_SPAN_BAD_INTERVAL;
	if (len == 0u)
		return NOR_SPAN_EMPTY;
	/* addr == hi is allowed this far and is then refused by the length test,
	 * because len is never zero here: an empty range at the exclusive end has
	 * no bytes to be inside or outside of. */
	if (addr < lo || addr > hi)
		return NOR_SPAN_OUTSIDE;
	if (len > hi - addr)
		return NOR_SPAN_OUTSIDE;
	/* After the bounds, deliberately: an address outside the interval is the
	 * more serious fact and should be the one reported.  This one is about
	 * whether the bytes would land in the right ORDER (issue #92). */
	if ((addr % 4u) != 0u)
		return NOR_SPAN_BAD_ALIGN;

	out->addr = addr;
	out->len  = len;
	return NOR_SPAN_OK;
}

enum nor_span_verdict nor_span_erase(uint32_t lo, uint32_t hi, uint32_t unit,
                                     uint32_t addr, uint32_t len,
                                     struct nor_span *out)
{
	uint32_t first, end, tail;

	if (out == NULL)
		return NOR_SPAN_BAD_INTERVAL;
	if (unit == 0u || (unit & (unit - 1u)) != 0u)
		return NOR_SPAN_BAD_UNIT;
	/* An interval whose edges are not whole units cannot bound a rounded
	 * footprint: an erase at its first address would destroy bytes below it. */
	if (lo >= hi || (lo % unit) != 0u || (hi % unit) != 0u)
		return NOR_SPAN_BAD_INTERVAL;
	if (len == 0u)
		return NOR_SPAN_EMPTY;
	if (addr < lo || addr >= hi)
		return NOR_SPAN_OUTSIDE;
	if (len > hi - addr)
		return NOR_SPAN_OUTSIDE;

	/* Only now: `addr + len` has been proved not to exceed hi, and hi is at
	 * most the part's size, so neither this sum nor the rounding can wrap. */
	first = addr - (addr % unit);
	end   = addr + len;
	tail  = end % unit;
	if (tail != 0u)
		end += unit - tail;

	/* Cannot fire while the interval's edges are whole units -- which the test
	 * above establishes.  Kept because containment should be READ off this
	 * function rather than re-derived from that alignment argument. */
	if (first < lo || end > hi)
		return NOR_SPAN_OUTSIDE;

	out->addr = first;
	out->len  = end - first;
	return NOR_SPAN_OK;
}

uint32_t nor_span_page_chunk(uint32_t addr, uint32_t remaining, uint32_t cap)
{
	uint32_t to_page = NOR_PROGRAM_PAGE - (addr % NOR_PROGRAM_PAGE);
	uint32_t n = remaining;

	if (cap == 0u)
		return 0u;
	if (n > to_page)
		n = to_page;
	if (n > cap)
		n = cap;
	return n;
}
