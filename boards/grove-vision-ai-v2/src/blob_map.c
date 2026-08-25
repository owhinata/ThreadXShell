/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    blob_map.c
 * @brief   The asset slot table and the rules it has to satisfy (#92).
 *
 * No hardware, no ThreadX, no logging -- test/test_blob_map.c compiles this
 * exact file on the host.  See blob_map.h for why the interval is an argument
 * and never a constant in this file.
 */
#include "blob_map.h"

#include <stddef.h>

/* Size classes, largest first, ending at 0xB00000.  The bases are spelled out
 * rather than computed: a loop that generated them would be a second statement
 * of the same layout, and the one thing worth checking about this table --
 * that it is ascending, non-overlapping and inside the interval -- is checked
 * below against the seam's numbers rather than against how it was written.
 *
 * test/test_blob_map.c pins these ten entries as a golden prefix, so that #49
 * Step 4 (which appends, once the model partitions are gone) cannot reorder
 * them: an index that moved would point every `blob list` line at different
 * flash. */
static const struct blob_slot map[] = {
	{ 0x00200000u, 0x00200000u },   /* 2 MB   */
	{ 0x00400000u, 0x00200000u },
	{ 0x00600000u, 0x00100000u },   /* 1 MB   */
	{ 0x00700000u, 0x00100000u },
	{ 0x00800000u, 0x00100000u },
	{ 0x00900000u, 0x00080000u },   /* 512 KB */
	{ 0x00980000u, 0x00080000u },
	{ 0x00A00000u, 0x00080000u },
	{ 0x00A80000u, 0x00040000u },   /* 256 KB */
	{ 0x00AC0000u, 0x00040000u },
};

#define MAP_COUNT   ((unsigned)(sizeof map / sizeof map[0]))

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

	/* Ascending is a separate fact from disjoint, and it is the one #49
	 * Step 4 depends on: it appends slots to this table, and an append is
	 * only an append if what is there is in order. */
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

const struct blob_slot *blob_map_table(void)
{
	return map;
}

unsigned blob_map_count(void)
{
	return MAP_COUNT;
}

const struct blob_slot *blob_map_slot(unsigned i)
{
	if (i >= MAP_COUNT)
		return NULL;
	return &map[i];
}

int blob_map_index_of(uint32_t base)
{
	unsigned i;

	for (i = 0u; i < MAP_COUNT; i++) {
		if (map[i].base == base)
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
