/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    nor_seam.c
 * @brief   The bounded door to the vendor's NOR write path (issue #88).
 *
 * The four wrappers board.cmake redirects with -Wl,--wrap, and the pure
 * decisions behind them.  What this seam is for, and what it deliberately does
 * not prove, is in nor_seam.h.
 *
 * TWO OF THE FOUR NEVER MENTION __real_, AND THAT IS THE POINT
 *
 * hx_lib_qspi_eeprom_erase_all is a chip erase: it names no address, so there
 * is no interval to check it against and no refusal that could be conditional.
 * hx_lib_qspi_eeprom_word_write is the vendor's word-at-a-time programmer,
 * which this port has no use for and which would be a second write path to
 * bound.  Both are refused unconditionally -- and because their wrappers hold
 * no __real_ reference, --gc-sections drops the vendor's implementations out of
 * the link entirely, so check_placement_budget.py's absence rule keeps covering
 * them exactly as it did before this seam existed.
 *
 * [!] THE WRAPPERS RUN ON THE WRITER'S THREAD, not in an interrupt -- unlike
 * the timer seam, whose stop wrapper is reached from a vendor ISR callback and
 * therefore may not log.  These may, and do: a refusal is a bug in first-party
 * code (every caller is written to satisfy these rules), so it has to leave a
 * record rather than only a return value the caller might discard.
 */
#include "nor_seam.h"

#include <stddef.h>

#include "nor_flash.h"              /* nor_lifecycle_state() */
#include "qspi_eeprom_interface.h"  /* the vendor prototypes, for the ABI   */

#define LOG_TAG "nor"
#include "log.h"

/* The writable interval must be whole erase units, or an in-bounds erase could
 * destroy bytes outside it -- the erase destroys a unit, not a byte.  Both
 * edges come from board.cmake and both are 4 KB aligned today; this is what
 * stops a future partition edge from quietly breaking that. */
_Static_assert(NOR_ERASE_UNIT != 0u, "erase unit must not be zero");
_Static_assert((NOR_ERASE_UNIT & (NOR_ERASE_UNIT - 1u)) == 0u,
               "erase unit must be a power of two");
_Static_assert(NOR_WRITABLE_LO % NOR_ERASE_UNIT == 0u,
               "the writable interval must start on an erase unit");
_Static_assert(NOR_WRITABLE_HI % NOR_ERASE_UNIT == 0u,
               "the writable interval must end on an erase unit");
_Static_assert(NOR_WRITABLE_LO < NOR_WRITABLE_HI,
               "the writable interval must be non-empty");

/* [!] ONE DECLARATION, TWO READERS.  The decisions below are written against
 * these fields and not against the macros, so that the record the gate reads
 * out of .rodata and the arithmetic the firmware performs cannot be different
 * numbers.  Within this translation unit the compiler folds them back to the
 * constants -- the emitted code is the same as before -- but the object is
 * still there for cmd_nor.c and the gate, because nothing here is allowed to
 * be built with LTO. */
const struct nor_seam_limits nor_seam_limits = {
	NOR_WRITABLE_LO, NOR_WRITABLE_HI, NOR_ERASE_UNIT,
};

/* The vendor's FLASH_ERASE_SIZE_E enumerator this port permits.  Named through
 * the SDK's own enum rather than written as 0, so that what is permitted is the
 * 4 KB sector erase and not a number.
 *
 * [!] AND THE NUMBER IS ASSERTED ANYWAY, because the measurement behind it is
 * about the number: the resident 2nd bootloader passes 0 to this argument and
 * 0x20 goes on the wire (issue #88, by disassembly).  If a future SDK pin
 * renumbered the enum, that observation would no longer be about FLASH_SECTOR
 * and somebody would have to measure again -- which is a build failure here
 * rather than a quietly different opcode. */
#define NOR_SEAM_UNIT_4K   ((uint32_t)FLASH_SECTOR)
_Static_assert(NOR_SEAM_UNIT_4K == 0u,
               "the erase unit measured on this die is enumerator 0");

const char *nor_seam_verdict_name(enum nor_seam_verdict v)
{
	switch (v) {
	case NOR_SEAM_GO:             return "ok";
	case NOR_SEAM_NO_TRANSACTION: return "no write transaction is open";
	case NOR_SEAM_NO_ADDRESS:     return "the operation names no address";
	case NOR_SEAM_NO_PATH:        return "this port offers no such write";
	case NOR_SEAM_BAD_UNIT:       return "erase unit not measured on this die";
	case NOR_SEAM_BAD_MODE:       return "word_switch must be 0";
	case NOR_SEAM_NO_BUFFER:      return "no payload";
	case NOR_SEAM_EMPTY:          return "zero length";
	case NOR_SEAM_UNALIGNED:      return "address is not on an erase unit";
	case NOR_SEAM_OUTSIDE:        return "outside the writable interval";
	default:                      return "?";
	}
}

/* [!] ENUMERATED, not excluded.  See nor_seam.h. */
static int in_transaction(enum nor_state st)
{
	switch (st) {
	case NOR_ST_WRITING:
		return 1;
	case NOR_ST_OFF:
	case NOR_ST_ENABLING:
	case NOR_ST_XIP:
	case NOR_ST_FAULTED:
	default:
		return 0;
	}
}

enum nor_seam_verdict nor_seam_check_erase(enum nor_state st, uint32_t addr,
                                           uint32_t unit)
{
	if (!in_transaction(st))
		return NOR_SEAM_NO_TRANSACTION;
	if (unit != NOR_SEAM_UNIT_4K)
		return NOR_SEAM_BAD_UNIT;
	if (addr % nor_seam_limits.unit != 0u)
		return NOR_SEAM_UNALIGNED;
	/* Subtraction only, and in this order: addr is proved to be inside the
	 * interval before anything is added to it. */
	if (addr < nor_seam_limits.lo || addr >= nor_seam_limits.hi)
		return NOR_SEAM_OUTSIDE;
	if (nor_seam_limits.unit > nor_seam_limits.hi - addr)
		return NOR_SEAM_OUTSIDE;
	return NOR_SEAM_GO;
}

enum nor_seam_verdict nor_seam_check_write(enum nor_state st, uint32_t addr,
                                           const void *data, uint32_t len,
                                           uint32_t word_switch)
{
	if (!in_transaction(st))
		return NOR_SEAM_NO_TRANSACTION;
	if (word_switch != 0u)
		return NOR_SEAM_BAD_MODE;
	if (data == NULL)
		return NOR_SEAM_NO_BUFFER;
	if (len == 0u)
		return NOR_SEAM_EMPTY;
	/* addr == NOR_WRITABLE_HI is allowed to reach here and is then refused by
	 * the length test, because len is never zero: an empty range at the
	 * exclusive end has no bytes to be in or out of. */
	if (addr < nor_seam_limits.lo || addr > nor_seam_limits.hi)
		return NOR_SEAM_OUTSIDE;
	if (len > nor_seam_limits.hi - addr)
		return NOR_SEAM_OUTSIDE;
	/* [!] WORDS, NOT BYTES (issue #92).  The transport takes 32-bit words with
	 * their bytes reversed, so the writer reverses what it sends -- and a
	 * transfer that does not start and end on a word has no defined byte
	 * order.  nor_span_program() refuses the address for the same reason; this
	 * is the door saying it too, because the door is what a caller that
	 * skipped the arithmetic would still have to get past. */
	if ((addr % 4u) != 0u || (len % 4u) != 0u)
		return NOR_SEAM_UNALIGNED;
	return NOR_SEAM_GO;
}

/* --- the wrappers ---------------------------------------------------------
 *
 * Signatures are the SDK's, taken from qspi_eeprom_interface.h, so the argument
 * ABI is whatever the prebuilt archive was built with rather than whatever this
 * file guessed.  __real_ is declared with the same prototype for the same
 * reason.
 */
extern int32_t __real_hx_lib_qspi_eeprom_erase_sector(uint32_t addr,
                                                      FLASH_ERASE_SIZE_E sz);
extern int32_t __real_hx_lib_qspi_eeprom_write(uint32_t addr, uint8_t *data,
                                               uint32_t len,
                                               uint8_t word_switch);

/* @p a and @p b are the call's first two interesting arguments -- address and
 * erase unit, address and length -- and not a range: for a refused erase the
 * requested unit is exactly what a reader needs and is exactly what is NOT the
 * footprint the seam would have permitted. */
static int32_t refuse(const char *what, enum nor_seam_verdict v,
                      uint32_t a, uint32_t b)
{
	LOG_ERR("seam refused %s (0x%08lx, 0x%lx): %s", what,
	        (unsigned long)a, (unsigned long)b, nor_seam_verdict_name(v));
	return NOR_SEAM_REFUSED;
}

int32_t __wrap_hx_lib_qspi_eeprom_erase_sector(uint32_t addr,
                                               FLASH_ERASE_SIZE_E sz)
{
	enum nor_seam_verdict v =
		nor_seam_check_erase(nor_lifecycle_state(), addr, (uint32_t)sz);

	if (v != NOR_SEAM_GO)
		return refuse("erase", v, addr, (uint32_t)sz);
	return __real_hx_lib_qspi_eeprom_erase_sector(addr, sz);
}

int32_t __wrap_hx_lib_qspi_eeprom_write(uint32_t addr, uint8_t *data,
                                        uint32_t len, uint8_t word_switch)
{
	enum nor_seam_verdict v =
		nor_seam_check_write(nor_lifecycle_state(), addr, data, len,
		                     (uint32_t)word_switch);

	if (v != NOR_SEAM_GO)
		return refuse("write", v, addr, len);
	return __real_hx_lib_qspi_eeprom_write(addr, data, len, word_switch);
}

/* [!] NO __real_ BELOW THIS LINE, and check_nor_seam.py checks that it stays
 * that way -- a conditional refusal here would be a second write path, and a
 * chip erase has no address to condition it on. */

int32_t __wrap_hx_lib_qspi_eeprom_erase_all(void)
{
	return refuse("erase_all", NOR_SEAM_NO_ADDRESS, 0u, 0u);
}

int32_t __wrap_hx_lib_qspi_eeprom_word_write(uint32_t addr, uint32_t *data,
                                             uint32_t bytes_len)
{
	(void)data;
	return refuse("word_write", NOR_SEAM_NO_PATH, addr, bytes_len);
}
