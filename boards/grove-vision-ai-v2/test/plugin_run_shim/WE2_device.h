/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Host stand-in for the CMSIS device header, for test_plugin_run_lease.c only
 * (issue #127).  What port/plugin/plugin_run.c touches: the Armv8-M MPU's
 * read-back registers, the PRIMASK intrinsics, the barriers and the I-cache
 * invalidate.  The MPU is a plain struct the test sets, so the loader's own
 * read-back sees whatever configuration a case needs.
 */
#ifndef PLUGIN_RUN_SHIM_WE2_DEVICE_H
#define PLUGIN_RUN_SHIM_WE2_DEVICE_H

#include <stdint.h>

typedef struct {
	volatile uint32_t TYPE, CTRL, RNR, RBAR, RLAR, MAIR0, MAIR1;
} MPU_Type;

extern MPU_Type test_mpu;
#define MPU (&test_mpu)

static inline uint32_t __get_PRIMASK(void) { return 0u; }
static inline void __disable_irq(void) {}
static inline void __enable_irq(void) {}
static inline void __DSB(void) {}
static inline void __ISB(void) {}
static inline void SCB_InvalidateICache_by_Addr(volatile void *a, int32_t n)
{
	(void)a;
	(void)n;
}

#endif
