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

/* [!] THIS TABLE WAS RE-CARVED WHOLE AT ISSUE #94, AND THAT IS NOT THE RULE.
 * Identity is the base address, never the index, so the shape of change this
 * table is normally allowed is APPEND: anything else points a stored blob's
 * header at a slot that no longer starts where it does, and the blob becomes
 * unreachable.  #92 shipped ten slots and pinned them as a golden prefix for
 * exactly that reason.
 *
 * The prefix was spent here, once, deliberately.  Folding the model partitions
 * into blob (#49 Step 4b) freed 0xB00000..0xFFE000, and appending to the old
 * table would have left the classes interleaved by address -- 2 MB, 1 MB,
 * 512 KB, 256 KB, then a 4 MB one bolted on at the top -- with the largest
 * class in the last place a reader looks.  Re-carving largest-first cost the
 * re-sending of two files, because at that moment the store held `cls` (which
 * survives: same base 0x200000, and a header only has to agree with its own
 * slot's base and fit its payload_max), `det` and one test file.
 *
 * [!] THE PRICE GOES UP FROM HERE.  There is no second cheap moment: the next
 * re-carve pays whatever is stored then.  Append.
 *
 * The bases are spelled out rather than computed: a loop that generated them
 * would be a second statement of the same layout, and the one thing worth
 * checking about this table -- that it is ascending, non-overlapping and inside
 * the interval -- is checked below against the seam's numbers rather than
 * against how it was written.
 *
 * [!] THE 4 MB CLASS IS SIZED BY WHAT THE HARDWARE CAN RUN, not by anything
 * stored here (the largest asset is a 1,704,672 B model).  Nothing but the SLOT
 * bounds how big a model this board can run: the flatbuffer is read in place
 * out of the XIP window and never copied, npu_open()'s length limit is whatever
 * is left of the window, and the arena is a function of feature-map sizes
 * rather than of weights.  The two models prove that last part -- the
 * 164,512 B detector uses a BIGGER arena (394,800 B) than the 1,704,672 B
 * classifier (385,748 B).  So a model over 2,093,056 B is something this board
 * could run and a 2 MB store could not hold.
 *
 * [!] SEVERAL SLOTS START OVER FLASH THAT IS NOT BLANK, AND THEY DO NOT ALL SAY
 * SO.  Nothing erased the old fixed model copies -- MobileNet
 * 0xB7B000..0xD1B2E0, BlazeFace 0xD20000..0xD482A0 and issue #88's test residue
 * above them, one contiguous 2,031,616 B run measured by `nor scan` -- nor the
 * factory data lower down, nor the assets this re-carve stranded.  What each
 * slot reports depends only on its own HEADER SECTOR:
 *
 *   0xC00000 and 0xD00000 fall inside the old model run, so their magic pages
 *       hold model bytes and they report `invalid` (BLOB_REJECT_MAGIC).
 *       `blob write` refuses them until somebody types `blob erase <slot>`.
 *   0x800000 and 0xA00000 were never written, so they report `empty` -- while
 *       the stranded `test-small` and `det` sit in their payloads.
 *
 * That second line is the distinction issue #92 named, doing real work:
 * `empty` means "no header", NEVER "blank flash".  `blob write` takes such a
 * slot as fresh and erases all of it.  Which is fine, since nothing can read
 * those bytes any more -- and `blob write` announces what it is about to
 * destroy once a slot is chosen -- but the store did not notice and had
 * nothing to warn about.  Reservation edges do not tell you which sectors hold
 * bytes either: `model-cls` used to end at 0xD20000, a further 128 KB past
 * where the model itself stops.  Only the scan does.
 *
 * [!] AND 0xF00000..0xFFE000 -- 1,040,384 B -- IS DELIBERATELY NOT CARVED.  It
 * is not too small; an earlier draft put a 512 KB and a 256 KB slot there.  It
 * is uncarved because nothing needs those classes while three 1 MB and two
 * 512 KB slots are free, and every carved slot costs a blob_stat() in each
 * `nn open <name>` and a line in each `blob list`.  Left whole it can become
 * whatever class a real demand asks for -- and appending is the safe direction
 * -- while nothing can be stored there, because no slot names those bytes.
 * blob_map_uncarved() reports it and `blob free` prints it apart from the free
 * slots for that reason.  (A 1 MB slot would not fit regardless: 1,048,576.) */
static const struct blob_slot map[] = {
	{ 0x00200000u, 0x00400000u },   /* 4 MB   */
	{ 0x00600000u, 0x00200000u },   /* 2 MB   */
	{ 0x00800000u, 0x00200000u },
	{ 0x00A00000u, 0x00100000u },   /* 1 MB   */
	{ 0x00B00000u, 0x00100000u },
	{ 0x00C00000u, 0x00100000u },
	{ 0x00D00000u, 0x00080000u },   /* 512 KB */
	{ 0x00D80000u, 0x00080000u },
	{ 0x00E00000u, 0x00080000u },
	{ 0x00E80000u, 0x00040000u },   /* 256 KB */
	{ 0x00EC0000u, 0x00040000u },
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
