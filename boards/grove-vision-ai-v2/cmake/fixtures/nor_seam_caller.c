/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Fixture translation unit for cmake/check_nor_seam.py (issue #88).
 *
 * Each -DFX_* is one way a translation unit could reach the vendor's NOR write
 * path around the seam.  run_fixture_tests.py compiles this with the FIRMWARE's
 * own compile line, links it into the seam probe with -u nor_fixture so it
 * cannot be collected away, and requires the gate to refuse with the diagnostic
 * that case was built to produce -- never merely to exit non-zero.
 *
 * It lives under cmake/fixtures/ and is in no target: nothing that ships
 * carries a hook that exists for a test.
 *
 * The prototypes are declared here rather than included, because the point of
 * each case is a REFERENCE with a particular relocation type and the vendor
 * headers would drag the whole device tree in for it.  A wrong argument list
 * would still produce the reference; nothing here is ever executed.
 */
#include <stdint.h>

#if defined(FX_INNER_CALL)
/* The plain hole: some other file calls the wrapped entry point.  --wrap sends
 * it to the seam, so it is bounded -- but the seam's rules are then being
 * satisfied by a caller nobody enumerated, and the next such caller is the one
 * that erases the wrong thing. */
extern int32_t hx_lib_qspi_eeprom_erase_sector(uint32_t addr, int sz);
int32_t nor_fixture(void)
{
	return hx_lib_qspi_eeprom_erase_sector(0x00200000u, 0);
}

#elif defined(FX_REAL_CALL)
/* Straight past the seam: __real_ IS the vendor implementation. */
extern int32_t __real_hx_lib_qspi_eeprom_write(uint32_t addr, uint8_t *data,
                                               uint32_t len, uint8_t ws);
int32_t nor_fixture(void)
{
	return __real_hx_lib_qspi_eeprom_write(0x00200000u, (uint8_t *)0, 1u, 0u);
}

#elif defined(FX_OUTER_CALL)
/* The one an object-level rule cannot see.  spi_eeprom_comm.o is ALREADY a link
 * input (open / read_ID / enable_XIP live in it) and its outer forwarders each
 * hold a relocation to the inner name -- so this file does not have to name the
 * inner entry point at all, only keep the vendor's forwarder alive. */
extern int32_t hx_lib_spi_eeprom_write(int spi_id, uint32_t addr, uint8_t *data,
                                       uint32_t len, uint8_t ws);
int32_t nor_fixture(void)
{
	return hx_lib_spi_eeprom_write(0, 0x00200000u, (uint8_t *)0, 1u, 0u);
}

#elif defined(FX_ADDRESS)
/* An address is not a call.  Handing the pointer somewhere else defeats any
 * audit that follows edges -- and for __real_ it defeats --wrap itself. */
extern int32_t hx_lib_qspi_eeprom_write(uint32_t addr, uint8_t *data,
                                        uint32_t len, uint8_t ws);
uintptr_t nor_fixture(void)
{
	return (uintptr_t)&hx_lib_qspi_eeprom_write;
}

#elif defined(FX_CHIP_ERASE)
/* Reaching for the one operation that has no address to be bounded against. */
extern int32_t hx_lib_qspi_eeprom_erase_all(void);
int32_t nor_fixture(void)
{
	return hx_lib_qspi_eeprom_erase_all();
}

#else
#error "compile one FX_* case"
#endif
