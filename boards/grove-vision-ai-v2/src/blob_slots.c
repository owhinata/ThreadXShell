/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    blob_slots.c
 * @brief   This board's asset slot table (#92, #94; split from blob_map.c at
 *          #192).
 *
 * The rules a table has to satisfy and the lookups over it are shared
 * (svc/blob/blob_map.c).  What is here is the one thing that is this board's:
 * which slots the writable NOR interval is carved into.  It is handed over
 * under the fixed names svc/blob/blob_map.h declares, as const data, so the
 * shared lookups read exactly this table and nothing can pass them another.
 * test/test_blob_map.c pins it entry by entry; shell/test/test_blob_map.c
 * walks the rules with the broken tables this board could never ship.
 */
#include "blob_map.h"

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
 * out of the XIP window and never copied, the backend's length limit is whatever
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
const struct blob_slot blob_map_slots[] = {
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
