/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    timebase.c
 * @brief   Microsecond busy-wait via the Cortex-M55 DWT cycle counter.
 *
 * Grove Vision AI V2 port of the svc timebase.  udelay() counts CPU cycles on
 * DWT->CYCCNT against the RUNTIME core clock: SystemCoreClock holds the value
 * platform_driver_init() read back from the SCU (the app inherits the
 * bootloader's clock tree and never assumes the compile-time placeholder), so
 * no compile-time frequency constant is baked in here.  CLI_CPU_CYCLES_PER_US
 * remains a shell-core configuration knob; main.c warns at boot if it
 * disagrees with the read-back.
 *
 * Touches only DEMCR/DWT (no clock registers).  v8-M has no DWT->LAR unlock.
 */
#include "timebase.h"

#include "WE2_device.h"

void timebase_init(void)
{
	CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
	DWT->CYCCNT = 0u;
	DWT->CTRL  |= DWT_CTRL_CYCCNTENA_Msk;
}

void udelay(uint32_t us)
{
	uint32_t cyc_per_us = SystemCoreClock / 1000000u;
	uint64_t total;

	if (cyc_per_us == 0u)
		cyc_per_us = 1u;                /* insane clock: still terminate */
	total = (uint64_t)us * cyc_per_us;

	/* Wrap-safe: wait in chunks small enough that the 32-bit subtraction is
	 * unambiguous even if an ISR delays a poll by a whole wrap fraction. */
	while (total > 0u) {
		uint32_t chunk = (total > 0x40000000ull) ? 0x40000000u
		                                         : (uint32_t)total;
		uint32_t start = DWT->CYCCNT;
		while ((uint32_t)(DWT->CYCCNT - start) < chunk)
			;
		total -= chunk;
	}
}
