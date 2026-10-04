/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Host stand-in for stm32h7xx_hal.h, for test_plugin_run_lease.c only (issue
 * #130).  What port/plugin/plugin_run.c touches: the Armv7-M MPU's read-back
 * registers, the PRIMASK intrinsics, the barriers and the two cache
 * maintenance calls.  The MPU is a plain struct the test sets, so the loader's
 * own read-back sees whatever configuration a case needs.  Nothing else is
 * here: a bigger shim would be a second copy of the HAL.
 */
#ifndef PLUGIN_RUN_SHIM_STM32H7XX_HAL_H
#define PLUGIN_RUN_SHIM_STM32H7XX_HAL_H

#include <stdint.h>

typedef struct {
	volatile uint32_t TYPE, CTRL, RNR, RBAR, RASR;
} MPU_Type;

extern MPU_Type test_mpu;
#define MPU (&test_mpu)

static inline uint32_t __get_PRIMASK(void) { return 0u; }
static inline void __disable_irq(void) {}
static inline void __enable_irq(void) {}
static inline void __DSB(void) {}
static inline void __ISB(void) {}
static inline void SCB_CleanDCache_by_Addr(uint32_t *a, int32_t n)
{
	(void)a;
	(void)n;
}
static inline void SCB_InvalidateICache_by_Addr(void *a, int32_t n)
{
	(void)a;
	(void)n;
}

#endif
