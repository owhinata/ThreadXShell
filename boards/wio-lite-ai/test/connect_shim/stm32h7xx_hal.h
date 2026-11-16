/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Host-test shim for stm32h7xx_hal.h, as src/wifi_connect.c and src/wifi_auto.c use
 * it (issue #240).
 *
 * wifi_connect.c includes the HAL for HAL_GetTick() -- the retry budget of the
 * association -- which the test defines; wifi_auto.c for the three PRIMASK accessors
 * around its critical sections, which here only keep the mask word (the tests run
 * on one thread; what they check is the order of whole calls, not interrupts).
 */
#ifndef CONNECT_SHIM_STM32H7XX_HAL_H
#define CONNECT_SHIM_STM32H7XX_HAL_H

#include <stdint.h>

uint32_t HAL_GetTick(void);

extern uint32_t connect_shim_primask;   /* defined by the test that needs it */

static inline uint32_t __get_PRIMASK(void)
{
	return connect_shim_primask;
}

static inline void __disable_irq(void)
{
	connect_shim_primask = 1u;
}

static inline void __set_PRIMASK(uint32_t v)
{
	connect_shim_primask = v;
}

#endif /* CONNECT_SHIM_STM32H7XX_HAL_H */
