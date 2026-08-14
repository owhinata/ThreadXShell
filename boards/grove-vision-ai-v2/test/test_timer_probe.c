/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Host test for the Timer0 interrupt-DELIVERY probe (issue #35,
 * grove_timer_seam_probe_delivery() in port/sdk_seam/timer_seam.c).
 *
 * WHY THIS EXISTS.  The probe is itself a check, and a check that reports
 * success when it should not is worse than no check: the camera bring-up
 * refuses to proceed on a Timer0 whose interrupt never arrives, precisely so
 * that the datapath is never paced by a timer that is armed, believed and
 * useless.  If the probe can be made to pass without an interrupt, that whole
 * guarantee is decoration.
 *
 * It cannot be tested on hardware in the failing direction either -- engineering
 * a Timer0 that counts but does not interrupt is not something the board offers
 * -- so the mock NVIC delivers (or withholds) the interrupt instead, and both
 * outcomes get asserted.
 *
 * TWO PROCESSES, ONE PROBE EACH.  The probe latches its answer on first call,
 * deliberately: later callers want the result, not another 100 ms wait.  So the
 * two cases cannot share a process, and this file is compiled twice with
 * different -D, the way test_complete.c already is for its buffer-size cases.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "hx_drv_timer.h"
#include "seam_host_env.h"
#include "timer_seam.h"
#include "WE2_device.h"

static int failures;

#define CHECK(cond, ...)                                                       \
	do {                                                                   \
		if (!(cond)) {                                                 \
			printf("FAIL %s:%d: ", __FILE__, __LINE__);            \
			printf(__VA_ARGS__);                                   \
			printf("\n");                                          \
			failures++;                                            \
		}                                                              \
	} while (0)

void Default_Handler(void) { }

#define T0_IRQ 34

static int irq_enabled(int irqn)
{
	return (seam_host_env.nvic.ISER[irqn >> 5] >> (irqn & 31)) & 1u;
}

static int irq_registered(int irqn)
{
	return (seam_host_env.registry[irqn >> 5] >> (irqn & 31)) & 1u;
}

/*
 * Everything the probe needs to get PAST hw_start's own checks, so that what it
 * is being tested on is interrupt delivery and nothing else: a counter that
 * visibly runs (hw_start refuses a stationary one), a registry that accepts, and
 * a clean NVIC.
 */
static void env_setup(void)
{
	memset(&seam_host_env, 0, sizeof seam_host_env);
	seam_host_env.fail_register_irqn = -1;
	seam_host_env.deliver_irqn = T0_IRQ;
	/* The counter must move every sampling window.  6 MHz / 200 us windows
	 * is 1200 counts; anything in the band hw_start accepts will do. */
	seam_host_env.timer_step = 1200u;
	seam_host_env.regs[1] = 0xFFFFFFFFu;   /* VALUE, counting down */
}

int main(void)
{
	int rc;

	env_setup();

#ifdef PROBE_CASE_DELIVERED
	/* The interrupt arrives a few polls into the wait. */
	seam_host_env.deliver_after_udelays = 8u;

	rc = grove_timer_seam_probe_delivery();

	CHECK(rc != 0, "the probe failed even though the interrupt was "
	      "delivered (%s)",
	      grove_timer_seam_fault() ? grove_timer_seam_fault() : "no reason");
	CHECK(seam_host_env.deliveries == 1u,
	      "the mock delivered %lu interrupt(s), wanted 1",
	      (unsigned long)seam_host_env.deliveries);
	CHECK(grove_timer_seam_probe_hits() == 1u,
	      "the probe counted %lu interrupt(s), wanted 1",
	      (unsigned long)grove_timer_seam_probe_hits());

	/* The probe must leave nothing behind.  It borrowed Timer0 and the line
	 * belongs to the vendor's own hw_start afterwards, so a line left
	 * enabled or registered here is a state the accounting registry would
	 * later find and refuse to explain. */
	CHECK(!irq_enabled(T0_IRQ),
	      "the probe left Timer0's interrupt enabled");
	CHECK(!irq_registered(T0_IRQ),
	      "the probe left Timer0 registered with the accounting registry");
	CHECK(seam_host_env.regs[0] == 0u,
	      "the probe left Timer0 running (CTRL %08lx)",
	      (unsigned long)seam_host_env.regs[0]);
	/* INTSTATUS is not asserted on here, and the probe does not check it
	 * either: it is write-one-to-clear on the real part, which a mock backed
	 * by a plain array cannot model -- the ISR's clearing write and a
	 * spurious set look identical.  The observable consequence, an interrupt
	 * still pending at the NVIC, is what both assert on instead. */

	/* Latched: a second call must not re-run the 100 ms wait. */
	{
		uint32_t before = seam_host_env.udelay_calls;

		CHECK(grove_timer_seam_probe_delivery() == rc,
		      "the second probe call disagreed with the first");
		CHECK(seam_host_env.udelay_calls == before,
		      "the second probe call re-ran the wait");
	}

	if (failures == 0)
		printf("test_timer_probe[delivered]: OK\n");
#endif

#ifdef PROBE_CASE_SILENT
	/* No delivery at all: the timer counts perfectly and never interrupts,
	 * which is exactly the case hw_start's own counter check cannot see. */
	seam_host_env.deliver_after_udelays = 0u;

	rc = grove_timer_seam_probe_delivery();

	CHECK(rc == 0, "the probe PASSED with no interrupt delivered -- a "
	      "Timer0 that counts but never fires would be accepted");
	CHECK(grove_timer_seam_fault() != NULL,
	      "the probe failed without latching a reason");
	CHECK(grove_timer_seam_probe_hits() == 0u,
	      "the probe counted %lu interrupt(s) with none delivered",
	      (unsigned long)grove_timer_seam_probe_hits());

	/* Fail-closed: a refused probe must still hand Timer0 back quiet. */
	CHECK(!irq_enabled(T0_IRQ),
	      "a failed probe left Timer0's interrupt enabled");
	CHECK(!irq_registered(T0_IRQ),
	      "a failed probe left Timer0 registered");
	CHECK(seam_host_env.regs[0] == 0u,
	      "a failed probe left Timer0 running (CTRL %08lx)",
	      (unsigned long)seam_host_env.regs[0]);

	/* And the answer sticks: the camera bring-up asks once per open. */
	CHECK(grove_timer_seam_probe_delivery() == 0,
	      "a second probe call reported success after the first failed");

	if (failures == 0)
		printf("test_timer_probe[silent]: OK\n");
#endif

	if (failures != 0) {
		printf("test_timer_probe: %d failure(s)\n", failures);
		return 1;
	}
	return 0;
}
