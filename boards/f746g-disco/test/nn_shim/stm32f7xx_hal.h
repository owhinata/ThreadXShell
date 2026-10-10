/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Host stand-in for stm32f7xx_hal.h, for test_nn_model.c only (issue #131).
 * What port/nn/nn.c touches: the DWT cycle counter, CoreDebug's DEMCR, the
 * PRIMASK intrinsics and the barriers.  DWT reports NOCYCCNT, so nn_init()
 * gives up on the counter before it would write the Lock Access Register at a
 * fixed address.  Nothing else is here: a bigger shim would be a second copy of
 * the HAL.
 */
#ifndef NN_SHIM_STM32F7XX_HAL_H
#define NN_SHIM_STM32F7XX_HAL_H

#include <stdint.h>

typedef struct {
	volatile uint32_t CTRL, CYCCNT;
} DWT_Type;
typedef struct {
	volatile uint32_t DEMCR;
} CoreDebug_Type;

#define DWT_CTRL_NOCYCCNT_Msk          (1u << 25)
#define DWT_CTRL_CYCCNTENA_Msk         (1u << 0)
#define CoreDebug_DEMCR_TRCENA_Msk     (1u << 24)

extern DWT_Type       test_dwt;
extern CoreDebug_Type test_coredebug;
#define DWT       (&test_dwt)
#define CoreDebug (&test_coredebug)

static inline uint32_t __get_PRIMASK(void) { return 0u; }
static inline void __set_PRIMASK(uint32_t v) { (void)v; }
static inline void __disable_irq(void) {}
static inline void __DSB(void) {}
static inline void __ISB(void) {}
static inline void __NOP(void) {}

#endif
