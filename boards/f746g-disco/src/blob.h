/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    blob.h
 * @brief   The asset store on the QSPI NOR: its interval and its read side
 *          (issue #194, on the shared svc/blob parts).
 *
 * Named slots on the on-board 16 MB QSPI NOR (N25Q128A), each holding one file
 * that arrives from the PC over YMODEM.  Until issue #194 this board read its
 * models from files on the microSD and the QSPI NOR was a FileX-on-LevelX
 * volume; the store is the same shape as wio-lite-ai's (src/blob.h there), so
 * `blob write` and `nn model load --name` mean the same thing on both.
 *
 *   0x000000 .. 0x100000   reserved (1 MB, no slot; `qspi erase/test` reach it)
 *   0x100000 .. 0x800000   THIS: 13 slots in three sizes (src/blob_slots.c)
 *   0x800000 .. 0x1000000  unallocated (8 MB, no slot)
 *
 * WHAT IS THIS BOARD'S AND WHAT IS SHARED.  The header format, the decisions a
 * transfer makes, the name rules and the name lookup are svc/blob/blob_state.c;
 * the table's rules and lookups are svc/blob/blob_map.c; the order of one `blob
 * write` is svc/blob/blob_write.c.  This board provides the table
 * (src/blob_slots.c), the interval and erase unit below, and the reads, the
 * erases and the name lookup over them (src/blob.c).  Nothing here takes a
 * `struct cli_instance`: printing, waiting and cancelling are the caller's.
 *
 * Slot layout.  The first 4 KB subsector of a slot holds the header -- the
 * shared format's two 256 B pages, magic page first and written last -- and the
 * rest is the payload.  Giving the header a whole subsector keeps the payload
 * 4 KB aligned and lets `blob erase` retire a blob with one subsector erase.
 *
 * LOCKING.  port/qspi/qspi_flash.c holds its own mutex for each call, from the
 * write-enable to the end of the busy wait, so every call below is atomic on
 * the bus and the device lock is never held across a transfer.  What IS
 * serialised is the operations that change or depend on the store, against
 * each other, with the busy flag below: two consoles (the VCP and telnet), a
 * background job, `qspi erase` / `qspi test` (cmds/cmd_qspi.c takes the flag
 * too), `blob write` / `blob erase` (src/blob_writer.c's reservation), and
 * `nn model load --name` (port/nn/nn_svc_f746_model.c, from the name lookup
 * to the end of the payload read).  The flag is taken by the CALLER for the
 * span only it knows.
 *
 * [!] AND THE PUBLIC READS REFUSE WHILE IT IS HELD (issue #194), unlike
 * wio-lite-ai's: blob_stat(), blob_read() and blob_verify() answer
 * BLOB_ERR_BUSY rather than describe flash a raw `qspi erase` / `qspi test`
 * or a write is half-way through changing.  They only look at the flag and
 * never take it, so two readers do not refuse each other.  What runs under the
 * flag -- the write's own stat (blob_stat_reserved), blob_find() and the nn
 * reader's blob_read_reserved() -- does not look.
 *
 * THREAD CONTEXT ONLY, after the scheduler is running: the QSPI driver sleeps
 * while it waits for an erase or a program.
 */
#ifndef F746_BLOB_H
#define F746_BLOB_H

#include <stdint.h>

#include "blob_map.h"    /* struct blob_slot, the shared table lookups      */
#include "blob_state.h"  /* struct blob_info, the states and their reasons  */
#include "qspi_flash.h"  /* the erase units of the part                     */

#ifdef __cplusplus
extern "C" {
#endif

/* ---- the interval ---------------------------------------------------------- */

/**
 * The writable interval the slot table is carved from, and the erase unit a
 * slot's header occupies.  This board's facts, handed to blob_map_check() at
 * every entry point; svc/blob declares no boundary of its own.  The same
 * numbers as wio-lite-ai's (its interval starts above its kv partition; this
 * one keeps the first megabyte free to match it).
 */
#define BLOB_REGION_BASE   0x00100000u
#define BLOB_REGION_END    0x00800000u
#define BLOB_ERASE_UNIT    QSPI_FLASH_SUBSECTOR_SIZE    /* 4 KB, header only */

/**
 * The longest single qspi_flash_read() blob_read() and blob_verify() issue.
 *
 * [!] HAL_QSPI_Receive()'s timeout covers the WHOLE call, not one byte, so a
 * payload read as one call is one long wait under the driver's mutex.  Split
 * into pieces of one erase sector, every piece is short against that timeout
 * even on the 1-line fallback, and the driver's mutex is given back between
 * them.  A thread that preempts a piece part way is harmless: QUADSPI stops
 * clocking when its FIFO is full and waits (RM0385 14.3.5).
 */
#define BLOB_READ_CHUNK    QSPI_FLASH_SECTOR_SIZE       /* 64 KB */

/* ---- returns ------------------------------------------------------------- */

/* The shared results (svc/blob/blob_err.h, issue #194).  This board answers
 * PARAM, IO (the QSPI refused a read, program or erase), STATE (the QSPI driver
 * did not come up), BUSY (another operation on the store is running), EMPTY,
 * CRC, MAP, CANCEL and DUPLICATE; never FAULT. */
#include "blob_err.h"

/* ---- the table, checked against the interval ------------------------------ */

/**
 * Is the shipped table one this store may act on?  @p bad receives the
 * offending slot when the verdict names one.  Asked at every entry point below.
 */
enum blob_map_verdict blob_check_map(unsigned *bad);

/**
 * Where @p slot is: its base, where its payload starts and how many payload
 * bytes it holds.  Any out pointer may be NULL.  BLOB_ERR_MAP / BLOB_ERR_PARAM
 * for a table that does not check out or a slot that does not exist.
 */
int blob_slot_geometry(unsigned slot, uint32_t *base, uint32_t *payload_addr,
                       uint32_t *payload_max);

/* ---- read side (refused while the busy flag is held; see above) ----------- */

/**
 * Decode @p slot's header into @p out (and why an INVALID one is, into @p why,
 * which may be NULL).  BLOB_OK whenever the header could be READ -- @p out->state
 * then says what was there, including "nothing" -- so a listing can tell "the
 * slot is empty" apart from "the flash did not answer" (BLOB_ERR_IO).
 */
int blob_stat(unsigned slot, struct blob_info *out, enum blob_hdr_reject *why);

/** blob_stat() for a caller that HOLDS the busy flag: the same read, with no
 *  look at the flag (a `blob write` choosing its slot under its reservation). */
int blob_stat_reserved(unsigned slot, struct blob_info *out);

/**
 * Read @p len payload bytes from @p off within @p slot, in pieces of at most
 * BLOB_READ_CHUNK.  Bounded by the payload AREA, not by the stored length; a
 * consumer that wants the blob itself takes the length from blob_stat() first.
 */
int blob_read(unsigned slot, uint32_t off, void *buf, uint32_t len);

/** blob_read() for a caller that HOLDS the busy flag (`nn model load`, from
 *  its name lookup to the end of its payload read, issue #194): the same read,
 *  with no look at the flag. */
int blob_read_reserved(unsigned slot, uint32_t off, void *buf, uint32_t len);

/**
 * Re-read @p slot's payload and check it against its stored CRC-32.  BLOB_OK on
 * a match, BLOB_ERR_CRC on a mismatch, BLOB_ERR_EMPTY when the slot holds no
 * header with a length and a CRC in it.  @p out receives the header it checked
 * against and @p computed what the flash adds up to; both may be NULL.
 * INCOMPLETE slots are checked too.
 */
int blob_verify(unsigned slot, struct blob_info *out, uint32_t *computed);

/**
 * Which slot holds the VALID blob called @p name, and its header -- through the
 * shared blob_resolve_name(), so the rule is the other boards': exactly one
 * VALID slot under the name is FOUND.  @p slot and @p out are written only on
 * BLOB_OK; @p why (may be NULL) receives the lookup's answer whenever the scan
 * completed.
 *
 * @return BLOB_OK; BLOB_ERR_EMPTY when no VALID slot carries the name;
 *         BLOB_ERR_DUPLICATE when more than one does (its own answer, not "not
 *         found"); BLOB_ERR_MAP for a table that does not check out; or a read
 *         failure from blob_stat()
 *
 * [!] THE ANSWER IS ONLY STILL TRUE UNDER THE BUSY FLAG.  Nothing here takes
 * it: the caller holds it from this scan to the payload read.
 */
int blob_find(const char *name, unsigned *slot, struct blob_info *out,
              enum blob_lookup *why);

/* ---- mutating side (all of it under blob_busy_acquire) -------------------- */

/**
 * Serialise operations on the store against each other.  A plain test-and-set
 * under a brief PRIMASK critical section -- interrupt-safe, thread-agnostic and
 * needing no initialisation.  BLOB_OK when acquired, BLOB_ERR_BUSY otherwise.
 */
int  blob_busy_acquire(void);
void blob_busy_release(void);

/**
 * Retire the blob in @p slot: erase its header subsector and nothing else (one
 * 4 KB erase).  The payload stays in the flash until something writes over it.
 */
int  blob_erase_header(unsigned slot);

/**
 * Called before each erase blob_erase_span() issues, with what is done so far.
 * Non-zero stops the erase before that unit.  The caller's: it is how a console
 * command looks at its Ctrl+C without this file knowing about consoles.
 */
typedef int (*blob_erase_tick_fn)(void *ctx, uint32_t done, uint32_t total);

/**
 * Erase [@p addr, @p addr + @p len): one 64 KB sector erase per call where the
 * range covers a whole sector, a 4 KB subsector erase otherwise, so the driver's
 * mutex is given back between them and @p tick can stop it.  The range must lie
 * inside the interval and be whole 4 KB units.
 *
 * @param done       receives the bytes whose erase reported OK
 * @param cancelled  receives non-zero when @p tick stopped it
 * @return BLOB_OK, BLOB_ERR_PARAM / _STATE before anything is erased, BLOB_ERR_IO
 *         when an erase failed, or BLOB_ERR_CANCEL when @p tick stopped it
 */
int  blob_erase_span(uint32_t addr, uint32_t len, blob_erase_tick_fn tick,
                     void *ctx, uint32_t *done, int *cancelled);

#ifdef __cplusplus
}
#endif

#endif /* F746_BLOB_H */
