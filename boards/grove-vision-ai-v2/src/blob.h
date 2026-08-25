/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    blob.h
 * @brief   The asset store's read side: what is in each slot (#92).
 *
 * The store itself is three files.  blob_map.c carves the writable interval
 * into slots, blob_state.c says what the bytes at the top of one mean, and this
 * is where those two meet the flash: it takes the lease, invalidates, decodes
 * and hands back an answer.  The write side (#49 Step 2, implementation order
 * items 6 and 7) lands on top of it.
 *
 * [!] EVERY READ TAKES A LEASE, and taking one is also what brings the window
 * up.  Reading the memory-mapped alias without one is issue #90: a window that
 * was never brought up does not fault and does not read 0xFF, it aliases one
 * register across all 16 MB, and a dump of it printed plausible nonsense as
 * flash contents.  So there is no "read it quickly without the ceremony" entry
 * point here, and there is not going to be one.
 *
 * [!] AND EVERY READ INVALIDATES FIRST.  The vendor's XIP restore invalidates
 * 512 bytes at the base of the window and nothing else (nor_flash.h), so after
 * any write transaction the cache can still hold what a slot USED to say.  A
 * listing that answered from that would be reporting the flash as it was
 * before somebody wrote it.
 *
 * ONE SLOT AT A TIME, LEASE AND ALL.  A listing is ten separate leases rather
 * than one held across the whole walk, which means it is ten snapshots and not
 * one.  That is the honest shape: nothing can be writing while a lease is out,
 * and a writer that takes the part half way through a listing makes the
 * remaining rows report busy rather than silently reporting the old contents.
 * Holding one lease across the walk would keep the window from `nn` for as
 * long as the printing took, and would still be a snapshot of ten reads.
 */
#ifndef BLOB_H
#define BLOB_H

#include <stdint.h>

#include "blob_map.h"
#include "blob_state.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- returns ------------------------------------------------------------- */

#define BLOB_OK          0
#define BLOB_ERR_PARAM  -1   /**< no such slot, or a range outside the payload */
#define BLOB_ERR_BUSY   -2   /**< the NOR would not grant a lease              */
#define BLOB_ERR_MAP    -3   /**< the slot table does not fit the seam's limits */

/* ---- the table, checked against the seam ---------------------------------- */

/**
 * @brief  Is the shipped slot table one this port may act on?
 *
 * @param bad  receives the offending slot index when the verdict names one
 *
 * Checked at every entry point rather than once at boot: the table is
 * const, but the interval it is checked against is a record in .rodata that
 * board.cmake compiles in, and a build whose partitions moved should refuse
 * rather than write where the old table said.  It costs a hundred
 * comparisons on a command that is about to read flash.
 */
enum blob_map_verdict blob_check_map(unsigned *bad);

/** Flash offset of @p slot's payload, and how many bytes of it there are.
 *  Zero for a slot that does not exist.  Both come from the checked table. */
uint32_t blob_payload_addr(unsigned slot);
uint32_t blob_payload_max(unsigned slot);

/* ---- reads ---------------------------------------------------------------- */

/**
 * @brief  Decode @p slot's header.
 *
 * @param out  receives the state and, when the header decoded, the fields
 * @param why  receives why an INVALID slot did not decode.  May be NULL
 * @return BLOB_OK whenever the slot could be READ -- @p out then says what was
 *         there, including "nothing" -- or BLOB_ERR_BUSY / _MAP / _PARAM
 *
 * "Empty" and "the flash would not answer" are different answers and a listing
 * has to be able to tell them apart, which is why the state is not folded into
 * the return.
 */
int blob_stat(unsigned slot, struct blob_info *out, enum blob_hdr_reject *why);

/**
 * @brief  Copy @p len payload bytes from @p off within @p slot into @p buf.
 *
 * Bounded by the payload AREA and not by any stored length: a hexdump may
 * legitimately look past the end of a blob, or at a slot whose header says
 * nothing at all.  A consumer that wants the blob itself takes the length from
 * blob_stat() first.
 */
int blob_read(unsigned slot, uint32_t off, void *buf, uint32_t len);

#ifdef __cplusplus
}
#endif

#endif /* BLOB_H */
