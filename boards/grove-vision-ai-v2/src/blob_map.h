/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    blob_map.h
 * @brief   How the writable NOR interval is carved into asset slots (#92).
 *
 * [!] THIS FILE DECLARES NO BOUNDARY.  The writable interval is
 * nor_seam_limits (port/sdk_seam/nor_seam.h), which board.cmake compiles in and
 * cmake/check_nor_seam.py reads back out of the linked image; the table here is
 * a CONSUMER of those three numbers, and blob_map_check() takes them as
 * arguments so it cannot quietly grow a second opinion about them.  That
 * matters twice over: a table that disagreed with the seam would be refused by
 * the seam anyway (the slots are where the writer aims, not what bounds it),
 * and #49 Step 4 moves the models into blob and the interval grows to
 * 0x200000..0xFFE000 -- at which point the only thing that changes here is the
 * table.
 *
 * FIXED SLOTS IN SIZE CLASSES, NOT VARIABLE EXTENTS.  What lands here is a few
 * models and assets, and the operation that matters is replacing one in place,
 * so an allocator would buy nothing and cost external fragmentation.  The one
 * real weakness of fixed slots -- a 164 KB detector sitting in a 2 MB hole --
 * is what the size classes are for:
 *
 *     2 MB   x2   0x200000, 0x400000
 *     1 MB   x3   0x600000, 0x700000, 0x800000
 *     512 KB x3   0x900000, 0x980000, 0xA00000
 *     256 KB x2   0xA80000, 0xAC0000
 *
 * Ten slots, 9 MB exactly, ending at 0xB00000.
 *
 * [!] AND THE 503,808 B ABOVE THEM ARE NOT CARVED.  Not because they are too
 * small to hold a slot -- they would hold another 256 KB one and change -- but
 * because the table stops on a round nine megabytes and Step 4 re-carves the
 * whole region anyway.  `blob free` reports it separately for exactly that
 * reason: it is not free space, because nothing here can name it.
 *
 * [!] IDENTITY IS THE BASE ADDRESS, NEVER THE INDEX.  Reorder the table and
 * every index means a different piece of flash, so nothing that persists may
 * carry one: a stored header names its own base (blob_state.h), and the index
 * `blob list` prints is a display ordinal resolved against the table on the
 * spot.  blob_map_index_of() is the resolver for the other direction.
 *
 * Pure arithmetic -- no hardware, no ThreadX -- so test/test_blob_map.c
 * compiles this exact file on the host and feeds blob_map_check() the broken
 * tables that the firmware, which ships one table, could never produce.
 */
#ifndef BLOB_MAP_H
#define BLOB_MAP_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

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

/** The table this firmware ships, and how many slots it has. */
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
