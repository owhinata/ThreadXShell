/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    nor_seam.h
 * @brief   The one door to the vendor's NOR write path (issue #88).
 *
 * The prebuilt lib_spi_eeprom.a can erase and program the external NOR, and
 * that flash holds the bootloader, the firmware image and the bootloader's slot
 * header.  This port needs a WRITE path for issue #49's blob, so the vendor
 * entry points cannot simply stay barred -- they have to be reachable through
 * exactly one place that bounds them.
 *
 * That place is here.  board.cmake redirects the four inner entry points with
 * -Wl,--wrap:
 *
 *     hx_lib_qspi_eeprom_erase_sector   -> bounded, then __real_
 *     hx_lib_qspi_eeprom_write          -> bounded, then __real_
 *     hx_lib_qspi_eeprom_erase_all      -> refused; no __real_ reference exists
 *     hx_lib_qspi_eeprom_word_write     -> refused; no __real_ reference exists
 *
 * Same shape as the vendor timer seam (issue #30), with one difference that
 * matters: THIS seam calls __real_.  The timer seam's claim is "no vendor timer
 * code is in the image at all", which a symbol check can settle.  Here the
 * vendor code IS in the image and the claim is about who may reach it, which is
 * a claim about the LINK -- so cmake/check_nor_seam.py audits relocations
 * against the linker's own map rather than reading the finished ELF.
 *
 * [!] WHAT THIS DOES NOT PROVE (issue #87, restated because it is the thing a
 * reader is most likely to get wrong).  It does NOT prove the firmware cannot
 * write this flash by another route.  The read/XIP path already links
 * hx_drv_spi_mst_get_dev, hx_drv_dmac_get_dev and the vendor's DMA_send /
 * set_DMA_config / waitWIP / setWriteEnable helpers, and those cannot be barred
 * because the read path needs them: a first-party translation unit can assemble
 * WREN plus an arbitrary opcode without naming one symbol any gate here looks
 * at.  Direct MMIO is beyond reach of any of this.  The seam is defence in
 * depth over the ONE path the vendor library offers, not a capability proof.
 *
 * [!] AND "BOUNDED" MEANS SPATIALLY.  hx_lib_spi_eeprom_waitWIP is a polling
 * loop with no timeout, and both permitted entry points call it, so a part that
 * never drops its WIP bit hangs the calling thread.  That is inside a prebuilt
 * archive and cannot be bounded from here.
 *
 * THE DECISIONS ARE PURE FUNCTIONS, and that is not a style preference: the
 * refusals cannot be produced on hardware.  Every caller that reaches the
 * wrappers is first-party code written to satisfy them, so the paths that
 * matter -- an address one byte past the writable interval, an erase unit this
 * die has never been asked for -- exist only in test/test_nor_seam.c.
 */
#ifndef GROVE_NOR_SEAM_H
#define GROVE_NOR_SEAM_H

#include <stdint.h>

#include "nor_state.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The interval a write may touch, and the unit an erase destroys.  All three
 * come from board.cmake, which derives them from cmake/flash_geometry.cmake --
 * the measured bootloader geometry (issue #85) -- so the rule the firmware
 * enforces and the layout cmake/check_flash_partitions.py checks are the same
 * numbers.  check_nor_seam.py asserts the LITERALS independently, out of the
 * linked image, so that a build which lost the definitions cannot enforce a
 * different interval than the one the layout was checked against.
 *
 * [!] NO FALLBACK DEFAULTS, for the reason cmd_nor.c states: a default is a
 * second declaration of a layout only one file may declare, and the wrong one
 * refuses nothing visibly. */
#if !defined(NOR_PART_FW_END) || !defined(NOR_PART_BLOB_END) || \
    !defined(NOR_ERASE_GRAN)
#error "NOR_PART_FW_END / NOR_PART_BLOB_END / NOR_ERASE_GRAN must come from board.cmake"
#endif

/* [!] blob AND NOT blob-tail.  The two runs are one reservation split by the
 * models that sit between them (board.cmake), and #49 Step 4 merges them --
 * but until it does, the tail is separated from blob by flash that `nn open
 * cls|det` reads, and a writable interval spanning it would span those. */
#define NOR_WRITABLE_LO   ((uint32_t)(NOR_PART_FW_END))
#define NOR_WRITABLE_HI   ((uint32_t)(NOR_PART_BLOB_END))   /* exclusive */

/* [!] MEASURED, NOT CHOSEN (issue #88).  The resident 2nd bootloader's range
 * eraser walks in 4 KB steps and passes erase unit 0; 0x52, 0xD8 and chip erase
 * are never issued on this part.  So 4 KB is what this die has been seen to
 * do -- the seam refuses the other two units for that reason and not because
 * they would be too coarse. */
#define NOR_ERASE_UNIT    ((uint32_t)(NOR_ERASE_GRAN))

/**
 * @brief  The interval and unit the seam was COMPILED to enforce.
 *
 * [!] THIS OBJECT EXISTS TO BE READ FROM OUTSIDE THE FIRMWARE.  The decision
 * functions below are written against these three fields, and
 * cmake/check_nor_seam.py reads the same twelve bytes back out of the linked
 * image and compares them with the layout board.cmake declared.  Without that,
 * a build that had picked up different partition definitions would enforce a
 * different interval than check_flash_partitions.py checked the layout of, and
 * nothing would say so.
 *
 * The literals cannot be checked in the INSTRUCTIONS instead: at -Os the
 * compiler rewrites `lo <= a && a < hi` into `a - lo <u hi - lo`, so the
 * interval's end never appears, and the constants that do appear are whatever
 * algebra the optimiser chose that day.  A record in .rodata is the same fact
 * in a form that does not depend on the optimiser.
 *
 * `nor info` prints it, so it is not a hook that exists only for a test.
 */
struct nor_seam_limits {
	uint32_t lo;    /**< first writable flash offset            */
	uint32_t hi;    /**< one past the last; exclusive           */
	uint32_t unit;  /**< bytes one permitted erase destroys     */
};
extern const struct nor_seam_limits nor_seam_limits;

/** What the seam decided about one call. */
enum nor_seam_verdict {
	NOR_SEAM_GO = 0,        /**< bounded; the vendor may be called       */
	NOR_SEAM_NO_TRANSACTION,/**< the port is not inside a write          */
	NOR_SEAM_NO_ADDRESS,    /**< the operation names no address at all   */
	NOR_SEAM_NO_PATH,       /**< an entry point this port does not offer */
	NOR_SEAM_BAD_UNIT,      /**< an erase unit this die has not shown    */
	NOR_SEAM_BAD_MODE,      /**< word_switch other than 0                */
	NOR_SEAM_NO_BUFFER,     /**< NULL payload                            */
	NOR_SEAM_EMPTY,         /**< zero length                             */
	NOR_SEAM_UNALIGNED,     /**< erase address off the erase unit        */
	NOR_SEAM_OUTSIDE,       /**< not wholly inside the writable interval */
};

/** Short name for a verdict, for the refusal log and the host test. */
const char *nor_seam_verdict_name(enum nor_seam_verdict v);

/**
 * @brief  May this erase reach the vendor?
 *
 * @param st    the NOR port's lifecycle state
 * @param addr  flash offset to erase
 * @param unit  the vendor's FLASH_ERASE_SIZE_E, widened
 *
 * [!] ONLY NOR_ST_WRITING MAY ACT, and the states are ENUMERATED rather than
 * excluded.  "not XIP" would let an erase run from OFF, with no QSPI master
 * open; "not FAULTED" would let one run underneath a reader's live window.
 * Same rule the rest of port/nor/ is written to (nor_state.h).
 *
 * [!] THE BOUNDS ARE SUBTRACTION-BASED.  addr + NOR_ERASE_UNIT is never formed
 * before the address is known to be inside the interval, so an offset near
 * 2^32 cannot wrap into a range that looks contained.
 */
enum nor_seam_verdict nor_seam_check_erase(enum nor_state st, uint32_t addr,
                                           uint32_t unit);

/**
 * @brief  May this program reach the vendor?
 *
 * @param st           the NOR port's lifecycle state
 * @param addr         flash offset to program
 * @param data         payload
 * @param len          payload length in bytes
 * @param word_switch  the vendor's word_switch argument
 *
 * Same rules as the erase, plus: the payload must exist, the length must not be
 * zero, and word_switch must be 0.  No alignment is required -- the caller
 * splits into the part's 256 B program pages, and a program that does not span
 * the interval's end is in bounds wherever it starts.
 */
enum nor_seam_verdict nor_seam_check_write(enum nor_state st, uint32_t addr,
                                           const void *data, uint32_t len,
                                           uint32_t word_switch);

/**
 * @brief  What a refused wrapper returns to its caller.
 *
 * [!] NOT ZERO AND NOT THE VENDOR'S OWN CODES.  hx_lib_qspi_eeprom_write
 * returns a hard-coded 0 and hx_lib_qspi_eeprom_erase_sector returns the write-
 * protect helper's result, discarding everything after it -- so a caller cannot
 * tell a vendor success from a vendor failure, and the only proof an operation
 * happened is reading the array back.  A refusal from HERE is different: it is
 * the one negative on this path that means something definite, namely that
 * nothing was sent to the part.
 */
#define NOR_SEAM_REFUSED   (-1000)

#ifdef __cplusplus
}
#endif

#endif /* GROVE_NOR_SEAM_H */
