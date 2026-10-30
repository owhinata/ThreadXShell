/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Host stand-in for stm32f7xx_hal.h (issue #159, as wio-lite-ai's since #130),
 * so that port/plugin/plugin_run.c compiles on the host: for the derived entry
 * list (cmake/check_lease_entries.py, compile only) and for its own MPU
 * read-back test (test/test_plugin_run_mpu.c).  What the binding touches: the Armv7-M MPU's
 * read-back registers, the PRIMASK intrinsics, the barriers and the two cache
 * maintenance calls.  Nothing else is here: a bigger shim would be a second
 * copy of the HAL.
 */
#ifndef PLUGIN_RUN_SHIM_STM32F7XX_HAL_H
#define PLUGIN_RUN_SHIM_STM32F7XX_HAL_H

#include <stdint.h>

typedef struct {
	volatile uint32_t TYPE, CTRL, RNR, RBAR, RASR;
} MPU_Type;

/*
 * The MPU as the binding reads it: RBAR / RASR answer for the region RNR
 * selects (issue #159 Step 4b review -- the read-back loop is tested against
 * this, test/test_plugin_run_mpu.c).  One register block per region; MPU
 * re-reads the RNR last written and selects that block, so a write of RNR
 * followed by a read of RBAR sees region RNR's value, as on the part.  TYPE and
 * CTRL are the test's to keep equal in every block.
 */
#define TEST_MPU_BANKS 20u
extern MPU_Type test_mpu_bank[TEST_MPU_BANKS];
extern unsigned test_mpu_sel;

static inline MPU_Type *test_mpu_view(void)
{
	uint32_t r = test_mpu_bank[test_mpu_sel].RNR;
	unsigned k;

	if (r < TEST_MPU_BANKS) {
		test_mpu_sel = (unsigned)r;
		for (k = 0u; k < TEST_MPU_BANKS; k++)
			test_mpu_bank[k].RNR = r;
	}
	return &test_mpu_bank[test_mpu_sel];
}
#define MPU (test_mpu_view())

/* PRIMASK as a variable, so a test sees whether the binding restored it. */
extern uint32_t test_primask;
extern unsigned test_irq_disables;
static inline uint32_t __get_PRIMASK(void) { return test_primask; }
static inline void __disable_irq(void) { test_primask = 1u; test_irq_disables++; }
static inline void __enable_irq(void) { test_primask = 0u; }
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
