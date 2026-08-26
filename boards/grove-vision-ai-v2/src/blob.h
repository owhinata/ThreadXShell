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
/**
 * The NOR port is faulted.  TERMINAL, and told apart from BLOB_ERR_BUSY on
 * purpose: nor_acquire() refuses for both reasons, and folding them together
 * would send an operator to `nor info` to find out who is holding the part
 * when the answer is that nothing is and a reset is required.  The three
 * entry points here classify a failure the same way, which is the rule
 * issue #79 wrote for frame_pipeline: an entry point that calls a terminal
 * failure retryable has a caller retrying forever.
 */
#define BLOB_ERR_FAULT  -4
#define BLOB_ERR_EMPTY  -5   /**< the slot holds no header to check against  */
#define BLOB_ERR_CRC    -6   /**< the payload does not match its stored CRC  */

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

/**
 * @brief  Re-read @p slot's payload and check it against its stored CRC-32.
 *
 * @param out       receives the header it checked against.  May be NULL
 * @param computed  receives what the flash actually adds up to.  May be NULL
 * @return BLOB_OK on a match, BLOB_ERR_CRC on a mismatch, BLOB_ERR_EMPTY when
 *         the slot holds no header with a length and a CRC in it
 *
 * The stored CRC is of the stream that arrived, so this compares the flash
 * against the PC's file rather than against itself.  It reads through the
 * staging buffer, which is safe because the buffer's other user is a write and
 * a write holds the reservation -- so the lease this takes cannot be out at the
 * same time.
 *
 * [!] THE HEADER AND THE PAYLOAD ARE READ UNDER ONE LEASE.  Taking a second one
 * in between would leave a window for a background write to change the slot,
 * and the result of that race is a PASS against a header that has been
 * replaced.  @p out is the header it actually compared against, so a caller
 * cannot print one thing and have checked another.
 *
 * INCOMPLETE slots are checked too: the body is there, so there is something to
 * check against, and "the payload of an interrupted transfer is intact as far
 * as it got" is a question worth being able to ask.
 */
int blob_verify(unsigned slot, struct blob_info *out, uint32_t *computed);

/* ---- for the writer -------------------------------------------------------- */

/**
 * @brief  blob_stat() for the holder of the writer reservation.
 *
 * Same answer, no lease -- a reservation refuses leases, so a writer deciding
 * which slot to use cannot take one.  Refused when no reservation is out.
 */
int blob_stat_reserved(unsigned slot, struct blob_info *out,
                       enum blob_hdr_reject *why);

/**
 * @brief  Where @p slot is, for a caller that is about to write it.
 *
 * Any of the out parameters may be NULL.  Fails the same way the read side does
 * when the table does not validate or the slot does not exist.
 */
int blob_slot_geometry(unsigned slot, uint32_t *base, uint32_t *payload_addr,
                       uint32_t *payload_max);

/**
 * @brief  Read the window WITHOUT taking a lease.
 *
 * [!] ONLY LEGAL FOR THE HOLDER OF THE WRITER RESERVATION, and it checks: with
 * no reservation out this refuses rather than reading, because a read of the
 * alias with neither a lease nor a reservation is issue #90 -- a window nobody
 * is holding up can be taken down underneath it, and a window that was never
 * brought up aliases one register across all 16 MB.
 *
 * It exists because a writer cannot take a lease: a reservation refuses them,
 * which is the whole point of it.
 */
int blob_read_reserved(uint32_t addr, void *buf, uint32_t len);

#ifdef __cplusplus
}
#endif

#endif /* BLOB_H */
