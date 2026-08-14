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

/* Every wrapped line needs a registry slot as well as a trampoline, so a
 * trampoline pool larger than the registry just turns into bring-up failures
 * once enough lines are in play.  The two constants live in different headers
 * (one is the seam's, one is the kit's); this is what keeps them together.
 *
 * EQUALITY, not "fits": a registry that is merely large enough would let the
 * two drift apart silently, and AGENTS.md states them as one number.  A
 * one-sided edit is meant to stop the build, which is the only moment anyone
 * will be thinking about both. */
_Static_assert(GROVE_EPK_WRAP_MAX == TX_GLUE_EPK_MAX_IRQ,
               "GROVE_EPK_WRAP_MAX and TX_GLUE_EPK_MAX_IRQ must be equal");

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

/* One list, used twice: once to define the functions and once to fill the
 * table.  Writing the numbers out is unavoidable -- ## needs literal digits --
 * but writing them out ONCE is not, and the _Static_assert below turns "raised
 * GROVE_EPK_WRAP_MAX and forgot a trampoline" into a compile error.  Without
 * it a short initialiser would leave NULL entries in a table whose values get
 * installed into the vector table. */
#define EPK_TRAMPOLINE_LIST                                                    \
	X( 0) X( 1) X( 2) X( 3) X( 4) X( 5) X( 6) X( 7)                        \
	X( 8) X( 9) X(10) X(11) X(12) X(13) X(14) X(15)                        \
	X(16) X(17) X(18) X(19) X(20) X(21) X(22) X(23)                        \
	X(24) X(25) X(26) X(27) X(28) X(29) X(30) X(31)

#define X(n) EPK_TRAMPOLINE(n)
EPK_TRAMPOLINE_LIST
#undef X

static void (*const trampolines[])(void) = {
#define X(n) epk_tramp##n,
	EPK_TRAMPOLINE_LIST
#undef X
};

_Static_assert(sizeof trampolines / sizeof trampolines[0] ==
               (size_t)GROVE_EPK_WRAP_MAX,
               "EPK_TRAMPOLINE_LIST and GROVE_EPK_WRAP_MAX disagree");

/*
 * Re-take a line the vendor has re-registered underneath us.
 *
 * The measure-then-wrap protocol assumes a vendor bring-up only ADDS enabled
 * lines.  That is not the whole truth: a driver can install a fresh ISR on a
 * line it has already got running, and then the vector table points at the
 * vendor handler while the accounting registry still points at our trampoline.
 * Nothing crashes -- the interrupt is simply no longer accounted, and
 * tx_glue_profile_ok() correctly reports the whole cpu% column as untrustworthy.
 *
 * Re-installing the ORIGINAL saved handler would be wrong: the vendor changed
 * it for a reason and the old one may no longer be valid.  So the new vendor
 * vector is adopted as the handler our trampoline calls, and the trampoline
 * goes back into the table.  The line keeps working AND stays accounted.
 *
 * @return 1 if every line in @p set is now wrapped, 0 if one could not be.
 */
static uint32_t epk_reasserts;

uint32_t grove_epk_irq_reasserts(void)
{
	return epk_reasserts;
}

int grove_epk_irq_reassert(const struct epk_irq_wrapset *set)
{
	uint32_t i, slot;
	int ok = 1;

	for (i = 0u; i < set->count; i++) {
		int irqn = set->irqn[i];
		uint32_t cur = NVIC_GetVector((IRQn_Type)irqn);
		uint32_t tramp;

		for (slot = 0u; slot < (uint32_t)GROVE_EPK_WRAP_MAX; slot++)
			if (wrapped[slot].vendor != NULL &&
			    wrapped[slot].irqn == irqn)
				break;
		if (slot == (uint32_t)GROVE_EPK_WRAP_MAX)
			continue;               /* already unwrapped: not ours */

		tramp = (uint32_t)(void (*)(void))trampolines[slot];
		if (cur == tramp)
			continue;               /* still ours */

		/* Refuse to adopt something that is not a handler: a null or the
		 * unclaimed-vector trap means the line was torn down, not
		 * re-registered, and wrapping that would turn its next interrupt
		 * into a hang. */
		if (cur == 0u ||
		    cur == (uint32_t)(void (*)(void))Default_Handler) {
			LOG_ERR("irq %d lost its vector entirely (%08lx)", irqn,
			        (unsigned long)cur);
			tx_glue_profile_fail("an accounted interrupt lost its "
			                     "vector");
			ok = 0;
			continue;
		}

		/* Log the first few and then only count.  This can fire once per
		 * FRAME (the datapath's retrigger re-registers), and a log line
		 * per frame would push everything else out of the dmesg ring
		 * within seconds -- while the count still says it is happening
		 * and how often. */
		if (epk_reasserts < 4u)
			LOG_INF("irq %d re-registered by the vendor (%08lx); "
			        "re-wrapping", irqn, (unsigned long)cur);
		epk_reasserts++;
		wrapped[slot].vendor = (void (*)(void))cur;
		EPII_NVIC_SetVector((IRQn_Type)irqn, tramp);
		__DSB();
		__ISB();
		if (NVIC_GetVector((IRQn_Type)irqn) != tramp) {
			LOG_ERR("irq %d re-wrap did not take", irqn);
			tx_glue_profile_fail("an interrupt re-wrap did not take");
			ok = 0;
		}
	}
	return ok;
}

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
