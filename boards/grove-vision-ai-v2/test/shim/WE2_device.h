/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Host-test shim for the SDK's WE2_device.h (issue #30).
 *
 * The real header drags in the whole WE2 device tree (every peripheral's
 * register map plus CMSIS for a Cortex-M55).  The seam needs four things from
 * it, so those four are supplied here and everything else is left out on
 * purpose: a bigger shim would be a second copy of the device definition, free
 * to drift from the SDK's without anything noticing.
 *
 * The type and enumerator names ARE copied from the SDK, because those are the
 * ABI the seam is written against; only the bodies are fake.
 */
#ifndef SEAM_SHIM_WE2_DEVICE_H
#define SEAM_SHIM_WE2_DEVICE_H

#include <stdint.h>
#include <stddef.h>

#include "seam_host_env.h"

/* Timer0's block is the mock array.  The seam is compiled with
   -DGROVE_TIMER_SEAM_T0_BASE pointing at the same storage. */
#define HX_TIMER0_BASE ((uint32_t)(uintptr_t)seam_host_env.regs)

typedef enum {
	TIMER0INT_IRQn = 34,
	TIMER1INT_IRQn = 35,
	TIMER2INT_IRQn = 36,
} IRQn_Type;

static inline void __DSB(void) { }
static inline void __ISB(void) { }

/*
 * The NVIC.  The timer-seam test only needs "was the line enabled", so it reads
 * nvic_enabled/nvic_vector; the epk_irq_wrap test needs the real per-line state,
 * so the enable bits and the vector table are modelled too.  Both are kept in
 * step here rather than in two shims that could disagree.
 */
/* The modelled NVIC lives in the shared environment (seam_host_env.h), so
 * that `NVIC->ISER[i]` and `NVIC->ISPR[i]` are spelled here exactly as the
 * firmware spells them against CMSIS -- sizeof included. */
#define NVIC (&seam_host_env.nvic)

static inline void NVIC_EnableIRQ(IRQn_Type irq)
{
	seam_host_env.nvic_enabled = 1;
	if ((int)irq >= 0 && (int)irq < 512)
		seam_host_env.nvic.ISER[(int)irq >> 5] |= 1u << ((int)irq & 31);
}

static inline void NVIC_DisableIRQ(IRQn_Type irq)
{
	seam_host_env.nvic_enabled = 0;
	if ((int)irq >= 0 && (int)irq < 512)
		seam_host_env.nvic.ISER[(int)irq >> 5] &= ~(1u << ((int)irq & 31));
}

static inline void NVIC_ClearPendingIRQ(IRQn_Type irq)
{
	if ((int)irq >= 0 && (int)irq < 512)
		seam_host_env.nvic.ISPR[(int)irq >> 5] &= ~(1u << ((int)irq & 31));
}
static inline void NVIC_SetPriority(IRQn_Type irq, uint32_t p)
{
	(void)irq; (void)p;
}

static inline uint32_t NVIC_GetVector(IRQn_Type irq)
{
	if ((int)irq >= 0 && (int)irq < 512)
		return seam_host_env.vector[(int)irq];
	return seam_host_env.nvic_vector;
}

void Default_Handler(void);

#endif /* SEAM_SHIM_WE2_DEVICE_H */
