/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    timebase.h
 * @brief   Microsecond busy-wait via the Cortex-M55 DWT cycle counter (svc/).
 *
 * Grove Vision AI V2 port: udelay() runs on DWT->CYCCNT at the runtime-read
 * core clock (SystemCoreClock, set by platform_driver_init()'s SCU read-back).
 * A foreground busy-wait needs no peripheral and touches no clock registers,
 * which keeps the app's clock-inheritance contract intact.
 */
#ifndef TIMEBASE_H
#define TIMEBASE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Enable the DWT cycle counter used by udelay().  Touches only CoreDebug/DWT.
 * Call once early (from main(), before tx_kernel_enter()).
 */
void timebase_init(void);

/**
 * Busy-wait @p us microseconds on the DWT cycle counter.  Does NOT yield --
 * short delays only; the `usleep` command caps it.
 */
void udelay(uint32_t us);

#ifdef __cplusplus
}
#endif

#endif /* TIMEBASE_H */
