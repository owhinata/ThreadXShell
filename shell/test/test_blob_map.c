/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Host test for the rules an asset slot table has to satisfy and the lookups
 * over one (#92, svc/blob/blob_map.c; shared since #192).
 *
 * WHY THIS EXISTS.  A firmware ships exactly one table, so every table that
 * breaks a rule -- a slot half a unit off the erase grid, one that runs past
 * the writable interval, two that claim the same flash -- exists here or
 * nowhere.  On a board the consequence would not be a refusal either: the
 * seam bounds the WRITER, not the table, so a slot that overlapped another
 * would erase a neighbour's payload without anything out of the ordinary
 * happening.
 *
 * The shipped table is not here.  It is a board's fact, and the board pins it
 * entry by entry in its own test (grove-vision-ai-v2: test/test_blob_map.c).
 * This file defines a small table of its own under the fixed names
 * svc/blob/blob_map.h declares, which is what the shared lookups
 * (blob_map_slot(), blob_map_index_of()) read.
 */
#include <stdint.h>
#include <stdio.h>
#include <stddef.h>

#include "blob_map.h"

static int failures;

#define CHECK(cond, ...)                                                       \
	do {                                                                   \
		if (!(cond)) {                                                 \
			printf("FAIL %s:%d: ", __FILE__, __LINE__);            \
			printf(__VA_ARGS__);                                   \
			printf("\n");                                          \
			failures++;                                            \
		}                                                              \
	} while (0)

/* An interval and unit with grove-vision-ai-v2's numbers, so the edge cases
 * sit where a real part puts them.  The subject is the arithmetic: nothing here
 * depends on these being any board's current layout. */
#define LO    0x00200000u
#define HI    0x00FFE000u
#define UNIT  0x00001000u

/* ---- the table the shared lookups read ------------------------------------ */

/* Three slots of different sizes with a gap between the second and third, so a
 * lookup that assumed contiguity or one size would answer wrongly. */
const struct blob_slot blob_map_slots[] = {
	{ LO,                 4u * UNIT },
	{ LO + 4u * UNIT,     2u * UNIT },
	{ LO + 16u * UNIT,    8u * UNIT },
};
const unsigned blob_map_slots_count =
	(unsigned)(sizeof blob_map_slots / sizeof blob_map_slots[0]);

static void t_lookups(void)
{
	unsigned i;

	CHECK(blob_map_check(LO, HI, UNIT, blob_map_slots, blob_map_slots_count,
	                     NULL) == BLOB_MAP_OK, "the test's own table is illegal");
	for (i = 0u; i < blob_map_slots_count; i++) {
		const struct blob_slot *s = blob_map_slot(i);

		CHECK(s == &blob_map_slots[i], "slot %u is not the table's entry", i);
		if (s == NULL)
			continue;
		/* The header is the first erase unit; the payload is the rest. */
		CHECK(blob_map_payload_addr(s, UNIT) == s->base + UNIT,
		      "slot %u payload does not start one unit in", i);
		CHECK(blob_map_payload_max(s, UNIT) == s->size - UNIT,
		      "slot %u payload size is wrong", i);

		/* A base resolves to its index; anything else resolves to
		 * nothing.  This direction only: an index never becomes a base
		 * except through the table. */
		CHECK(blob_map_index_of(s->base) == (int)i,
		      "slot %u does not resolve from its base", i);
		CHECK(blob_map_index_of(s->base + UNIT) == -1,
		      "an address inside slot %u resolved to a slot", i);
	}
	CHECK(blob_map_slot(blob_map_slots_count) == NULL,
	      "a slot past the end of the table was returned");
	CHECK(blob_map_slot(0xFFFFFFFFu) == NULL, "slot 0xFFFFFFFF was returned");
	/* The gap between the second and third slot names no slot. */
	CHECK(blob_map_index_of(LO + 6u * UNIT) == -1, "the gap resolved to a slot");
	CHECK(blob_map_index_of(0u) == -1, "0 resolved to a slot");
	CHECK(blob_map_index_of(HI) == -1, "the end of the interval is a slot");
	CHECK(blob_map_payload_max(&blob_map_slots[0], blob_map_slots[0].size) == 0u,
	      "a unit as large as the slot left payload behind");
	CHECK(blob_map_uncarved(LO, HI, blob_map_slots, blob_map_slots_count) ==
	      (HI - LO) - 14u * UNIT, "the test table's remainder is wrong");
}

/* ---- the interval and the table as a whole -------------------------------- */

static void t_interval(void)
{
	static const struct blob_slot one[] = { { LO, 2u * UNIT } };
	unsigned bad;

	CHECK(blob_map_check(LO, HI, 0u, one, 1u, &bad) == BLOB_MAP_BAD_INTERVAL,
	      "a zero erase unit was accepted");
	CHECK(blob_map_check(LO, HI, 3u, one, 1u, &bad) == BLOB_MAP_BAD_INTERVAL,
	      "an erase unit that is not a power of two was accepted");
	CHECK(blob_map_check(HI, LO, UNIT, one, 1u, &bad) ==
	      BLOB_MAP_BAD_INTERVAL, "an inverted interval was accepted");
	CHECK(blob_map_check(LO, LO, UNIT, one, 1u, &bad) ==
	      BLOB_MAP_BAD_INTERVAL, "an empty interval was accepted");
	/* Edges off the erase grid: an erase at the first address would take
	 * bytes below it, so no table inside can be safe. */
	CHECK(blob_map_check(LO + 1u, HI, UNIT, one, 1u, &bad) ==
	      BLOB_MAP_BAD_INTERVAL, "an unaligned lo was accepted");
	CHECK(blob_map_check(LO, HI + 1u, UNIT, one, 1u, &bad) ==
	      BLOB_MAP_BAD_INTERVAL, "an unaligned hi was accepted");

	CHECK(blob_map_check(LO, HI, UNIT, NULL, 1u, &bad) == BLOB_MAP_NO_SLOTS,
	      "a NULL table was accepted");
	CHECK(blob_map_check(LO, HI, UNIT, one, 0u, &bad) == BLOB_MAP_NO_SLOTS,
	      "an empty table was accepted");

	/* The interval is checked before the table, so a broken interval is not
	 * reported as a broken slot. */
	CHECK(blob_map_check(LO, HI, 0u, NULL, 0u, &bad) ==
	      BLOB_MAP_BAD_INTERVAL, "a broken interval was reported as slots");
}

static void t_slot_rules(void)
{
	struct blob_slot s[1];
	unsigned bad;

	s[0].base = LO + (UNIT / 2u);
	s[0].size = 2u * UNIT;
	CHECK(blob_map_check(LO, HI, UNIT, s, 1u, &bad) == BLOB_MAP_UNALIGNED &&
	      bad == 0u, "a base off the erase grid was accepted");

	s[0].base = LO;
	s[0].size = 2u * UNIT + 1u;
	CHECK(blob_map_check(LO, HI, UNIT, s, 1u, &bad) == BLOB_MAP_UNALIGNED,
	      "a size that is not whole units was accepted");

	/* A slot is a header sector plus payload; one unit is header only. */
	s[0].size = UNIT;
	CHECK(blob_map_check(LO, HI, UNIT, s, 1u, &bad) == BLOB_MAP_TOO_SMALL,
	      "a slot with no payload was accepted");
	s[0].size = 0u;
	CHECK(blob_map_check(LO, HI, UNIT, s, 1u, &bad) == BLOB_MAP_TOO_SMALL,
	      "an empty slot was accepted");

	s[0].base = LO - UNIT;
	s[0].size = 2u * UNIT;
	CHECK(blob_map_check(LO, HI, UNIT, s, 1u, &bad) == BLOB_MAP_OUTSIDE,
	      "a slot below the interval was accepted");
	s[0].base = HI;
	CHECK(blob_map_check(LO, HI, UNIT, s, 1u, &bad) == BLOB_MAP_OUTSIDE,
	      "a slot at the exclusive end was accepted");
	s[0].base = HI - UNIT;
	CHECK(blob_map_check(LO, HI, UNIT, s, 1u, &bad) == BLOB_MAP_OUTSIDE,
	      "a slot that runs one unit past the end was accepted");

	/* The wrap case, which is why the containment test is a subtraction:
	 * base + size would be 0x2001F000 and would look contained. */
	s[0].base = LO;
	s[0].size = 0xFFFFF000u;
	CHECK(blob_map_check(LO, HI, UNIT, s, 1u, &bad) == BLOB_MAP_OUTSIDE,
	      "a size that wraps past 2^32 was accepted");

	/* And the smallest legal slot, so the refusals above are not the only
	 * answer this function knows. */
	s[0].base = HI - 2u * UNIT;
	s[0].size = 2u * UNIT;
	CHECK(blob_map_check(LO, HI, UNIT, s, 1u, &bad) == BLOB_MAP_OK,
	      "the smallest slot at the top of the interval was refused");
}

static void t_pairs(void)
{
	struct blob_slot s[3];
	unsigned bad;

	/* Touching is not overlapping. */
	s[0].base = LO;             s[0].size = 2u * UNIT;
	s[1].base = LO + 2u * UNIT; s[1].size = 2u * UNIT;
	CHECK(blob_map_check(LO, HI, UNIT, s, 2u, &bad) == BLOB_MAP_OK,
	      "two abutting slots were refused");

	s[1].base = LO + UNIT;
	CHECK(blob_map_check(LO, HI, UNIT, s, 2u, &bad) == BLOB_MAP_OVERLAP &&
	      bad == 1u, "an overlap of one unit was accepted");

	/* Descending but disjoint is an ordering fault and nothing else. */
	s[0].base = LO + 2u * UNIT; s[0].size = 2u * UNIT;
	s[1].base = LO;             s[1].size = 2u * UNIT;
	CHECK(blob_map_check(LO, HI, UNIT, s, 2u, &bad) == BLOB_MAP_ORDER &&
	      bad == 1u, "a descending table was accepted");

	/* [!] Descending AND overlapping.  This is the case that decides the
	 * order of the two checks: reported as an ordering fault, the all-pairs
	 * loop would never answer anything the neighbour check had not already
	 * caught, and nobody would ever see it fail. */
	s[1].base = LO + UNIT;
	CHECK(blob_map_check(LO, HI, UNIT, s, 2u, &bad) == BLOB_MAP_OVERLAP,
	      "an out-of-order overlap was reported as an ordering fault");

	/* Two identical entries. */
	s[0].base = LO; s[0].size = 2u * UNIT;
	s[1].base = LO; s[1].size = 2u * UNIT;
	CHECK(blob_map_check(LO, HI, UNIT, s, 2u, &bad) == BLOB_MAP_OVERLAP,
	      "a table that lists one slot twice was accepted");

	/* [!] And the case only ALL PAIRS can see: slot 0 and slot 2 claim the
	 * same flash while neither touches slot 1.  A neighbour-by-neighbour
	 * check passes this table. */
	s[0].base = LO + 4u * UNIT; s[0].size = 2u * UNIT;
	s[1].base = LO;             s[1].size = 2u * UNIT;
	s[2].base = LO + 4u * UNIT; s[2].size = 2u * UNIT;
	CHECK(blob_map_check(LO, HI, UNIT, s, 3u, &bad) == BLOB_MAP_OVERLAP &&
	      bad == 2u, "an overlap between non-neighbours was accepted");
}

static void t_uncarved(void)
{
	struct blob_slot s[2];

	/* A table that claims the whole interval leaves nothing. */
	s[0].base = LO;                   s[0].size = (HI - LO) / 2u;
	s[1].base = LO + (HI - LO) / 2u;  s[1].size = (HI - LO) - s[0].size;
	CHECK(blob_map_uncarved(LO, HI, s, 2u) == 0u,
	      "a table claiming everything left a remainder");

	s[1].size -= UNIT;
	CHECK(blob_map_uncarved(LO, HI, s, 2u) == UNIT,
	      "one unit short did not leave one unit");

	CHECK(blob_map_uncarved(LO, HI, s, 0u) == HI - LO,
	      "an empty table did not leave the whole interval");
	CHECK(blob_map_uncarved(LO, HI, NULL, 2u) == 0u,
	      "a NULL table did not answer nothing");
	CHECK(blob_map_uncarved(HI, LO, s, 2u) == 0u,
	      "an inverted interval did not answer nothing");

	/* A table claiming more than there is answers "nothing spare" rather
	 * than wrapping into a large number that reads as room. */
	s[0].size = 0xF0000000u;
	s[1].size = 0xF0000000u;
	CHECK(blob_map_uncarved(LO, HI, s, 2u) == 0u,
	      "an oversized table wrapped into free space");
}

int main(void)
{
	t_lookups();
	t_interval();
	t_slot_rules();
	t_pairs();
	t_uncarved();

	if (failures != 0) {
		printf("test_blob_map (svc): %d failure(s)\n", failures);
		return 1;
	}
	printf("test_blob_map (svc): all passed\n");
	return 0;
}
