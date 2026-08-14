/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Host-test shim for the SDK's WE2_core.h: only EPII_NVIC_SetVector, which is
 * the cache-maintaining vector install the seam uses.
 */
#ifndef SEAM_SHIM_WE2_CORE_H
#define SEAM_SHIM_WE2_CORE_H

#include <stdint.h>

#include "WE2_device.h"
#include "seam_host_env.h"

/* Writes BOTH views of the vector table: nvic_vector is the "last one
 * installed" the timer-seam test looks at, vector[] is the per-line table the
 * epk_irq_wrap test needs.  Keeping them in step here is what stops the two
 * tests disagreeing about what got installed. */
static inline void EPII_NVIC_SetVector(IRQn_Type irq, uint32_t vector)
{
	seam_host_env.nvic_vector = vector;
	if ((int)irq >= 0 && (int)irq < 512)
		seam_host_env.vector[(int)irq] = vector;
}

#endif /* SEAM_SHIM_WE2_CORE_H */
