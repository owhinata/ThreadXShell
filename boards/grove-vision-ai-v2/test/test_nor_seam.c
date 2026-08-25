/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Host test for the NOR write seam (issue #88,
 * port/sdk_seam/nor_seam.c).
 *
 * WHY THIS EXISTS.  cmake/check_nor_seam.py settles the LINK-level claim --
 * that only the seam reaches the vendor's erase and program entry points.  This
 * settles the behavioural one, and it is the only thing that can: every caller
 * of the wrappers is first-party code written to satisfy them, so no console
 * input and no sequence on the board produces an address one byte past the
 * writable interval, an erase unit this die has never been asked for, or a
 * chip erase.  Those exist here or nowhere.
 *
 * [!] AND THE ASSERTION THAT MATTERS IS NOT THE RETURN VALUE.  The vendor's
 * own erase and program report nothing usable -- erase_sector returns the
 * write-protect helper's result and discards everything after it, write returns
 * a hard-coded 0 -- so what a refusal has to guarantee is that the part was
 * never addressed at all.  The __real_ stubs below record every call, and every
 * refused case checks that the recorder is still empty.  A seam that refused
 * and called through anyway would pass a return-value test.
 *
 * The REAL nor_seam.c is compiled, against the REAL SDK headers, so the
 * FLASH_ERASE_SIZE_E enum ABI the wrappers are written against is the
 * firmware's rather than one restated here.
 */
#include <stdio.h>
#include <stdarg.h>
#include <stdint.h>

#include "nor_seam.h"
/* The vendor prototypes, for the same reason nor_seam.c includes them: the
 * erase entry point takes FLASH_ERASE_SIZE_E, and a locally invented `int`
 * would be this test agreeing with itself about an ABI instead of with the
 * archive.  Pulled in with -isystem so the SDK's own warnings stay out of the
 * suite's output. */
#include "qspi_eeprom_interface.h"

static int failures;

#define CHECK(cond, ...)                                                       \
	do {                                                                   \
		if (!(cond)) {                                                 \
			printf("FAIL %s:%d: ", __FILE__, __LINE__);            \
			printf(__VA_ARGS__);                                   \
			printf("\n");                                          \
			failures++;                                            \
		}                                                              \
	} while (0)

/* --- what the seam calls out to ------------------------------------------- */

static enum nor_state g_state = NOR_ST_WRITING;

enum nor_state nor_lifecycle_state(void)
{
	return g_state;
}

/* The board's log ring.  Refusals are logged; nothing here reads them back,
 * the point is only that the seam may call this. */
void log_write(unsigned level, const char *tag, const char *fmt, ...)
{
	(void)level;
	(void)tag;
	(void)fmt;
}

/* [!] THE RECORDER.  One counter and one copy of the last arguments, for both
 * entry points, because "was the part addressed" is the question. */
static struct {
	unsigned calls;
	uint32_t addr;
	uint32_t len;
	uint32_t unit;
	uint8_t *data;
	uint8_t  word_switch;
} vendor;

int32_t __real_hx_lib_qspi_eeprom_erase_sector(uint32_t addr,
                                               FLASH_ERASE_SIZE_E sz);
int32_t __real_hx_lib_qspi_eeprom_write(uint32_t addr, uint8_t *data,
                                        uint32_t len, uint8_t ws);

int32_t __real_hx_lib_qspi_eeprom_erase_sector(uint32_t addr,
                                               FLASH_ERASE_SIZE_E sz)
{
	vendor.calls++;
	vendor.addr = addr;
	vendor.unit = (uint32_t)sz;
	return 0;
}

int32_t __real_hx_lib_qspi_eeprom_write(uint32_t addr, uint8_t *data,
                                        uint32_t len, uint8_t ws)
{
	vendor.calls++;
	vendor.addr = addr;
	vendor.data = data;
	vendor.len = len;
	vendor.word_switch = ws;
	return 0;
}

/* The wrappers, as the firmware's linker names them. */
int32_t __wrap_hx_lib_qspi_eeprom_erase_sector(uint32_t addr,
                                               FLASH_ERASE_SIZE_E sz);
int32_t __wrap_hx_lib_qspi_eeprom_write(uint32_t addr, uint8_t *data,
                                        uint32_t len, uint8_t ws);
int32_t __wrap_hx_lib_qspi_eeprom_erase_all(void);
int32_t __wrap_hx_lib_qspi_eeprom_word_write(uint32_t addr, uint32_t *data,
                                             uint32_t bytes_len);

static void reset(enum nor_state st)
{
	g_state = st;
	vendor.calls = 0u;
}

static const enum nor_state ALL_STATES[] = {
	NOR_ST_OFF, NOR_ST_ENABLING, NOR_ST_XIP, NOR_ST_WRITING, NOR_ST_FAULTED,
};
#define NSTATES (sizeof(ALL_STATES) / sizeof(ALL_STATES[0]))

int main(void)
{
	const uint32_t LO = NOR_WRITABLE_LO;
	const uint32_t HI = NOR_WRITABLE_HI;
	const uint32_t U  = NOR_ERASE_UNIT;
	uint8_t payload[4] = { 0u, 1u, 2u, 3u };

	/* The record the gate reads out of .rodata is what the decisions use.
	 * [!] This does NOT check the interval is the right one -- the test is
	 * compiled with the same -D the firmware is.  What it checks is that one
	 * declaration feeds both, so the gate's twelve bytes and the arithmetic
	 * below cannot be different numbers. */
	CHECK(nor_seam_limits.lo == LO && nor_seam_limits.hi == HI &&
	      nor_seam_limits.unit == U,
	      "nor_seam_limits does not match the compile-time interval");

	/* --- only NOR_ST_WRITING may act ---------------------------------- */
	for (unsigned i = 0; i < NSTATES; i++) {
		enum nor_state st = ALL_STATES[i];
		enum nor_seam_verdict want =
			(st == NOR_ST_WRITING) ? NOR_SEAM_GO
			                       : NOR_SEAM_NO_TRANSACTION;

		CHECK(nor_seam_check_erase(st, LO, 0u) == want,
		      "erase in state %d: wrong verdict", (int)st);
		CHECK(nor_seam_check_write(st, LO, payload, 4u, 0u) == want,
		      "write in state %d: wrong verdict", (int)st);
	}

	/* --- the erase table ----------------------------------------------- */
	{
		struct {
			uint32_t addr;
			uint32_t unit;
			enum nor_seam_verdict want;
			const char *what;
		} cases[] = {
			{ LO,            0u, NOR_SEAM_GO,        "first unit" },
			{ HI - U,        0u, NOR_SEAM_GO,        "last unit" },
			{ LO,            1u, NOR_SEAM_BAD_UNIT,  "32 KB block" },
			{ LO,            2u, NOR_SEAM_BAD_UNIT,  "64 KB block" },
			{ LO + 1u,       0u, NOR_SEAM_UNALIGNED, "off the unit" },
			{ LO - U,        0u, NOR_SEAM_OUTSIDE,   "below" },
			{ HI,            0u, NOR_SEAM_OUTSIDE,   "at the end" },
			{ HI + U,        0u, NOR_SEAM_OUTSIDE,   "above" },
			{ 0u,            0u, NOR_SEAM_OUTSIDE,   "flash offset 0" },
			/* [!] The one the subtraction order exists for: an
			 * address so high that addr + unit wraps to something
			 * that looks contained. */
			{ 0xFFFFF000u,   0u, NOR_SEAM_OUTSIDE,   "wraps" },
		};

		for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
			enum nor_seam_verdict got =
				nor_seam_check_erase(NOR_ST_WRITING, cases[i].addr,
				                     cases[i].unit);
			CHECK(got == cases[i].want,
			      "erase %s (0x%08lx unit %lu): got %s, wanted %s",
			      cases[i].what, (unsigned long)cases[i].addr,
			      (unsigned long)cases[i].unit,
			      nor_seam_verdict_name(got),
			      nor_seam_verdict_name(cases[i].want));
		}
	}

	/* --- the write table ----------------------------------------------- */
	{
		struct {
			uint32_t addr;
			uint32_t len;
			uint32_t ws;
			int      null_data;
			enum nor_seam_verdict want;
			const char *what;
		} cases[] = {
			/* [!] WHOLE WORDS, and the three cases below used to say
			 * the opposite -- "first byte", "last byte" and
			 * "unaligned is fine" all expected GO until issue #92
			 * measured what a byte-granular write actually does.  The
			 * transport takes 32-bit words byte-reversed; a transfer
			 * that does not start and end on a word has no defined
			 * byte order, and the writer pads its tail to a word
			 * before it gets here. */
			{ LO,       4u,      0u, 0, NOR_SEAM_GO,       "first word" },
			{ HI - 4u,  4u,      0u, 0, NOR_SEAM_GO,       "last word" },
			{ LO,       HI - LO, 0u, 0, NOR_SEAM_GO,       "whole interval" },
			{ LO,       1u,      0u, 0, NOR_SEAM_UNALIGNED,"a single byte" },
			{ LO,       3u,      0u, 0, NOR_SEAM_UNALIGNED,"a short tail" },
			{ LO + 1u,  4u,      0u, 0, NOR_SEAM_UNALIGNED,"one byte into a word" },
			{ LO + 2u,  4u,      0u, 0, NOR_SEAM_UNALIGNED,"two bytes into a word" },
			{ LO,       0u,      0u, 0, NOR_SEAM_EMPTY,    "zero length" },
			{ LO,       4u,      1u, 0, NOR_SEAM_BAD_MODE, "word_switch" },
			{ LO,       4u,      0u, 1, NOR_SEAM_NO_BUFFER,"no payload" },
			{ LO - 4u,  4u,      0u, 0, NOR_SEAM_OUTSIDE,  "below" },
			{ HI - 4u,  8u,      0u, 0, NOR_SEAM_OUTSIDE,  "over the end" },
			{ HI,       4u,      0u, 0, NOR_SEAM_OUTSIDE,  "at the end" },
			/* Outside AND unaligned reports OUTSIDE: the bounds are
			 * what protect the flash. */
			{ HI + 1u,  4u,      0u, 0, NOR_SEAM_OUTSIDE,  "outside and unaligned" },
			{ LO,       HI - LO + 1u, 0u, 0, NOR_SEAM_OUTSIDE,
			  "one past the whole interval" },
			/* Same wrap case as the erase table: addr + len would
			 * be inside if it were ever computed. */
			{ 0xFFFFFF00u, 0x200u, 0u, 0, NOR_SEAM_OUTSIDE, "wraps" },
		};

		for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
			const void *d = cases[i].null_data ? NULL : payload;
			enum nor_seam_verdict got =
				nor_seam_check_write(NOR_ST_WRITING, cases[i].addr,
				                     d, cases[i].len, cases[i].ws);
			CHECK(got == cases[i].want,
			      "write %s (0x%08lx+0x%lx): got %s, wanted %s",
			      cases[i].what, (unsigned long)cases[i].addr,
			      (unsigned long)cases[i].len,
			      nor_seam_verdict_name(got),
			      nor_seam_verdict_name(cases[i].want));
		}
	}

	/* --- [!] a refusal must not reach the part ------------------------- */
	{
		reset(NOR_ST_XIP);
		CHECK(__wrap_hx_lib_qspi_eeprom_erase_sector(LO, FLASH_SECTOR) ==
		      NOR_SEAM_REFUSED, "erase outside a transaction was not refused");
		CHECK(vendor.calls == 0u,
		      "the erase was refused AND sent to the part");

		reset(NOR_ST_WRITING);
		CHECK(__wrap_hx_lib_qspi_eeprom_erase_sector(HI, FLASH_SECTOR) ==
		      NOR_SEAM_REFUSED, "an out-of-range erase was not refused");
		CHECK(vendor.calls == 0u,
		      "the erase was refused AND sent to the part");

		reset(NOR_ST_WRITING);
		CHECK(__wrap_hx_lib_qspi_eeprom_erase_sector(LO, FLASH_64KBLOCK) ==
		      NOR_SEAM_REFUSED, "a 64 KB block erase was not refused");
		CHECK(vendor.calls == 0u,
		      "the erase was refused AND sent to the part");

		reset(NOR_ST_WRITING);
		CHECK(__wrap_hx_lib_qspi_eeprom_write(HI - 1u, payload, 2u, 0u) ==
		      NOR_SEAM_REFUSED, "a write over the end was not refused");
		CHECK(vendor.calls == 0u,
		      "the write was refused AND sent to the part");
	}

	/* --- a permitted call goes through UNCHANGED ----------------------- */
	{
		reset(NOR_ST_WRITING);
		CHECK(__wrap_hx_lib_qspi_eeprom_erase_sector(HI - U,
		                                             FLASH_SECTOR) == 0,
		      "a bounded erase was not passed through");
		CHECK(vendor.calls == 1u && vendor.addr == HI - U &&
		      vendor.unit == 0u,
		      "the erase reached the part with different arguments");

		reset(NOR_ST_WRITING);
		CHECK(__wrap_hx_lib_qspi_eeprom_write(LO, payload, 4u, 0u) == 0,
		      "a bounded write was not passed through");
		CHECK(vendor.calls == 1u && vendor.addr == LO &&
		      vendor.data == payload && vendor.len == 4u &&
		      vendor.word_switch == 0u,
		      "the write reached the part with different arguments");
	}

	/* --- [!] the two with nothing to bound them, in EVERY state -------- */
	for (unsigned i = 0; i < NSTATES; i++) {
		uint32_t word = 0u;

		reset(ALL_STATES[i]);
		CHECK(__wrap_hx_lib_qspi_eeprom_erase_all() == NOR_SEAM_REFUSED,
		      "chip erase was not refused in state %d", (int)ALL_STATES[i]);
		CHECK(vendor.calls == 0u, "chip erase reached the part");

		reset(ALL_STATES[i]);
		CHECK(__wrap_hx_lib_qspi_eeprom_word_write(NOR_WRITABLE_LO, &word,
		                                           4u) == NOR_SEAM_REFUSED,
		      "word_write was not refused in state %d", (int)ALL_STATES[i]);
		CHECK(vendor.calls == 0u, "word_write reached the part");
	}

	if (failures) {
		printf("test_nor_seam: %d failure(s)\n", failures);
		return 1;
	}
	printf("test_nor_seam: ok\n");
	return 0;
}
