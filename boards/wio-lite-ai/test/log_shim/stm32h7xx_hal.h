/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Host-test shim for stm32h7xx_hal.h, as svc/log.c uses it (issue #112).
 *
 * log.c needs the PRIMASK accessors, __disable_irq, the two barriers,
 * HAL_GetTick() and RCC->RSR with its reset-cause flags -- and nothing else,
 * so nothing else is here.  A bigger shim would be a second copy of the HAL.
 *
 * The five intrinsics are TRACED (see test_log.c): the ring must only be
 * touched with PRIMASK set, PRIMASK must come back as it was, and the barriers
 * must fall where the append says they do.
 *
 * [!] THE RCC_RSR_* VALUES ARE DISTINCT BITS, NOT THE DEVICE'S.  log_init()
 * decodes the reset cause from them; that decode is not what this test is
 * for, and copying RM0468's positions here would only make a second place
 * that has to be kept right.
 */
#ifndef LOG_SHIM_STM32H7XX_HAL_H
#define LOG_SHIM_STM32H7XX_HAL_H

#include <stdint.h>

extern uint32_t log_test_primask;
void log_test_event(char kind);

static inline uint32_t __get_PRIMASK(void)
{
	return log_test_primask;
}

static inline void __disable_irq(void)
{
	log_test_primask = 1u;
	log_test_event('I');
}

static inline void __set_PRIMASK(uint32_t v)
{
	log_test_primask = v;
	log_test_event('E');
}

static inline void __DMB(void)
{
	log_test_event('M');
}

static inline void __DSB(void)
{
	log_test_event('S');
}

extern uint32_t log_test_tick;

static inline uint32_t HAL_GetTick(void)
{
	return log_test_tick;
}

struct log_test_rcc {
	volatile uint32_t RSR;
};
extern struct log_test_rcc log_test_rcc;
#define RCC (&log_test_rcc)

#define RCC_RSR_RMVF       (1u << 0)
#define RCC_RSR_LPWRRSTF   (1u << 1)
#define RCC_RSR_WWDG1RSTF  (1u << 2)
#define RCC_RSR_IWDG1RSTF  (1u << 3)
#define RCC_RSR_SFTRSTF    (1u << 4)
#define RCC_RSR_PORRSTF    (1u << 5)
#define RCC_RSR_BORRSTF    (1u << 6)
#define RCC_RSR_PINRSTF    (1u << 7)

#endif /* LOG_SHIM_STM32H7XX_HAL_H */
