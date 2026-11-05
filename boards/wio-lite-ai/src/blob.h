/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Wio Lite AI ThreadX Shell Project
 */
/**
 * @file    blob.h
 * @brief   The asset store on the external NOR: its interval and its read side
 *          (owhinata/wio-lite-ai#10 / owhinata/wio-lite-ai#9 P2b; on the shared
 *          svc/blob parts since issue #193).
 *
 * Named slots above the `kv` partition, each holding one file that arrived from
 * the PC over YMODEM.  This exists because owhinata/wio-lite-ai#9 decided that the
 * TFLM model is supplied from the external NOR: a model does not fit in the
 * internal flash's spare room, and linking it in would spend an internal flash
 * erase cycle (~10k lifetime) on every model change.  Here a model is data, and
 * replacing one costs nothing but a transfer.
 *
 * THIS IS THE THIRD SIBLING ON THE SAME DEVICE, not a layer under or over them:
 * port/nor is the device, src/kv.c is the configuration store, and this is the
 * asset store.  It calls the nor_* primitives directly and knows nothing of
 * FlashDB or FAL -- the one thing it borrows from that side is the CRC-32
 * (port/nn/crc32_fdb.c serves svc/crc32.h's crc32_update() with FlashDB's).
 *
 *   0x000000 .. 0x100000   kv partition (FlashDB, owhinata/wio-lite-ai#37)
 *   0x100000 .. 0x800000   THIS: 13 slots in three sizes (src/blob_slots.c)
 *   0x800000 .. 0x1000000  unallocated (8 MB, no FAL entry and no slot)
 *
 * WHAT IS THIS BOARD'S AND WHAT IS SHARED (issue #193).  The header format, the
 * decisions a transfer makes, the name rules and the name lookup are
 * svc/blob/blob_state.c; the table's rules and lookups are svc/blob/blob_map.c;
 * the order of one `blob write` is svc/blob/blob_write.c.  This board provides
 * the table (src/blob_slots.c), the interval and erase unit below, the reads
 * and the name lookup over them (src/blob.c) and the write's operations
 * (src/blob_writer.c, for the shared command shell/cmds/cmd_blob.c; this
 * board's cmds/cmd_blob.c until issue #194).
 *
 * Slot layout.  The first 4 KB sector of a slot holds the header -- the shared
 * format's two 256 B pages, magic page first and written last -- and the rest
 * is the payload.  Giving the header a whole sector keeps the payload 4 KB
 * aligned and lets `blob erase` retire a blob with one ~45 ms sector erase.
 *
 * [!] A HEADER WRITTEN BEFORE ISSUE #193 READS `invalid`, BY CONSTRUCTION AND
 * NOT BY A VERSION NUMBER.  This board's old format put a 4-byte magic and the
 * body in one page; the shared format's magic page must be either erased or the
 * shared magic alone, so an old header -- finished or interrupted -- is refused
 * at that page before any body field is read (test/test_blob_slots.c tries both
 * at every slot base).  Three of the new slot bases (0x100000, 0x200000,
 * 0x300000) are where old headers sit on a board updated in place; one
 * `blob erase <slot>` clears each.
 *
 * LOCKING -- the NOR mutex is deliberately NOT held across a transfer.  A
 * `blob write` holds the device only inside each nor_* primitive (they take the
 * recursive device mutex themselves), never across the whole YMODEM session,
 * and its erase is one nor_erase() call per 64 KB block rather than one for the
 * slot, so the configuration store gets a turn between blocks.  Holding it
 * across would stop the configuration store for as long as a transfer takes
 * while buying nothing: the two ranges are disjoint (FAL range-checks within a
 * partition and the only partition is `kv` at [0, 1 MB), see
 * port/flashdb/fal_cfg.h), and so is the device STATE -- port/nor never writes a
 * status register, never uses erase suspend/resume and never enters
 * memory-mapped mode, and each WREN -> program/erase -> WIP wait completes
 * inside one lock.
 *
 * The honest cost in the other direction: a KV operation running while a
 * transfer is in flight delays a block ACK by however long it holds the device.
 * An ordinary `kv set` is one ~45 ms sector erase, but `kv format` erases a
 * megabyte -- ~2.4 s typical and tens of seconds at the driver's timeout budget.
 * That still fits inside lrzsz's patience, but "do not run `kv format` during a
 * transfer" is the real rule.
 *
 * What IS serialised is blob-mutating operations against each other, with the
 * busy flag below: YMODEM only runs on the USB CDC console, but a telnet session
 * can still type `blob erase` into the middle of a `blob write`, and `nn model
 * load` holds it from reading a header to reading the payload that header
 * describes.  The flag is the shared write's reservation (src/blob_writer.c).
 *
 * THREAD CONTEXT ONLY, after the scheduler is running: erases and programs
 * sleep.
 */
#ifndef APP_BLOB_H
#define APP_BLOB_H

#include <stdint.h>

#include "blob_map.h"    /* struct blob_slot, the shared table lookups      */
#include "blob_state.h"  /* struct blob_info, the states and their reasons  */
#include "nor_flash.h"   /* NOR_SECTOR_SIZE -- the erase unit of the store   */

#ifdef __cplusplus
extern "C" {
#endif

/* ---- the interval ---------------------------------------------------------- */

/**
 * The writable interval the slot table is carved from, and the erase unit a
 * slot's header occupies.  This board's facts, handed to blob_map_check() at
 * every entry point; svc/blob declares no boundary of its own.
 *
 * [!] THE END IS ALSO WHERE `nor test` DEFAULTS TO (cmds/cmd_nor.c asserts it),
 * so a bare `nor test` lands above every slot.
 */
#define BLOB_REGION_BASE   0x00100000u
#define BLOB_REGION_END    0x00800000u
#define BLOB_ERASE_UNIT    NOR_SECTOR_SIZE              /* 4 KB, header only */

/* ---- returns ------------------------------------------------------------- */

/* The shared results (svc/blob/blob_err.h, issue #194).  This board answers
 * PARAM, IO (the NOR refused a read, program or erase), STATE (the NOR device
 * is down), BUSY (another blob-mutating operation is running), EMPTY, CRC, MAP,
 * CANCEL and DUPLICATE; never FAULT. */
#include "blob_err.h"

/* ---- the table, checked against the interval ------------------------------ */

/**
 * Is the shipped table one this store may act on?  @p bad receives the
 * offending slot when the verdict names one.  Asked at every entry point below:
 * it costs a few dozen comparisons on a call that is about to touch the NOR.
 */
enum blob_map_verdict blob_check_map(unsigned *bad);

/**
 * Where @p slot is: its base, where its payload starts and how many payload
 * bytes it holds.  Any out pointer may be NULL.  BLOB_ERR_MAP / BLOB_ERR_PARAM
 * for a table that does not check out or a slot that does not exist.
 */
int blob_slot_geometry(unsigned slot, uint32_t *base, uint32_t *payload_addr,
                       uint32_t *payload_max);

/* ---- read side (no exclusion: see the locking note above) ----------------- */

/**
 * Decode @p slot's header into @p out (and why an INVALID one is, into @p why,
 * which may be NULL).  BLOB_OK whenever the header could be READ -- @p out->state
 * then says what was there, including "nothing" -- so a listing can tell "the
 * slot is empty" apart from "the flash did not answer" (BLOB_ERR_IO).
 */
int blob_stat(unsigned slot, struct blob_info *out, enum blob_hdr_reject *why);

/**
 * Read @p len payload bytes from @p off within @p slot.  Bounded by the payload
 * AREA, not by the stored length: a debugging hexdump may legitimately look past
 * the end of a blob.  A consumer that wants the blob itself takes the length
 * from blob_stat() first.
 *
 * One nor_read() transaction, no yield (about 90 ns a byte on the wire, so ~94 ms
 * for the largest payload); every higher-priority thread, the IWDG petter
 * included, still preempts it.
 */
int blob_read(unsigned slot, uint32_t off, void *buf, uint32_t len);

/**
 * Re-read @p slot's payload and check it against its stored CRC-32.  BLOB_OK on
 * a match, BLOB_ERR_CRC on a mismatch, BLOB_ERR_EMPTY when the slot holds no
 * header with a length and a CRC in it.  @p out receives the header it checked
 * against and @p computed what the flash adds up to; both may be NULL.
 *
 * INCOMPLETE slots are checked too: the body is there, so "the payload of an
 * interrupted transfer is intact as far as it got" is a question it can answer.
 */
int blob_verify(unsigned slot, struct blob_info *out, uint32_t *computed);

/**
 * Which slot holds the VALID blob called @p name, and its header.
 *
 * Reads every slot's header and hands the views to the shared
 * blob_resolve_name(), so the rule is the one grove-vision-ai-v2 resolves by:
 * exactly one VALID slot under the name is FOUND.  @p slot and @p out are
 * written only on BLOB_OK; @p out is the header read in this scan, not a second
 * read of the same slot.  @p why (may be NULL) receives the lookup's answer
 * whenever the scan completed.
 *
 * @return BLOB_OK; BLOB_ERR_EMPTY when no VALID slot carries the name;
 *         BLOB_ERR_DUPLICATE when more than one does -- its own answer, NOT
 *         "not found" (issue #93): flash outlives firmware and a raw `nor write`
 *         can put anything anywhere, and picking the first would load whichever
 *         copy the table reaches first; BLOB_ERR_MAP for a table that does not
 *         check out; or a read failure from blob_stat()
 *
 * [!] THE ANSWER IS ONLY STILL TRUE UNDER THE BUSY FLAG.  Nothing here takes
 * it: `nn model load` holds it from this scan to the payload read, which is
 * what keeps a `blob write` or `blob erase` from changing the slot in between.
 */
int blob_find(const char *name, unsigned *slot, struct blob_info *out,
              enum blob_lookup *why);

/* ---- mutating side (all of it under blob_busy_acquire) -------------------- */

/**
 * Serialise blob-mutating operations against each other.  A plain test-and-set
 * under a brief PRIMASK critical section -- interrupt-safe, thread-agnostic and
 * needing no initialisation, the same idiom as the NN session guard
 * (svc/nn/nn.c).  BLOB_OK when acquired, BLOB_ERR_BUSY otherwise.
 */
int  blob_busy_acquire(void);
void blob_busy_release(void);

/**
 * Retire the blob in @p slot: erase its header sector and nothing else (one
 * ~45 ms sector erase).  The payload stays in the flash until something writes
 * over it, and `blob list` stops showing the blob at once.
 */
int  blob_erase_header(unsigned slot);

/**
 * Called before each 64 KB block blob_erase_span() erases, with what is done so
 * far.  Non-zero stops the erase before that block.
 */
typedef int (*blob_erase_tick_fn)(void *ctx, uint32_t done, uint32_t total);

/**
 * Erase [@p addr, @p addr + @p len) one 64 KB block per nor_erase() call, so the
 * NOR mutex is given back between blocks (the kv store gets a turn) and @p tick
 * can stop it.  The range must lie inside the interval and be whole erase units.
 *
 * @param done       receives the bytes whose erase reported OK
 * @param cancelled  receives non-zero when @p tick stopped it
 * @return BLOB_OK, BLOB_ERR_PARAM / _STATE before anything is erased, BLOB_ERR_IO
 *         when a block did not erase, or BLOB_ERR_CANCEL when @p tick stopped it
 */
int  blob_erase_span(uint32_t addr, uint32_t len, blob_erase_tick_fn tick,
                     void *ctx, uint32_t *done, int *cancelled);

#ifdef __cplusplus
}
#endif

#endif /* APP_BLOB_H */
