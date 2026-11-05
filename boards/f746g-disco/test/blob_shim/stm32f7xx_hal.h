/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Host-test shim for stm32f7xx_hal.h, as src/blob.c uses it (issue #194; the
 * same shim as wio-lite-ai's for stm32h7xx_hal.h).
 *
 * blob.c includes the HAL for the PRIMASK accessors around its busy flag and
 * for nothing else, so only those three are here.  They are TRACED: the test
 * checks that the flag is tested and set with interrupts masked and that the
 * mask comes back as it was.
 */
#ifndef BLOB_SHIM_STM32F7XX_HAL_H
#define BLOB_SHIM_STM32F7XX_HAL_H

#include <stdint.h>

extern uint32_t blob_test_primask;
extern unsigned blob_test_masked_sections;

static inline uint32_t __get_PRIMASK(void)
{
	return blob_test_primask;
}

static inline void __disable_irq(void)
{
	blob_test_primask = 1u;
	blob_test_masked_sections++;
}

static inline void __set_PRIMASK(uint32_t v)
{
	blob_test_primask = v;
}

#endif /* BLOB_SHIM_STM32F7XX_HAL_H */
