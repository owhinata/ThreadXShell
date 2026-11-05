/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    blob_map.c
 * @brief   The rules an asset slot table has to satisfy, and the arithmetic
 *          over one (#92; shared since #192).
 *
 * No hardware, no ThreadX, no logging -- shell/test/test_blob_map.c compiles
 * this exact file on the host.  See blob_map.h for why the interval is an
 * argument and never a constant in this file.
 *
 * [!] THE TABLE IS NOT HERE.  Which slots a board carves is that board's fact
 * (grove-vision-ai-v2: src/blob_slots.c), and it hands the table over under the
 * fixed names blob_map.h declares rather than as an argument -- so the slots
 * this file looks up are the slots that board ships, and there is no second
 * table a caller could pass instead.
 */
#include "blob_map.h"

#include <stddef.h>

const char *blob_map_verdict_name(enum blob_map_verdict v)
{
	switch (v) {
	case BLOB_MAP_OK:           return "ok";
	case BLOB_MAP_NO_SLOTS:     return "no slots";
	case BLOB_MAP_BAD_INTERVAL: return "the writable interval is not usable";
	case BLOB_MAP_UNALIGNED:    return "slot is not whole erase units";
	case BLOB_MAP_TOO_SMALL:    return "slot has no payload";
	case BLOB_MAP_OUTSIDE:      return "slot is outside the writable interval";
	case BLOB_MAP_ORDER:        return "slots are not in ascending order";
	case BLOB_MAP_OVERLAP:      return "slots overlap";
	default:                    break;
	}
	return "?";
}

int blob_map_verdict_names_slot(enum blob_map_verdict v)
{
	switch (v) {
	case BLOB_MAP_UNALIGNED:
	case BLOB_MAP_TOO_SMALL:
	case BLOB_MAP_OUTSIDE:
	case BLOB_MAP_ORDER:
	case BLOB_MAP_OVERLAP:
		return 1;
	case BLOB_MAP_OK:
	case BLOB_MAP_NO_SLOTS:
	case BLOB_MAP_BAD_INTERVAL:
	default:
		break;
	}
	return 0;
}

enum blob_map_verdict blob_map_check(uint32_t lo, uint32_t hi, uint32_t unit,
                                     const struct blob_slot *slots,
                                     unsigned count, unsigned *bad)
{
	unsigned i, j;

	if (bad != NULL)
		*bad = 0u;
	if (unit == 0u || (unit & (unit - 1u)) != 0u)
		return BLOB_MAP_BAD_INTERVAL;
	/* The same test nor_span_erase() makes: an interval whose own edges are
	 * not whole units cannot bound a footprint that gets rounded out to
	 * them, so a table inside it would not be safe to erase either. */
	if (lo >= hi || (lo % unit) != 0u || (hi % unit) != 0u)
		return BLOB_MAP_BAD_INTERVAL;
	if (slots == NULL || count == 0u)
		return BLOB_MAP_NO_SLOTS;

	for (i = 0u; i < count; i++) {
		uint32_t base = slots[i].base;
		uint32_t size = slots[i].size;

		if (bad != NULL)
			*bad = i;
		if ((base % unit) != 0u || (size % unit) != 0u)
			return BLOB_MAP_UNALIGNED;
		/* One unit of header and at least one of payload. */
		if (size < 2u * unit)
			return BLOB_MAP_TOO_SMALL;
		if (base < lo || base >= hi)
			return BLOB_MAP_OUTSIDE;
		/* Only now is base + size safe to think about: hi is the part's
		 * size at most, so a size that passes this cannot wrap. */
		if (size > hi - base)
			return BLOB_MAP_OUTSIDE;
	}

	/* All pairs, and BEFORE the ordering check rather than after it, so that
	 * disjointness is decided without resting on the table being ascending.
	 * Checked afterwards it would be dead: two slots that overlap while out
	 * of order would already have been reported as an ordering fault, and
	 * nothing would ever exercise this loop's answer.  It is subtraction
	 * only, so it holds on a table that has not been sanity-checked at all. */
	for (i = 0u; i < count; i++) {
		for (j = i + 1u; j < count; j++) {
			uint32_t a = slots[i].base, an = slots[i].size;
			uint32_t b = slots[j].base, bn = slots[j].size;

			if (a < b ? (b - a) < an : (a - b) < bn) {
				if (bad != NULL)
					*bad = j;
				return BLOB_MAP_OVERLAP;
			}
		}
	}

	/* Ascending is a separate fact from disjoint, and it is the one an
	 * append depends on: appending to this table is only an append if what
	 * is already there is in order. */
	for (i = 1u; i < count; i++) {
		if (slots[i].base <= slots[i - 1u].base) {
			if (bad != NULL)
				*bad = i;
			return BLOB_MAP_ORDER;
		}
	}

	if (bad != NULL)
		*bad = 0u;
	return BLOB_MAP_OK;
}

const struct blob_slot *blob_map_slot(unsigned i)
{
	if (i >= blob_map_slots_count)
		return NULL;
	return &blob_map_slots[i];
}

int blob_map_index_of(uint32_t base)
{
	unsigned i;

	for (i = 0u; i < blob_map_slots_count; i++) {
		if (blob_map_slots[i].base == base)
			return (int)i;
	}
	return -1;
}

uint32_t blob_map_payload_addr(const struct blob_slot *s, uint32_t unit)
{
	return s->base + unit;
}

uint32_t blob_map_payload_max(const struct blob_slot *s, uint32_t unit)
{
	if (s->size <= unit)
		return 0u;
	return s->size - unit;
}

uint32_t blob_map_uncarved(uint32_t lo, uint32_t hi,
                           const struct blob_slot *slots, unsigned count)
{
	uint32_t span, carved = 0u;
	unsigned i;

	if (lo >= hi || slots == NULL)
		return 0u;
	span = hi - lo;
	for (i = 0u; i < count; i++) {
		/* Saturate rather than wrap: on a checked table the sum cannot
		 * exceed the span, and on an unchecked one "nothing spare" is
		 * the answer that cannot be mistaken for room. */
		if (slots[i].size > span - carved)
			return 0u;
		carved += slots[i].size;
	}
	return span - carved;
}
