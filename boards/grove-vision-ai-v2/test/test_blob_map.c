/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Host test for this board's asset slot table (#92, src/blob_slots.c).
 *
 * The rules a table has to satisfy, fed the broken tables no firmware ships,
 * are tested once for every board in shell/test/test_blob_map.c (the rules are
 * svc/blob/blob_map.c since #192).  What is left here is this board's half:
 * that the table it ships is legal under its own interval, and that it is the
 * table it has always been.
 *
 * [!] PINNING THE TABLE.  Every entry is spelled out here base by base,
 * because the index is a display ordinal and the base is the identity: a slot that moves strands whatever was stored in it, since the
 * stored header still names a base no table entry has any more.
 *
 * This used to be phrased as a GOLDEN PREFIX -- #92's ten entries, unmovable,
 * with room to append above them.  #94 spent that prefix once and on purpose,
 * re-carving the whole table largest-first at the one moment when the store
 * held a single model that survived the move and two files worth re-sending.
 * The check is stronger for it rather than weaker: the WHOLE table is pinned
 * now, not just its head, so any entry that moves fails here.  The next
 * re-carve pays whatever is stored at the time, and this file is where somebody
 * proposing one has to come first.
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

/* The interval and unit the firmware is compiled with today.  Restated here
 * rather than taken from board.cmake for the same reason test_nor_write.c
 * restates them: the subject is the arithmetic, and numbers that moved with the
 * layout would stop covering the edges they were written for.
 * cmake/check_nor_seam.py is what ties the firmware's copy to the layout. */
/* [!] HI MOVED TO THE SLOT HEADER (issue #94).  Unlike test_nor_write.c, whose
 * LO/HI are frozen numbers chosen to put edge cases where it wants them, the
 * cases below feed the SHIPPED table to blob_map_check() -- so this interval
 * has to be the shipped one or that check is against a layout nobody runs. */
#define LO    0x00200000u
#define HI    0x00FFE000u
#define UNIT  0x00001000u

/* ---- the shipped table --------------------------------------------------- */

/* The shipped table, entry by entry.  None of these may move without stranding
 * whatever is stored in the slot that moved (#94). */
static const struct blob_slot want[] = {
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

#define N_WANT  ((unsigned)(sizeof want / sizeof want[0]))

static void t_shipped_table(void)
{
	uint32_t carved = 0u, prev_size = 0xFFFFFFFFu;
	unsigned i;

	CHECK(blob_map_count() == N_WANT, "table has %u slots, expected %u",
	      blob_map_count(), N_WANT);
	if (blob_map_count() != N_WANT)
		return;

	/* Entry by entry rather than by count alone: an edit that swapped two
	 * slots, or inserted one and dropped another, keeps the count and
	 * renumbers assets already stored.  That is the mistake worth catching. */
	for (i = 0u; i < N_WANT; i++) {
		const struct blob_slot *s = blob_map_slot(i);

		CHECK(s != NULL, "slot %u is missing", i);
		if (s == NULL)
			continue;
		CHECK(s->base == want[i].base && s->size == want[i].size,
		      "slot %u is 0x%08X+0x%X, expected 0x%08X+0x%X", i,
		      (unsigned)s->base, (unsigned)s->size,
		      (unsigned)want[i].base, (unsigned)want[i].size);
		carved += s->size;
	}
	CHECK(blob_map_slot(N_WANT) == NULL, "there is a twelfth slot");

	/* 13 MB exactly, from the first writable address to where the uncarved
	 * tail begins. */
	CHECK(want[0].base == LO, "the table does not start at the interval's lo");
	CHECK(carved == 0x00D00000u, "table carves 0x%X, expected 0xD00000",
	      (unsigned)carved);
	CHECK(want[N_WANT - 1u].base + want[N_WANT - 1u].size == 0x00F00000u,
	      "the table does not end at 0xF00000");

	/* [!] NO GAPS, AND DESCENDING BY CLASS.  blob_map_check() proves the
	 * slots are disjoint and ascending, which a table full of holes also
	 * satisfies -- and a hole is flash no slot names, indistinguishable from
	 * the deliberate tail below.  Contiguity is what makes the uncarved
	 * remainder ONE run, which is what `blob free` reports it as. */
	for (i = 1u; i < N_WANT; i++)
		CHECK(want[i].base == want[i - 1u].base + want[i - 1u].size,
		      "slot %u does not abut slot %u", i, i - 1u);
	for (i = 0u; i < N_WANT; i++) {
		CHECK(want[i].size <= prev_size,
		      "slot %u is larger than the one before it", i);
		prev_size = want[i].size;
	}
}

static void t_shipped_table_is_legal(void)
{
	unsigned bad = 99u;
	enum blob_map_verdict v;

	v = blob_map_check(LO, HI, UNIT, blob_map_table(), blob_map_count(),
	                   &bad);
	CHECK(v == BLOB_MAP_OK, "the shipped table is %s (slot %u)",
	      blob_map_verdict_name(v), bad);

	/* [!] THE REMAINDER IS BIG ENOUGH TO CARVE AND IS LEFT ALONE ANYWAY
	 * (issue #94).  Before #92's ten grew, it was 503,808 B; it is now
	 * 1,040,384 B, which would hold a 512 KB slot and a 256 KB one -- the
	 * shape the first draft of this change had.  It is uncarved because
	 * nothing needs those classes while three 1 MB and two 512 KB slots are
	 * still free, so the assertion says the remainder is LARGE rather than
	 * small: a future edit that carves it has to come here and say so.
	 * `blob free` reports it apart from the free slots either way, because
	 * no slot names those bytes. */
	CHECK(blob_map_uncarved(LO, HI, blob_map_table(), blob_map_count()) ==
	      HI - 0x00F00000u, "uncarved remainder is not 0x%X",
	      (unsigned)(HI - 0x00F00000u));
	CHECK(HI - 0x00F00000u == 1040384u, "the remainder moved");
	CHECK(1040384u > 0x00080000u, "the remainder no longer holds a 512 KB slot");
	/* And not a 1 MB slot, which is why the draft's tail was 512 KB + 256 KB
	 * rather than a member of an existing class. */
	CHECK(1040384u < 0x00100000u, "a 1 MB slot would now fit the remainder");
}

static void t_derivations(void)
{
	unsigned i;

	for (i = 0u; i < blob_map_count(); i++) {
		const struct blob_slot *s = blob_map_slot(i);

		/* The header is the first erase unit; the payload is the rest,
		 * which is what keeps it 4 KB aligned for the NPU. */
		CHECK(blob_map_payload_addr(s, UNIT) == s->base + UNIT,
		      "slot %u payload does not start one unit in", i);
		CHECK(blob_map_payload_max(s, UNIT) == s->size - UNIT,
		      "slot %u payload size is wrong", i);
		CHECK((blob_map_payload_addr(s, UNIT) % UNIT) == 0u,
		      "slot %u payload is not unit aligned", i);

		/* A base resolves to its index; anything else resolves to
		 * nothing.  This direction only: an index never becomes a base
		 * except through the table. */
		CHECK(blob_map_index_of(s->base) == (int)i,
		      "slot %u does not resolve from its base", i);
		CHECK(blob_map_index_of(s->base + UNIT) == -1,
		      "an address inside slot %u resolved to a slot", i);
	}
	CHECK(blob_map_index_of(0u) == -1, "0 resolved to a slot");
	CHECK(blob_map_index_of(HI) == -1, "the end of the interval is a slot");
	CHECK(blob_map_payload_max(blob_map_slot(0), blob_map_slot(0)->size) == 0u,
	      "a unit as large as the slot left payload behind");
}

int main(void)
{
	t_shipped_table();
	t_shipped_table_is_legal();
	t_derivations();

	if (failures != 0) {
		printf("test_blob_map: %d failure(s)\n", failures);
		return 1;
	}
	printf("test_blob_map: all passed\n");
	return 0;
}
