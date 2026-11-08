/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Host-test shim for stm32h7xx_hal.h, as src/link_data.c uses it (issue #218):
 * the three PRIMASK accessors and nothing else.
 *
 * They are TRACED, and __disable_irq() is also where the test lands a
 * "preemption": when interrupts were enabled just before the mask is taken, the
 * hook the test armed runs first (once).  That models another thread or an ISR
 * running at the last unmasked instant before a critical section -- the only
 * place one can land relative to it -- so a check that sits outside its masked
 * section can be caught taking a decision on a value that has already moved.
 */
#ifndef LINK_DATA_SHIM_STM32H7XX_HAL_H
#define LINK_DATA_SHIM_STM32H7XX_HAL_H

#include <stdint.h>

extern uint32_t ld_test_primask;
extern unsigned ld_test_masked_sections;
void            ld_test_preempt_point(void);

static inline uint32_t __get_PRIMASK(void)
{
	return ld_test_primask;
}

static inline void __disable_irq(void)
{
	if (ld_test_primask == 0u)
		ld_test_preempt_point();
	ld_test_primask = 1u;
	ld_test_masked_sections++;
}

static inline void __set_PRIMASK(uint32_t v)
{
	ld_test_primask = v;
}

#endif /* LINK_DATA_SHIM_STM32H7XX_HAL_H */
