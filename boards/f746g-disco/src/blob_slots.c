/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    blob_slots.c
 * @brief   This board's asset slot table (issue #194).
 *
 * The rules a table has to satisfy and the lookups over it are shared
 * (svc/blob/blob_map.c).  What is here is the one thing that is this board's:
 * how the writable QSPI interval src/blob.h declares -- [BLOB_REGION_BASE,
 * BLOB_REGION_END) -- is carved into slots.  It is handed over under the fixed
 * names svc/blob/blob_map.h declares, as const data, so the shared lookups read
 * exactly this table and nothing can pass them another.  test/test_blob_slots.c
 * pins it entry by entry.
 *
 * [!] THE SAME VALUES AS wio-lite-ai's TABLE, AND A COPY OF THEM ON PURPOSE.
 * The two boards have the same interval and the same staging buffer (1 MB), so
 * the same classes fit both, and an asset built for a slot means the same slot
 * on either.  It is not a shared file: a table is a statement about one board's
 * flash, and a place two boards share would make an append on one an edit of
 * the other.
 *
 * [!] APPEND ONLY FROM THE FIRST SHIPPED IMAGE.  Identity is the base address,
 * never the index: a stored header names its own slot's base
 * (svc/blob/blob_state.h), so a slot that moves strands whatever was stored in
 * it.
 *
 * WHY THESE CLASSES.  The largest is 1 MB because that is what the tflm
 * staging buffer holds (NN_TFLM_MODEL_KB): a model is copied out of its slot
 * into that buffer before it is parsed, so a slot larger than it would store
 * something `nn model load` could never take (cmake/check_blob_staging.py reads
 * this table and that buffer's size back out of the linked image and stops the
 * build if any slot's payload is larger).  The 512 KB class (520,192 B of
 * payload) takes the mid-size models and the 256 KB class (258,048 B) the small
 * ones and the containers.  Largest first, so a listing reads by size.
 *
 * [!] THE WHOLE INTERVAL IS CARVED: 3 x 1 MB + 6 x 512 KB + 4 x 256 KB is
 * exactly the 7 MB of [0x100000, 0x800000), with no gap, and every slot is
 * whole 64 KB sectors (blob_erase_span() erases a slot one sector per call).
 *
 * The bases are spelled out rather than computed: a loop that generated them
 * would be a second statement of the same layout.
 */
#include "blob_map.h"

const struct blob_slot blob_map_slots[] = {
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

/* More slots than BLOB_MAX_SLOTS would not overflow anything -- `blob write`
 * and the nn adapter refuse such a table at run time -- but that refusal is
 * first seen when somebody types the command on hardware, so the file that
 * ships the table stops the build instead (as grove-vision-ai-v2's does). */
_Static_assert(sizeof blob_map_slots / sizeof blob_map_slots[0] <= BLOB_MAX_SLOTS,
               "the blob slot table has more slots than BLOB_MAX_SLOTS");

const unsigned blob_map_slots_count =
	(unsigned)(sizeof blob_map_slots / sizeof blob_map_slots[0]);

const struct blob_slot *blob_map_table(void)
{
	return blob_map_slots;
}

unsigned blob_map_count(void)
{
	return blob_map_slots_count;
}
