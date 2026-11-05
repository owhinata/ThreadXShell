/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Host test for this board's asset slot table (issue #194, src/blob_slots.c),
 * compiled against the board's real src/blob.h -- the same two checks as
 * wio-lite-ai's test/test_blob_slots.c:
 *
 *   1. THE TABLE IT SHIPS, entry by entry.  The index is a display ordinal and
 *      the base is the identity, so a slot that moves strands whatever was
 *      stored in it: an edit that swapped two slots, or inserted one and
 *      dropped another, keeps the count and silently renumbers the store.
 *   2. THAT THE TABLE IS LEGAL UNDER ITS OWN INTERVAL, carves all of it, and is
 *      whole 64 KB sectors.
 *
 * The rules a table has to satisfy, fed the broken tables no firmware ships,
 * are tested once for every board in shell/test/test_blob_map.c; the header
 * codec in shell/test/test_blob_state.c.
 */
#include <stdint.h>
#include <stdio.h>
#include <stddef.h>
#include <string.h>

#include "blob.h"        /* BLOB_REGION_BASE / _END / BLOB_ERASE_UNIT -- the real ones */
#include "qspi_flash.h"  /* QSPI_FLASH_SECTOR_SIZE -- the slot erase unit */
#include "blob_map.h"
#include "blob_state.h"

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

/* ---- 1. the shipped table ------------------------------------------------- */

/* The shipped table, entry by entry.  None of these may move without stranding
 * whatever is stored in the slot that moved (issue #194). */
static const struct blob_slot want[] = {
	{ 0x00100000u, 0x00100000u },   /* 1 MB   */
	{ 0x00200000u, 0x00100000u },
	{ 0x00300000u, 0x00100000u },
	{ 0x00400000u, 0x00080000u },   /* 512 KB */
	{ 0x00480000u, 0x00080000u },
	{ 0x00500000u, 0x00080000u },
	{ 0x00580000u, 0x00080000u },
	{ 0x00600000u, 0x00080000u },
	{ 0x00680000u, 0x00080000u },
	{ 0x00700000u, 0x00040000u },   /* 256 KB */
	{ 0x00740000u, 0x00040000u },
	{ 0x00780000u, 0x00040000u },
	{ 0x007C0000u, 0x00040000u },
};

#define N_WANT  ((unsigned)(sizeof want / sizeof want[0]))

static void t_shipped_table(void)
{
	uint32_t carved = 0u, prev_size = 0xFFFFFFFFu;
	unsigned i;

	CHECK(blob_map_count() == N_WANT, "table has %u slots, expected %u",
	      blob_map_count(), N_WANT);
	CHECK(blob_map_slots_count == blob_map_count(),
	      "the count under the fixed name disagrees with blob_map_count()");
	CHECK(blob_map_table() == blob_map_slots,
	      "blob_map_table() is not the table under the fixed name");
	CHECK(blob_map_count() <= BLOB_MAX_SLOTS, "more slots than BLOB_MAX_SLOTS");
	if (blob_map_count() != N_WANT)
		return;

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
	CHECK(blob_map_slot(N_WANT) == NULL, "there is a fourteenth slot");

	/* 3 x 1 MB + 6 x 512 KB + 4 x 256 KB = 7 MB, the whole interval. */
	CHECK(want[0].base == BLOB_REGION_BASE,
	      "the table does not start at the interval's start");
	CHECK(carved == 0x00700000u, "table carves 0x%X, expected 0x700000",
	      (unsigned)carved);
	CHECK(want[N_WANT - 1u].base + want[N_WANT - 1u].size == BLOB_REGION_END,
	      "the table does not end at the interval's end");

	/* [!] NO GAPS, AND DESCENDING BY CLASS.  blob_map_check() proves the slots
	 * are disjoint and ascending, which a table full of holes also satisfies --
	 * and a hole is flash no slot names. */
	for (i = 1u; i < N_WANT; i++)
		CHECK(want[i].base == want[i - 1u].base + want[i - 1u].size,
		      "slot %u does not abut slot %u", i, i - 1u);
	for (i = 0u; i < N_WANT; i++) {
		CHECK(want[i].size <= prev_size,
		      "slot %u is larger than the one before it", i);
		prev_size = want[i].size;
	}
}

/* ---- 2. legal under its own interval ------------------------------------- */

static void t_shipped_table_is_legal(void)
{
	unsigned bad = 99u, i;
	enum blob_map_verdict v;

	/* The interval and unit src/blob.h compiles into the firmware, not a copy:
	 * this is the same call the store makes at every entry point. */
	CHECK(BLOB_REGION_BASE == 0x00100000u, "the interval no longer starts "
	      "above the reserved first 1 MB");
	CHECK(BLOB_REGION_END == 0x00800000u, "the interval's end moved");
	CHECK(BLOB_ERASE_UNIT == 0x1000u, "the erase unit is not the 4 KB subsector");

	v = blob_map_check(BLOB_REGION_BASE, BLOB_REGION_END, BLOB_ERASE_UNIT,
	                   blob_map_table(), blob_map_count(), &bad);
	CHECK(v == BLOB_MAP_OK, "the shipped table is %s (slot %u)",
	      blob_map_verdict_name(v), bad);
	CHECK(blob_map_uncarved(BLOB_REGION_BASE, BLOB_REGION_END,
	                        blob_map_table(), blob_map_count()) == 0u,
	      "part of the interval is named by no slot");

	for (i = 0u; i < blob_map_count(); i++) {
		const struct blob_slot *s = blob_map_slot(i);

		/* The header is the first erase unit; the payload is the rest. */
		CHECK(blob_map_payload_addr(s, BLOB_ERASE_UNIT) ==
		      s->base + BLOB_ERASE_UNIT,
		      "slot %u payload does not start one unit in", i);
		CHECK(blob_map_payload_max(s, BLOB_ERASE_UNIT) ==
		      s->size - BLOB_ERASE_UNIT, "slot %u payload size is wrong", i);
		/* `blob write` erases a slot one 64 KB sector at a time (the
		 * cancel comes between them), so every slot is whole sectors --
		 * otherwise its tail would go to 4 KB subsector erases, sixteen
		 * times the commands for the same bytes. */
		CHECK((s->base % QSPI_FLASH_SECTOR_SIZE) == 0u &&
		      (s->size % QSPI_FLASH_SECTOR_SIZE) == 0u,
		      "slot %u is not whole 64 KB sectors", i);
		CHECK(blob_map_index_of(s->base) == (int)i,
		      "slot %u does not resolve from its base", i);
		CHECK(blob_map_index_of(s->base + BLOB_ERASE_UNIT) == -1,
		      "an address inside slot %u resolved to a slot", i);
	}
	CHECK(blob_map_index_of(BLOB_REGION_END) == -1,
	      "the end of the interval is a slot");
	CHECK(blob_map_payload_max(blob_map_slot(0), BLOB_ERASE_UNIT) == 1044480u,
	      "the 1 MB slot's payload is not 1,044,480 B");
}

int main(void)
{
	t_shipped_table();
	t_shipped_table_is_legal();

	if (failures != 0) {
		printf("test_blob_slots: %d failure(s)\n", failures);
		return 1;
	}
	printf("test_blob_slots: all passed\n");
	return 0;
}
