/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * The mocked side of the timer-seam host test (issue #30): the SCU driver, the
 * EPK accounting registry and udelay(), plus the storage the shimmed NVIC and
 * register-block macros write into.
 *
 * Everything here is deliberately dumb -- it records that it was called and
 * succeeds.  The test's assertions are about the SEAM's decisions, so a mock
 * that failed in interesting ways would only move the logic under test out of
 * the file being tested.  The one exception is deny_registration, which exists
 * because "the EPK registry refused, so do not enable the interrupt" is a
 * decision the seam makes and therefore has to be reachable.
 */
#include <stdint.h>

#include "seam_host_env.h"
#include "hx_drv_scu.h"

struct seam_host_env seam_host_env;

/* ---- SCU driver -------------------------------------------------------- */

SCU_ERROR_E hx_drv_scu_set_timer_clk_en(uint32_t id, uint8_t en)
{
	(void)id; (void)en;
	seam_host_env.scu_calls++;
	return SCU_NO_ERROR;
}

SCU_ERROR_E hx_drv_scu_get_timer_clk_en(uint32_t id, uint8_t *en)
{
	(void)id;
	seam_host_env.scu_calls++;
	*en = 1u;
	return SCU_NO_ERROR;
}

SCU_ERROR_E hx_drv_scu_set_timer_clkdiv(uint32_t id, uint32_t div)
{
	(void)id; (void)div;
	seam_host_env.scu_calls++;
	return SCU_NO_ERROR;
}

SCU_ERROR_E hx_drv_scu_get_timer_clkdiv(uint32_t id, uint32_t *div)
{
	(void)id;
	seam_host_env.scu_calls++;
	*div = 1u;
	return SCU_NO_ERROR;
}

SCU_ERROR_E hx_drv_scu_set_timer_ctrl(uint32_t id, uint32_t ctrl)
{
	(void)id; (void)ctrl;
	seam_host_env.scu_calls++;
	return SCU_NO_ERROR;
}

/* A plausible SB APB1 clock: the board reads back 6 MHz for TIMER2 today, so a
   500 ms period lands on a reload that comfortably fits 32 bits and is not
   zero -- i.e. the accepted path is exercised, not accidentally refused. */
SCU_ERROR_E hx_drv_scu_get_freq(uint32_t type, uint32_t *hz)
{
	(void)type;
	seam_host_env.scu_calls++;
	*hz = 6000000u;
	return SCU_NO_ERROR;
}

/* ---- EPK accounting registry (port/threadx/tx_glue.h) ------------------ */

int tx_glue_profile_register_irq(int irqn, uint32_t wrapper_vector)
{
	if (seam_host_env.deny_registration)
		return 0;
	/* The real one verifies the vector is actually installed; keeping that
	 * check here is what makes "wrapped but not registered" impossible to
	 * pass by accident. */
	if (irqn >= 0 && irqn < 512 &&
	    seam_host_env.vector[irqn] != wrapper_vector)
		return 0;
	if (irqn == seam_host_env.fail_register_irqn)
		return 0;
	if (irqn >= 0 && irqn < 512)
		seam_host_env.registry[irqn >> 5] |= 1u << (irqn & 31);
	seam_host_env.registered = 1;
	return 1;
}

void tx_glue_profile_unregister_irq(int irqn)
{
	if (irqn >= 0 && irqn < 512)
		seam_host_env.registry[irqn >> 5] &= ~(1u << (irqn & 31));
}

/* The failure latch and the dmesg ring: swallowed here.  What the tests assert
 * is the HARDWARE state after a failure, not the wording of the log line -- and
 * a mock that recorded the text would pin the message rather than the
 * behaviour. */
void tx_glue_profile_fail(const char *why) { (void)why; }

void log_write(unsigned level, const char *tag, const char *fmt, ...)
{
	(void)level; (void)tag; (void)fmt;
}
void tx_glue_isr_enter(void) { }
void tx_glue_isr_exit(void) { }

/* ---- svc/timebase ------------------------------------------------------ */

void udelay(uint32_t us)
{
	(void)us;
	seam_host_env.udelay_calls++;

	/* Advance the modelled Timer0 down-counter.  regs[1] is VALUE (+0x04).
	 * Wrapping is deliberately naive -- the tests that use this pick a
	 * reload big enough that it never comes up. */
	if (seam_host_env.timer_step != 0u) {
		if (seam_host_env.timer_stalls != 0u &&
		    seam_host_env.udelay_calls > seam_host_env.timer_stalls)
			return;                 /* frozen from here on */
		seam_host_env.regs[1] -= seam_host_env.timer_step;
	}

	/*
	 * Deliver the interrupt, if the test asked for one (issue #35).  udelay()
	 * is the hook because it is where the firmware waits: the probe polls a
	 * flag between udelay() calls, so calling the vector from here reproduces
	 * "the interrupt arrived during the wait" without the mock needing a
	 * thread.  The line must be ENABLED, exactly as on hardware -- that is
	 * what makes "the probe passed but never enabled the line" a state this
	 * mock cannot manufacture.
	 */
	if (seam_host_env.deliver_after_udelays != 0u &&
	    seam_host_env.udelay_calls >= seam_host_env.deliver_after_udelays) {
		int irqn = seam_host_env.deliver_irqn;

		if (irqn >= 0 && irqn < 512 &&
		    (seam_host_env.nvic.ISER[irqn >> 5] &
		     (1u << (irqn & 31))) != 0u &&
		    seam_host_env.vector[irqn] != 0u) {
			void (*isr)(void) =
				(void (*)(void))(uintptr_t)seam_host_env.vector[irqn];

			seam_host_env.deliver_after_udelays = 0u;  /* once */
			seam_host_env.deliveries++;
			/* Timer0 asserts INTSTATUS before the CPU takes the
			 * interrupt; the seam's ISR reads it and writes 1 to
			 * clear.  regs[3] is INTSTATUS (+0x0C). */
			seam_host_env.regs[3] = 1u;
			isr();
		}
	}
}
