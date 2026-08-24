/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    nor_write.h
 * @brief   The bounded NOR write transaction (issue #88 Part C).
 *
 * This is the only translation unit in the firmware allowed to call the
 * vendor's erase and program entry points, and cmake/check_nor_seam.py is what
 * enforces that -- board.cmake names this object as the seam's one authorised
 * caller.  What the seam does and does not prove is in nor_seam.h; read that
 * first, because the summary is that this is defence in depth over ONE path and
 * not a proof that the firmware cannot write this flash another way.
 *
 * A TRANSACTION, NOT A SEQUENCE OF CALLS
 *
 * The vendor refuses erase and program while the memory-mapped window is up
 * (both return -28), so a write has to take the window DOWN -- and the window
 * is what every reader of this part uses.  So one call here is the whole thing:
 *
 *   1. claim the part: state XIP, no reader leases, publish NOR_ST_WRITING
 *      under the same critical section that read them (nor_state.h)
 *   2. drop the window, and establish that it really went down by reading the
 *      SCU back -- the vendor's own enable_XIP discards every return value it
 *      collects
 *   3. re-read the JEDEC id.  See below; this is a canary, not decoration
 *   4. the operation, one erase unit or one program page at a time
 *   5. bring the window back, verify it and re-probe it
 *   6. read the array back through the window and compare
 *   7. commit: NOR_ST_XIP if everything held, terminal NOR_ST_FAULTED if not
 *
 * Steps 5 and 7 run whatever happened in step 4.  A transaction that gave up
 * part-way still owes every reader a window.
 *
 * [!] STEP 3 IS THE CANARY, AND IT IS THE ONE STEP HERE THAT IS ABOUT LIVENESS.
 * The vendor's write path is built on hx_lib_spi_eeprom_DMA_send_recv, which
 * spins on a flag a completion interrupt clears and has no timeout -- waitWIP,
 * set_quad_mode, clear_write_protect and readWEL are all made of it.  A
 * throwaway branch in August 2026 hung the console exactly there, on the first
 * such call after the window came down, and it could not be recovered without a
 * reset.  Nothing in this file can bound a spin inside a prebuilt archive.  What
 * it can do is put the FIRST one on a call that is a plain read of the id --
 * no write-enable latch, no opcode that changes anything -- so that a part which
 * has stopped answering costs a reset instead of a half-erased region.  It also
 * turns "the transport still works with the window down" from an assumption
 * into a checked precondition, and compares the answer with what bring-up saw.
 *
 * [!] AND THE ARRAY IS NOT THE ONLY THING A TRANSACTION WRITES.  Taking the
 * window down calls the vendor's set_quad_mode with the quad bit CLEARED and
 * bringing it back sets it again, so every transaction -- including the
 * do-nothing one below -- writes the NOR's non-volatile status register twice
 * (verified by disassembly; the helper reads it first and skips the write when
 * the bit already matches, which is why it is twice and not more).  An erase
 * also goes through clear_write_protect, which does the same for the block
 * protect bits.  The array is untouched by all of that, so nothing can be lost
 * -- but it is not free, and on a part whose endurance is not documented (the
 * fitted die is a Zbit ZB25LQ128C, not the W25Q128JW the schematic names --
 * issue #89) that is worth knowing before putting one of these in a loop.
 *
 * If the board resets between steps 2 and 5 the part is left with its quad bit
 * clear.  That is the factory state and the state the 2nd bootloader leaves
 * behind after every flash, so it boots.
 *
 * [!] WHAT A RETURN OF ZERO FROM THE VENDOR MEANS: nothing.
 * hx_lib_qspi_eeprom_write ends in `movs r0, #0` and hx_lib_qspi_eeprom_erase_
 * sector returns the write-protect helper's result, both discarding every
 * transfer result in between.  So the only evidence an operation happened is
 * reading the array back, which is step 6 and is not optional.  The negatives
 * they CAN return are still worth having -- -28 for a window that did not go
 * down and -50 for a write-enable latch that never set -- because those mean
 * the operation was refused before anything went on the wire.
 */
#ifndef NOR_WRITE_H
#define NOR_WRITE_H

#include <stdint.h>

#include "nor_span.h"

#ifdef __cplusplus
extern "C" {
#endif

/** How a transaction ended. */
enum nor_write_status {
	NOR_WRITE_OK = 0,
	/** The part is not this caller's to take: a reader lease is out, a
	 *  bring-up is in flight, another writer holds it -- or the window has
	 *  never been brought up, which is a reader's errand (nor_state.h). */
	NOR_WRITE_BUSY,
	/** The request names flash this port may not write.  Nothing was
	 *  claimed and no hardware was touched. */
	NOR_WRITE_REFUSED,
	/** The window came down and the part did not answer the JEDEC read.
	 *  Nothing was sent; the window was brought back and verified. */
	NOR_WRITE_NO_TRANSPORT,
	/** The vendor refused part-way.  What it DID accept was read back and
	 *  is in the array; nothing is claimed about the rest of the span. */
	NOR_WRITE_INCOMPLETE,
	/** Terminal.  Either the port was already faulted, or this transaction
	 *  could not take the window down, could not bring it back, could not
	 *  re-probe it, or read back something other than what it wrote.  See
	 *  nor_write.c for why the last of those has to be terminal. */
	NOR_WRITE_FAULTED,
};

/** Short name, for the console and the log. */
const char *nor_write_status_name(enum nor_write_status s);

/** What one transaction did.  Every field is filled in on every outcome. */
struct nor_write_report {
	/** The footprint acted on.  For an erase this is the caller's range
	 *  rounded out to whole erase units -- what it LOSES, not what it
	 *  named.  Zero length for a cycle, which acts on nothing. */
	struct nor_span span;
	/** Bytes of @ref span the vendor accepted before it stopped, and bytes
	 *  read back and compared.  On NOR_WRITE_OK both equal span.len. */
	uint32_t done;
	uint32_t verified;
	/** The first offset that did not read back, and the two bytes there.
	 *  [!] Read them only when @ref bad_valid is set.  A fault has several
	 *  causes and only one of them fills these in; "both bytes are zero"
	 *  is not a test, because zero is a byte the array can hold. */
	uint32_t bad_off;
	uint8_t  bad_got;
	uint8_t  bad_want;
	uint8_t  bad_valid;
	/** The JEDEC id read back with the window down, and whether it answered
	 *  and matched what bring-up recorded. */
	uint8_t  jedec[3];
	uint8_t  jedec_ok;
	/** The last value a vendor entry point returned, or 0 if none refused.
	 *  NOT evidence of success -- see the file comment. */
	int32_t  vendor_rc;
	/** Why it failed, or NULL.  Points at a string literal. */
	const char *fail;
};

/**
 * @brief  Take the window down and bring it back, touching nothing else.
 *
 * The transaction above with step 4 left out.  It exists because everything
 * before and after the operation is the risky part -- the window transition,
 * the transport, the re-probe -- and this is the only way to exercise it
 * without putting an erase on the wire.
 *
 * [!] "Touching nothing" is about the ARRAY.  It still writes the status
 * register twice; see the file comment.
 */
enum nor_write_status nor_write_cycle(struct nor_write_report *r);

/**
 * @brief  Erase the flash covering [addr, addr+len).
 *
 * @param addr  first flash offset to lose
 * @param len   how many bytes the caller cares about; the footprint is this
 *              range rounded out to whole erase units, and @p r carries it
 *
 * Every unit in the footprint must lie inside the seam's writable interval, so
 * a request that would round outside it is refused rather than clipped.
 */
enum nor_write_status nor_write_erase(uint32_t addr, uint32_t len,
                                      struct nor_write_report *r);

/**
 * @brief  Program @p len bytes of @p data at @p addr.
 *
 * @p data is copied a page at a time into a buffer this file owns before it
 * reaches the vendor, so the caller's buffer is neither handed to a prebuilt
 * archive that takes it non-const nor read again after the fact: the read-back
 * in step 6 compares the array against what the caller still holds.
 *
 * Programming does not erase.  Bits only go from 1 to 0, so writing over
 * something that is not erased leaves the AND of the two -- which the read-back
 * will catch, terminally.  Erase first.
 */
enum nor_write_status nor_write_program(uint32_t addr, const void *data,
                                        uint32_t len,
                                        struct nor_write_report *r);

#ifdef __cplusplus
}
#endif

#endif /* NOR_WRITE_H */
