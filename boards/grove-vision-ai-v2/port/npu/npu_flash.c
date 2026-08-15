/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    npu_flash.c
 * @brief   Turn the QSPI NOR into a readable memory window (issue #44).
 *
 * WHY THIS IS NEEDED, AND HOW WE FOUND OUT
 *
 * `nn` parses the model flatbuffer IN PLACE in the external NOR, through the
 * memory-mapped read alias at 0x3A000000.  That window is NOT live at reset:
 * the QSPI master has to be opened and put into XIP mode by the application.
 * The bootloader reads the firmware out of the same flash, but through the
 * controller's register interface, so nothing before us has any reason to leave
 * XIP on.
 *
 * This was diagnosed on hardware rather than reasoned about, and the symptom is
 * worth recording because it is not what "unmapped memory" usually looks like:
 *
 *     devmem dump 0x3AB7B000 32   ->  07 0c 40 00 00 00 00 00 ...
 *     devmem dump 0x3A000000 32   ->  07 0c 40 00 00 00 00 00 ...
 *
 * Identical bytes 11 MB apart.  Reads were not faulting and were not returning
 * 0xFF -- they were landing on a controller register block aliased across the
 * whole window.  A model check that only looked for "erased flash" would have
 * sailed past it; what caught it was the flatbuffer identifier check in
 * npu_open(), and what explained it was being able to dump the window at all.
 *
 * [!] IT ENABLES AN INTERRUPT NOBODY NAMED.  hx_lib_spi_eeprom_open() turns on
 * DMAC1's combined interrupt (IRQ 133, DMAC1_DMACINTR_IRQn) -- the QSPI library
 * moves flash data with DMA.  Nothing in the plan or the review predicted that
 * line; it was the EPK snapshot wrap around the whole bring-up that found it,
 * which is exactly why the accounting is done by measuring what got enabled
 * rather than by listing the numbers we expect.  `nn info` prints the wrapped
 * set, so the answer is 133 and 192 rather than a hopeful 192.
 *
 * [!] READ PATH ONLY.  This flash holds the bootloader and the firmware image.
 * Nothing here writes, and the erase/write entry points that come with
 * lib_spi_eeprom.a are barred from the image by check_placement_budget.py --
 * with -ffunction-sections + --gc-sections, their PRESENCE means something
 * references them, which is exactly the property that gate asserts.  The
 * devmem allow-list follows the same split: the read alias is listed read-only
 * and the write alias is not listed at all.
 */
#include "npu_hw.h"

#include "spi_eeprom_comm.h"

#include "log.h"

/* The flash read alias, and a probe offset far enough into it that a degenerate
 * window cannot coincidentally match.  11 MB apart is where the aliasing above
 * showed itself. */
#define FLASH_R_BASE  0x3A000000u
#define FLASH_PROBE   0x00B00000u

static uint8_t xip_ready;

int npu_flash_xip_init(void)
{
	volatile const uint32_t *a = (volatile const uint32_t *)FLASH_R_BASE;
	volatile const uint32_t *b =
		(volatile const uint32_t *)(FLASH_R_BASE + FLASH_PROBE);

	if (xip_ready)
		return 0;

	if (hx_lib_spi_eeprom_open(USE_DW_SPI_MST_Q) != 0) {
		LOG_ERR("npu: QSPI open failed; the model cannot be read\r\n");
		return -1;
	}
	/* Quad, continuous-read.  Donor-identical: this is the configuration the
	 * SDK's own classification app uses to read a model from this part. */
	if (hx_lib_spi_eeprom_enable_XIP(USE_DW_SPI_MST_Q, true, FLASH_QUAD,
	                                 true) != 0) {
		LOG_ERR("npu: QSPI XIP enable failed\r\n");
		return -1;
	}

	/* Read-back: two widely separated words must not be identical.
	 *
	 * This is a weak check and is meant to be -- it does not prove the window
	 * shows the right bytes, only that it is no longer the degenerate alias
	 * described at the top of this file, which is the failure that actually
	 * happened.  Proof that the RIGHT bytes are there is npu_open()'s job: it
	 * checks the flatbuffer identifier before following a single offset. */
	if (*a == *b) {
		LOG_ERR("npu: flash window still aliases (0x%08lx == 0x%08lx); "
		        "XIP did not take\r\n",
		        (unsigned long)*a, (unsigned long)*b);
		return -1;
	}

	xip_ready = 1u;
	LOG_INF("npu: QSPI XIP on, model window readable at 0x%08lx\r\n",
	        (unsigned long)FLASH_R_BASE);
	return 0;
}

int npu_flash_xip_ready(void)
{
	return xip_ready ? 1 : 0;
}
