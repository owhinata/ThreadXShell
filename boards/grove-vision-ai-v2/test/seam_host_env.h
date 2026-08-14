/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Observable environment for the timer-seam host test (issue #30).
 *
 * The seam talks to four things: a Timer0 register block, the NVIC, the SCU
 * driver and udelay().  All four are replaced here with plain memory and
 * counters so the test can assert what the seam DID and, more importantly,
 * what it did NOT do -- "the refusal path writes no register" is a memcmp
 * against this struct, not an argument about the source.
 *
 * The register array is deliberately bigger than one timer block: the seam
 * must never reach past Timer0, and TIMER2 (the execution-profile time source)
 * is what it would damage if it did.
 */
#ifndef SEAM_HOST_ENV_H
#define SEAM_HOST_ENV_H

#include <stdint.h>

struct seam_host_env {
	/* Timer0's four registers (CTRL/VALUE/RELOAD/INTSTATUS) plus slack, so a
	   stray write past the block lands somewhere this test can see. */
	uint32_t regs[16];

	int      nvic_enabled;      /* NVIC_EnableIRQ/DisableIRQ tracking      */
	uint32_t nvic_vector;       /* EPII_NVIC_SetVector tracking            */
	int      registered;        /* tx_glue_profile_register_irq succeeded  */
	int      deny_registration; /* make the registry refuse (fail-closed)  */

	uint32_t scu_calls;         /* any SCU driver entry point was reached  */
	uint32_t udelay_calls;      /* udelay() was reached                    */
	uint32_t cb_calls;          /* the vendor callback was invoked         */

	/*
	 * A fake NVIC, for the epk_irq_wrap tests.  Enough of one to answer the
	 * question those tests exist for: after a partial failure and a retry,
	 * is every line either disabled or wrapped-and-registered?  That is a
	 * statement about enable bits and vector table entries, so both are
	 * modelled rather than stubbed.
	 */
	uint32_t iser[16];          /* enable bits, 512 lines                  */
	uint32_t vector[512];       /* the writable vector table               */
	uint32_t registry[16];      /* lines tx_glue_profile_register_irq took */
	int      fail_register_irqn; /* make registration refuse ONE line, -1 = none */

	/*
	 * A model of Timer0's counter, driven from udelay() -- which is exactly
	 * how the seam paces its own samples, so the two stay in step without
	 * the mock needing to intercept reads.
	 *
	 * timer_step   : counts subtracted from VALUE per udelay() call
	 * timer_stalls : after this many udelay() calls the counter freezes
	 *                (0 = never).  That is the "twitched once and stopped"
	 *                timer a single-sample check would have accepted.
	 */
	uint32_t timer_step;
	uint32_t timer_stalls;
};

extern struct seam_host_env seam_host_env;

#endif /* SEAM_HOST_ENV_H */
