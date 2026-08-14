/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    epk_irq_wrap.c
 * @brief   Measure-then-wrap for vendor-installed ISRs (issue #30).
 *
 * See epk_irq_wrap.h for why the interrupt set is measured rather than named.
 *
 * The trampolines are a fixed array of distinct functions because a vector
 * table entry carries no argument: each wrapped line needs its own frame that
 * knows which vendor handler to call.  GROVE_EPK_WRAP_MAX of them is not a
 * budget to grow casually -- every entry is an interrupt whose vendor code this
 * port cannot read, running inside the profile kit's accounting.
 */
#include <stdint.h>

#include "WE2_device.h"
#include "WE2_core.h"            /* EPII_NVIC_SetVector (cache-maintaining) */

#include "epk_irq_wrap.h"
#include "tx_glue.h"

#define LOG_TAG "epkirq"
#include "log.h"

void Default_Handler(void);  /* SDK startup; the unclaimed-vector trap */

#define ISER_WORDS (sizeof NVIC->ISER / sizeof NVIC->ISER[0])

_Static_assert(ISER_WORDS <= (sizeof ((struct epk_irq_snapshot *)0)->iser /
                              sizeof ((struct epk_irq_snapshot *)0)->iser[0]),
               "epk_irq_snapshot.iser is smaller than NVIC->ISER");

/* One slot per wrapped line: the vendor entry point and which line it serves.
 * A slot is free exactly when vendor == NULL.  Slots are never compacted --
 * slot index and trampoline index are the same thing, so moving an entry would
 * mean re-pointing a live vector table for no gain. */
static struct {
	void (*vendor)(void);
	int  irqn;
} wrapped[GROVE_EPK_WRAP_MAX];

/*
 * The trampolines.  Each is the OUTERMOST frame of its interrupt, which is what
 * makes the kit's nesting rules hold: enter, run the vendor handler to
 * completion, exit.  A vendor handler that never returns would strand the
 * nesting counter -- tx_glue_profile_ok() reports exactly that (it requires the
 * counter to read zero from thread context).
 */
#define EPK_TRAMPOLINE(n)                                                      \
	static void epk_tramp##n(void)                                         \
	{                                                                      \
		tx_glue_isr_enter();                                           \
		wrapped[n].vendor();                                           \
		tx_glue_isr_exit();                                            \
	}

EPK_TRAMPOLINE(0)
EPK_TRAMPOLINE(1)
EPK_TRAMPOLINE(2)
EPK_TRAMPOLINE(3)
EPK_TRAMPOLINE(4)
EPK_TRAMPOLINE(5)
EPK_TRAMPOLINE(6)
EPK_TRAMPOLINE(7)

static void (*const trampolines[GROVE_EPK_WRAP_MAX])(void) = {
	epk_tramp0, epk_tramp1, epk_tramp2, epk_tramp3,
	epk_tramp4, epk_tramp5, epk_tramp6, epk_tramp7,
};

void grove_epk_irq_snapshot(struct epk_irq_snapshot *snap)
{
	uint32_t i;

	for (i = 0u; i < ISER_WORDS; i++)
		snap->iser[i] = NVIC->ISER[i];
}

/* Wrap ONE line that is already disabled.  Returns 1 on success; on failure the
 * vector is left exactly as the vendor set it and the line stays disabled. */
static int wrap_one(int irqn)
{
	uint32_t vendor = NVIC_GetVector((IRQn_Type)irqn);
	uint32_t tramp;
	uint32_t slot;

	/* Already wrapped?  Then this line was enabled twice without being
	 * unwrapped in between, and a second slot would make grove_epk_irq_
	 * unwrap() restore whichever entry it found FIRST -- installing a stale
	 * vendor vector on a live line.  Refuse instead: a line left disabled
	 * costs a feature, a mismatched vector costs the interrupt. */
	for (slot = 0u; slot < (uint32_t)GROVE_EPK_WRAP_MAX; slot++) {
		if (wrapped[slot].vendor != NULL && wrapped[slot].irqn == irqn) {
			LOG_ERR("irq %d is already wrapped; refusing to wrap it "
			        "twice", irqn);
			tx_glue_profile_fail("an interrupt was wrapped twice "
			                     "without being unwrapped");
			return 0;
		}
	}

	for (slot = 0u; slot < (uint32_t)GROVE_EPK_WRAP_MAX; slot++) {
		if (wrapped[slot].vendor == NULL)
			break;
	}
	if (slot == (uint32_t)GROVE_EPK_WRAP_MAX) {
		LOG_ERR("no trampoline left for irq %d", irqn);
		tx_glue_profile_fail("EPK trampoline table is full");
		return 0;
	}
	if (vendor == 0u ||
	    vendor == (uint32_t)(void (*)(void))Default_Handler) {
		/* The driver enabled a line it never installed a handler for.
		 * Wrapping that would turn the first interrupt into a hang. */
		LOG_ERR("irq %d enabled with no vendor vector (%08lx)", irqn,
		        (unsigned long)vendor);
		tx_glue_profile_fail("a driver enabled an interrupt with no handler");
		return 0;
	}

	wrapped[slot].vendor = (void (*)(void))vendor;
	wrapped[slot].irqn   = irqn;
	tramp = (uint32_t)(void (*)(void))trampolines[slot];

	EPII_NVIC_SetVector((IRQn_Type)irqn, tramp);
	__DSB();
	__ISB();
	if (NVIC_GetVector((IRQn_Type)irqn) != tramp) {
		EPII_NVIC_SetVector((IRQn_Type)irqn, vendor);
		__DSB();
		__ISB();
		wrapped[slot].vendor = NULL;
		LOG_ERR("irq %d vector swap did not take", irqn);
		tx_glue_profile_fail("an interrupt vector swap did not take");
		return 0;
	}
	if (!tx_glue_profile_register_irq(irqn, tramp)) {
		EPII_NVIC_SetVector((IRQn_Type)irqn, vendor);
		__DSB();
		__ISB();
		wrapped[slot].vendor = NULL;
		return 0;            /* the registry latched its own reason */
	}

	LOG_INF("irq %d wrapped (vendor %08lx)", irqn, (unsigned long)vendor);
	return 1;
}

int grove_epk_irq_wrap_new(const struct epk_irq_snapshot *snap,
                           struct epk_irq_wrapset *out)
{
	uint32_t fresh[ISER_WORDS];
	uint32_t i, bit;
	int ok = 1;

	out->count = 0u;

	/* Pass 1: latch which lines appeared, then disable them ALL before
	 * touching any vector.  A line left enabled while its neighbour is
	 * being swapped could fire into a half-installed table -- and the
	 * enabled set has to be captured before the disabling starts, because
	 * that is what the disabling destroys. */
	for (i = 0u; i < ISER_WORDS; i++)
		fresh[i] = NVIC->ISER[i] & ~snap->iser[i];

	for (i = 0u; i < ISER_WORDS; i++) {
		for (bit = 0u; bit < 32u; bit++) {
			if ((fresh[i] & (1UL << bit)) != 0u)
				NVIC_DisableIRQ((IRQn_Type)(i * 32u + bit));
		}
	}
	__DSB();
	__ISB();

	/* Pass 2: wrap, register, re-enable -- one line at a time, and only
	 * re-enable the ones that got all the way through.  A line that failed
	 * stays disabled: no feature is worth an unaccounted interrupt. */
	for (i = 0u; i < ISER_WORDS; i++) {
		for (bit = 0u; bit < 32u; bit++) {
			int irqn = (int)(i * 32u + bit);

			if ((fresh[i] & (1UL << bit)) == 0u)
				continue;
			if (!wrap_one(irqn)) {
				ok = 0;
				continue;
			}
			/* Record BEFORE enabling: from here on the line is
			 * live, and an undo log that misses it would leave an
			 * enabled interrupt nobody can take back. */
			out->irqn[out->count] = irqn;
			out->count++;
			/* [!] Deliberately NOT clearing the pending bit.  The
			 * caller runs the vendor bring-up inside this same
			 * PRIMASK section, so a line can legitimately be
			 * PENDING here -- a DMA priming transfer completing
			 * while interrupts were masked is exactly that, and
			 * exactly what the LCD driver relies on.  Dropping it
			 * would lose a completion the vendor driver is waiting
			 * for and leave its busy flag set forever.  Pass 1
			 * disabled the line before any vector was touched, so
			 * the pending event can only be delivered now, through
			 * the wrapper. */
			NVIC_EnableIRQ((IRQn_Type)irqn);
		}
	}
	__DSB();
	__ISB();
	return ok;
}

void grove_epk_irq_unwrap(int irqn)
{
	uint32_t i;

	/* Disable and DEREGISTER before restoring the vector: for the window in
	 * between, an enabled line whose vector is no longer the registered
	 * wrapper would (correctly) make tx_glue_profile_ok() fail.  Doing it in
	 * this order means the line is simply gone from both sets at once. */
	NVIC_DisableIRQ((IRQn_Type)irqn);
	__DSB();
	__ISB();
	tx_glue_profile_unregister_irq(irqn);

	for (i = 0u; i < (uint32_t)GROVE_EPK_WRAP_MAX; i++) {
		if (wrapped[i].vendor == NULL || wrapped[i].irqn != irqn)
			continue;
		EPII_NVIC_SetVector((IRQn_Type)irqn,
		                    (uint32_t)(void (*)(void))wrapped[i].vendor);
		__DSB();
		__ISB();
		wrapped[i].vendor = NULL;
		return;
	}
}

void grove_epk_irq_unwrap_set(struct epk_irq_wrapset *set)
{
	uint32_t i;

	for (i = 0u; i < set->count; i++)
		grove_epk_irq_unwrap(set->irqn[i]);
	set->count = 0u;        /* idempotent: a later teardown finds nothing */
}
