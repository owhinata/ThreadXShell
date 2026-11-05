/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    blob_map.h
 * @brief   How a board's writable flash interval is carved into asset slots
 *          (#92; shared since #192).
 *
 * [!] THIS FILE DECLARES NO BOUNDARY.  The writable interval is the board's
 * (grove-vision-ai-v2: nor_seam_limits in port/sdk_seam/nor_seam.h, which its
 * board.cmake compiles in and its cmake/check_nor_seam.py reads back out of the
 * linked image); the board's table is a CONSUMER of those three numbers, and
 * blob_map_check() takes them as arguments so it cannot quietly grow a second
 * opinion about them.  That matters twice over: a table that disagreed with the
 * seam would be refused by the seam anyway (the slots are where the writer
 * aims, not what bounds it), and when grove-vision-ai-v2's #49 Step 4b moved its models into blob and the
 * interval grew to 0x200000..0xFFE000, the only thing that changed was that
 * board's table.
 *
 * FIXED SLOTS IN SIZE CLASSES, NOT VARIABLE EXTENTS.  What lands in a store is
 * a few models and assets, and the operation that matters is replacing one in
 * place, so an allocator would buy nothing and cost external fragmentation.
 * The one real weakness of fixed slots -- a small model sitting in a large hole
 * -- is what size classes are for.  Which classes, how many of each and where
 * they start is the board's table, not this file's: grove-vision-ai-v2's is
 * boards/grove-vision-ai-v2/src/blob_slots.c, which also says why its largest
 * class is the size it is and why the top of its interval is left uncarved.
 *
 * [!] IDENTITY IS THE BASE ADDRESS, NEVER THE INDEX.  Reorder the table and
 * every index means a different piece of flash, so nothing that persists may
 * carry one: a stored header names its own base (svc/blob/blob_state.h), and
 * the index `blob list` prints is a display ordinal resolved against the table
 * on the spot.  blob_map_index_of() is the resolver for the other direction.
 *
 * [!] WHICH IS WHY THE ONLY SAFE EDIT TO A SHIPPED TABLE IS AN APPEND.  A slot
 * that moves strands whatever was stored in it: the header is still in the
 * flash, it still names its own base, and no entry has that base any more, so
 * nothing resolves it.  (grove-vision-ai-v2 re-carved its table whole once, at
 * issue #94, and paid that price on purpose; src/blob_slots.c has the account.)
 *
 * Pure arithmetic -- no hardware, no ThreadX -- so shell/test/test_blob_map.c
 * compiles svc/blob/blob_map.c on the host and feeds blob_map_check() the
 * broken tables that a firmware, which ships one table, could never produce.
 * The table itself is the board's (grove-vision-ai-v2: src/blob_slots.c, pinned
 * entry by entry by that board's test/test_blob_map.c).
 */
#ifndef BLOB_MAP_H
#define BLOB_MAP_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * The most slots this build will look at in one pass.
 *
 * [!] A BOUND ON A STACK ARRAY, not a statement about the table.  The writer
 * decides which slot to use from one view per slot, gathered under the
 * reservation, and that array has to be a fixed size on a console thread's
 * stack (4 KB on grove-vision-ai-v2, whose table has eleven).  A table with
 * more than this is refused rather than truncated, because choosing from a
 * truncated view is how a write ends up somewhere nobody looked at.
 */
#define BLOB_MAX_SLOTS   32u

/**
 * One slot: where it starts and how much flash it owns, header included.
 *
 * The first erase unit of @ref base is the header sector and the rest is
 * payload, which is why blob_map_check() insists on at least two units: a slot
 * with no payload is not a slot.  The unit is not stored -- it belongs to the
 * seam, and every function here that needs it takes it as an argument.
 */
struct blob_slot {
	uint32_t base;
	uint32_t size;
};

/** Why a table is or is not one this port may use. */
enum blob_map_verdict {
	BLOB_MAP_OK = 0,
	BLOB_MAP_NO_SLOTS,     /**< an empty table carves nothing            */
	BLOB_MAP_BAD_INTERVAL, /**< lo/hi/unit cannot bound anything         */
	BLOB_MAP_UNALIGNED,    /**< a base or size is not whole erase units  */
	BLOB_MAP_TOO_SMALL,    /**< a slot has a header and no payload       */
	BLOB_MAP_OUTSIDE,      /**< a slot is not wholly inside the interval */
	BLOB_MAP_ORDER,        /**< the table is not strictly ascending      */
	BLOB_MAP_OVERLAP,      /**< two slots claim the same flash           */
};

/** Short name, for the refusal message and the host test's diagnostics. */
const char *blob_map_verdict_name(enum blob_map_verdict v);

/**
 * Does @p v name a slot, i.e. is blob_map_check()'s @p bad meaningful?
 *
 * Two of the verdicts are about the table as a whole and would otherwise be
 * reported against slot 0, which is a slot that exists and is probably fine.
 * The answer lives here rather than in whatever prints the message, so there
 * is one statement of which verdicts carry an index.
 */
int blob_map_verdict_names_slot(enum blob_map_verdict v);

/**
 * @brief  Is @p slots a table this port may act on inside [@p lo, @p hi)?
 *
 * @param unit   bytes one permitted erase destroys; a power of two
 * @param bad    receives the offending slot index; set to 0 for the two
 *               verdicts that are about the table as a whole, and meaningless
 *               unless the verdict names a slot.  May be NULL.
 *
 * Checked, in this order: the interval itself; at least one slot; every base
 * and size a whole number of units, every slot at least two of them and wholly
 * inside the interval; no two slots overlapping, over ALL PAIRS; and only then
 * strictly ascending.
 *
 * [!] OVERLAP IS DECIDED BEFORE ORDER, and that ordering of the checks is the
 * point.  The other way round, two slots that overlapped while out of order
 * would be reported as an ordering fault and the overlap loop would never
 * answer anything an ordering check had not already caught -- a test nobody
 * could ever see fail.  Disjoint and ascending are separate facts: the first is
 * what keeps two slots off the same flash, the second is what makes #49 Step 4
 * an append.
 *
 * [!] THE ARITHMETIC IS SUBTRACTION-BASED, in the same discipline as
 * nor_span.c: `base + size` is not formed until `size <= hi - base` has been
 * established, so a table with a size near 2^32 is refused rather than wrapped
 * into looking contained.
 */
enum blob_map_verdict blob_map_check(uint32_t lo, uint32_t hi, uint32_t unit,
                                     const struct blob_slot *slots,
                                     unsigned count, unsigned *bad);

/**
 * The table this firmware ships, and how many slots it has.
 *
 * [!] DEFINED BY THE BOARD, UNDER THESE NAMES, AS CONST DATA (issue #192).
 * The table is the board's fact; the lookups below (blob_map_slot(),
 * blob_map_index_of()) are shared and read it by name, so there is no argument
 * through which a caller could hand them a different table from the one the
 * board links.  A board without a blob store defines neither and links none of
 * this.
 */
extern const struct blob_slot blob_map_slots[];
extern const unsigned blob_map_slots_count;

/** The same table and count through a call; also defined by the board. */
const struct blob_slot *blob_map_table(void);
unsigned blob_map_count(void);

/** The @p i-th slot of the shipped table, or NULL when @p i is past its end. */
const struct blob_slot *blob_map_slot(unsigned i);

/**
 * The shipped table's index for the slot based at @p base, or -1.  This is how
 * a base address recovered from flash becomes something to print -- the
 * direction that must never be inverted into "index 3 is at 0x700000".
 */
int blob_map_index_of(uint32_t base);

/**
 * Where @p s's payload starts and how much of it there is, given the erase
 * @p unit that the header sector occupies.  Both assume a table that
 * blob_map_check() has passed; on one it did not, they are arithmetic on
 * numbers that mean nothing.
 */
uint32_t blob_map_payload_addr(const struct blob_slot *s, uint32_t unit);
uint32_t blob_map_payload_max(const struct blob_slot *s, uint32_t unit);

/**
 * Bytes of [@p lo, @p hi) that @p slots does not claim -- the remainder
 * `blob free` reports apart from the free slots, so that nobody reads it as
 * space they can put something in.  Zero when the table claims at least the
 * whole interval, which on a checked table means it claims exactly it.
 */
uint32_t blob_map_uncarved(uint32_t lo, uint32_t hi,
                           const struct blob_slot *slots, unsigned count);

#ifdef __cplusplus
}
#endif

#endif /* BLOB_MAP_H */
