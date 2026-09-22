/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Host-test shim for the SDK's WE2_device.h, as svc/log.c uses it (issue #112).
 *
 * log.c needs exactly five things from the device header: the PRIMASK
 * accessors, __disable_irq, and the two barriers.  They are supplied here and
 * nothing else is -- a bigger shim would be a second copy of the device
 * definition.  (The seam tests' shim in ../shim models the timer and the NVIC
 * instead; it is kept separate so neither grows the other's surface.)
 *
 * Every one of the five is TRACED: test_log.c defines log_test_event() and
 * log_test_primask, and asserts on the sequence -- that the ring is only ever
 * touched with PRIMASK set, that it is restored to what it was, and where the
 * barriers fall relative to the seq bump and the head commit.
 */
#ifndef LOG_SHIM_WE2_DEVICE_H
#define LOG_SHIM_WE2_DEVICE_H

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

#endif /* LOG_SHIM_WE2_DEVICE_H */
