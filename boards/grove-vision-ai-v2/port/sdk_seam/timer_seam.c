/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    timer_seam.c
 * @brief   Board-owned implementations of the vendor timer API (issue #30).
 *
 * Installed with -Wl,--wrap=<symbol> (board.cmake), so every reference from the
 * prebuilt archives resolves here.  __real_hx_drv_timer_* is NEVER called: the
 * point of the seam is that no vendor timer code survives into the image, which
 * is what lets check_placement_budget.py keep barring the whole hx_drv_timer_*
 * prefix.  Everything the archives need is reproduced below.
 *
 * WHAT THE ARCHIVES ACTUALLY ASK FOR (verified by disassembly, see the board
 * README):
 *  - hx_drv_timer_hw_start / hw_stop: 41 call sites in libsensordp.a
 *    (sensor_dp_lib.o), every one of them passing the constant TIMER_ID_0.
 *    Timer0 is the datapath's frame watchdog: start_periodic_timer() arms a
 *    periodic callback (periodhalfsec_timer_dump) that either retriggers the
 *    capture or raises an error event when a frame does not complete.
 *  - hx_drv_timer_cm55x_delay_ms / _us: plain busy delays.  The SDK's own
 *    interface/timer_interface.c forwards them to the TIMER_ID_3 wrappers; this
 *    port does not compile that file any more (board.cmake) and spends DWT
 *    cycles instead, so no Himax timer is involved at all.
 *
 * FAIL CLOSED, AND ISR-SAFE ABOUT IT.  Any id other than TIMER_ID_0, and any
 * configuration this file does not reproduce exactly, is REFUSED -- no register
 * is written, an error is returned, and the reason is latched for the `epk`
 * command.  The refusal path deliberately has no logging, no mutex, no ThreadX
 * call and no fail-stop loop: hx_drv_timer_hw_stop() is reached from the
 * vendor's own Timer0 ISR callback (periodhalfsec_timer_dump tail-calls
 * sensordplib_retrigger_capture, which stops the timer), so anything blocking
 * there would run inside an interrupt.
 *
 * CONTEXT RULES, mirroring the vendor's own:
 *  - hw_start(): THREAD context only.  It talks to the SCU driver (clock gate,
 *    divider, owner) and installs a vector, exactly as the vendor entry point
 *    does.  Every SDK call site is a setup path.
 *  - hw_stop(): ISR-safe.  MMIO and NVIC only.
 *  - the delays: ISR-safe (udelay() is a DWT busy-wait).
 */
#include <stdint.h>

#include "WE2_device.h"
#include "WE2_core.h"            /* EPII_NVIC_SetVector (cache-maintaining) */
#include "hx_drv_scu.h"          /* SCU timer clock gate / divider / owner  */
#include "hx_drv_timer.h"        /* TIMER_* types -- declarations only      */

#include "timebase.h"            /* udelay(): DWT cycle busy-wait           */
#include "timer_seam.h"
#include "tx_glue.h"             /* EPK ISR hooks + accounted-irq registry  */

/* ---- Timer0 register block ---------------------------------------------- */

/* CMSDK-style layout (WE2_S.svd): CTRL/VALUE/RELOAD/INTSTATUS at +0x00..+0x0C.
 * The CTRL bit meanings are the ones the SDK documents on hx_drv_timer_GetCtrl():
 * bit0 = counter enable, bit3 = interrupt enable.
 *
 * The override exists for the host test (test/test_timer_seam.c), which points
 * the block at a plain array so the refusal paths can be asserted to write
 * nothing.  A pointer into an array is not a constant expression, so the static
 * assert cannot survive that -- which is why the FIRMWARE build has two
 * independent guards instead of one: the assert below (the literal matches the
 * SDK's macro) and check_timer_seam.py's S6 (the linked image really carries
 * 0x5500A000 inside the seam).  Defining the override in a firmware build
 * silences the first; the second still fails the build. */
#ifdef GROVE_TIMER_SEAM_T0_BASE
#define T0_BASE       GROVE_TIMER_SEAM_T0_BASE
#else
#define T0_BASE       0x5500A000UL
_Static_assert(T0_BASE == (uint32_t)HX_TIMER0_BASE,
               "TIMER0 base moved: T0_BASE no longer matches HX_TIMER0_BASE");
#endif

#define T0_CTRL       (*(volatile uint32_t *)(T0_BASE + 0x00u))
#define T0_VALUE      (*(volatile uint32_t *)(T0_BASE + 0x04u))
#define T0_RELOAD     (*(volatile uint32_t *)(T0_BASE + 0x08u))
#define T0_INTSTATUS  (*(volatile uint32_t *)(T0_BASE + 0x0Cu))

/* CTRL bits, per the SDK's own documentation on hx_drv_timer_GetCtrl():
 * bit0 counter enable, bit1/bit2 external-input trigger modes, bit3 interrupt
 * enable. */
#define T0_CTRL_EN     (1u << 0)
#define T0_CTRL_EXTCLK (1u << 2)
#define T0_CTRL_IRQEN  (1u << 3)

/*
 * [!] The value the VENDOR writes, bit for bit.
 *
 * Disassembling the pinned SDK's StartTimer.part.0 shows a CPU-controlled,
 * TIMER_STATE_DC timer started with CTRL = 0x0D -- enable, interrupt enable,
 * and bit2, which the header calls "enable by external input, use a clock with
 * high/low state".  Setting bit2 on a timer meant to count the internal clock
 * makes no sense as documented, and issue #25 established on HARDWARE that this
 * timer block counts perfectly well without it (TIMER2 runs at CTRL = 0x01 and
 * `epk sleep` measures exactly the rate the SCU predicts).  So the bit appears
 * inert in CPU mode.
 *
 * "Appears inert" is not a reason to differ from the implementation the
 * prebuilt archives were built against, though: this file's whole claim is that
 * it reproduces that behaviour.  So the vendor's value is used, and the
 * ASSUMPTION IS CHECKED -- the counter must be observed advancing before this
 * function reports success (see below).  If the bit does mean what the header
 * says on some future part, the seam refuses instead of handing the datapath a
 * watchdog that never fires.
 */
#define T0_CTRL_VENDOR (T0_CTRL_EN | T0_CTRL_EXTCLK)

/*
 * Proving the armed counter RUNS -- not merely that it twitched once.
 *
 * One observed change is not enough: a transient at arm time, or a counter that
 * advances a few ticks and stops, would both pass it, and the datapath would be
 * handed a frame watchdog that reports armed and never expires.  So the counter
 * is sampled across several intervals timed by an INDEPENDENT clock (udelay, on
 * DWT cycles), and every interval must show motion.
 *
 * Where the period is long enough that the counter cannot wrap inside one
 * interval, the AMOUNT is checked too, against what Timer0's own clock rate
 * predicts.  The band is deliberately loose -- this is separating "running" from
 * "stopped or running at a wildly different rate", not calibrating anything.
 */
#define T0_CHECK_INTERVALS 4u
#define T0_CHECK_US        200u
#define T0_CHECK_RATE_LO   4u   /* delta must be >= expected / 4 */
#define T0_CHECK_RATE_HI   4u   /* delta must be <= expected * 4 */

/* The SCU divider applied to Timer0's reference clock.  1 = full resolution,
 * matching what this port does for TIMER2 (port/threadx/tx_glue.c). */
#define T0_CLKDIV 1u

/* Timer0's interrupt line (device/inc/WE2_ARMCM55.h). */
_Static_assert((int)TIMER0INT_IRQn == 34,
               "TIMER0INT_IRQn moved; the seam's IRQ bookkeeping assumes 34");

/* Priority for the datapath watchdog.  Below the console UART (which must stay
 * responsive) and above PendSV/SysTick's 7/6 in NUMERIC terms means a SMALLER
 * number; __NVIC_PRIO_BITS is 3 here, so the usable range is 0..7. */
#ifndef GROVE_TIMER0_IRQ_PRIORITY
#define GROVE_TIMER0_IRQ_PRIORITY 3
#endif

/* ---- seam state ---------------------------------------------------------- */

/* Latched refusal.  String literals only -- stored by pointer, never copied.
 * Written from both thread and interrupt context: the pointer store is a single
 * aligned word (atomic here), and a lost increment on the counter is a lost
 * diagnostic, not a lost guarantee -- so no critical section, which is what
 * keeps the refusal path usable from the vendor's Timer0 ISR callback. */
static volatile const char *seam_why;
static volatile uint32_t    seam_refusals;

/* Timer0 callback the vendor handed us, and the mode it asked for. */
static volatile Timer_ISREvent_t t0_cb;
static volatile uint8_t          t0_oneshot;

/* ISR-safe refusal: latch the first reason, count them all, write nothing. */
static TIMER_ERROR_E seam_refuse(const char *why)
{
	if (seam_why == NULL)
		seam_why = why;
	seam_refusals++;
	return TIMER_ERROR_INVALID_PARAMETERS;
}

const char *grove_timer_seam_fault(void)    { return (const char *)seam_why; }
uint32_t    grove_timer_seam_refusals(void) { return seam_refusals; }

/* ---- Timer0 ISR ---------------------------------------------------------- */

/*
 * This port owns the handler outright (unlike UART0, whose handler lives inside
 * libdriver.a and has to be wrapped), so the EPK accounting hooks bracket the
 * body directly -- this frame IS the outermost frame of the interrupt.
 */
static void timer0_seam_isr(void)
{
	Timer_ISREvent_t cb;
	uint32_t status;

	tx_glue_isr_enter();

	status = T0_INTSTATUS;
	T0_INTSTATUS = 1u;                      /* w1c */

	if (t0_oneshot != 0u) {
		T0_CTRL = 0u;                   /* one shot: disarm before the cb */
		__DSB();
	}

	cb = t0_cb;
	if (cb != NULL)
		cb(status);

	tx_glue_isr_exit();
}

/*
 * Is the armed Timer0 actually running?
 *
 * @p ticks is RELOAD + 1 (the counter's modulus) and @p hz its clock rate.
 * Samples across T0_CHECK_INTERVALS windows of T0_CHECK_US, each timed by
 * udelay() -- a DWT cycle wait, independent of the timer under test, so a dead
 * Timer0 cannot also stall the measurement.
 *
 * Every window must show motion; that is what separates "running" from a
 * one-off twitch at arm time.  Where a window is short enough that the
 * down-counter cannot pass zero more than once, the amount is checked too.
 */
static int t0_is_running(uint32_t ticks, uint32_t hz)
{
	uint64_t expect64 = ((uint64_t)hz * (uint64_t)T0_CHECK_US) / 1000000ull;
	uint32_t expect = (uint32_t)expect64;
	uint32_t prev = T0_VALUE;
	uint32_t i;

	/* Only meaningful while a window cannot contain a whole cycle; with a
	 * very short period the count still has to MOVE, but by how much stops
	 * being predictable without tracking reloads. */
	int rate_checkable = (expect64 != 0ull) && (expect64 < (uint64_t)ticks);

	for (i = 0u; i < T0_CHECK_INTERVALS; i++) {
		uint32_t cur, delta;

		udelay(T0_CHECK_US);
		cur = T0_VALUE;

		if (cur > prev) {
			/* Wrapped: ... 1, 0, RELOAD.  One reload at most, which
			 * rate_checkable guarantees where it is used. */
			delta = prev + (ticks - cur);
		} else {
			delta = prev - cur;
		}
		if (delta == 0u)
			return 0;               /* stopped in this window */
		if (rate_checkable &&
		    (delta < expect / T0_CHECK_RATE_LO ||
		     delta > expect * T0_CHECK_RATE_HI))
			return 0;               /* running, but not at its clock */
		prev = cur;
	}
	return 1;
}

/* ---- wrapped entry points ------------------------------------------------ */

/*
 * hx_drv_timer_hw_start(TIMER_ID_0, cfg, cb) -- thread context.
 *
 * Reproduces the vendor behaviour the datapath depends on: reload derived from
 * cfg->period MILLISECONDS against the SCU reference clock, counter enabled
 * under CPU control, interrupt enabled when a callback was supplied.
 *
 * Every configuration outside that is refused rather than approximated, and a
 * failure to REGISTER the interrupt with the EPK accounting registry refuses
 * the whole call: an enabled line with no accounting wrapper would silently
 * bill its runtime to whichever thread it interrupted, which is precisely the
 * fail-open this port spent issue #25 closing.
 */
TIMER_ERROR_E __wrap_hx_drv_timer_hw_start(TIMER_ID_E id, TIMER_CFG_T *cfg,
                                           Timer_ISREvent_t cb_event)
{
	uint32_t vector = (uint32_t)(void (*)(void))timer0_seam_isr;
	uint32_t ref = 0u, div_rb = 0u, ticks;
	uint64_t t64;
	uint8_t  en_rb = 0u;

	if (id != TIMER_ID_0)
		return seam_refuse("timer seam refused a non-Timer0 start");
	if (cfg == NULL)
		return seam_refuse("timer seam got a NULL Timer0 config");
	if (cfg->ctrl != TIMER_CTRL_CPU)
		return seam_refuse("timer seam refused a PMU-started Timer0");
	if (cfg->state != TIMER_STATE_DC)
		return seam_refuse("timer seam refused a PMU-state Timer0");
	if (cfg->mode != TIMER_MODE_PERIODICAL && cfg->mode != TIMER_MODE_ONESHOT)
		return seam_refuse("timer seam refused an unsupported Timer0 mode");
	if (cfg->period == 0u)
		return seam_refuse("timer seam got a zero Timer0 period");

	/* SCU: gate the clock, pin the divider, put the timer under CPU control,
	 * and learn the reference -- reading back everything that can be read
	 * back, so a write that did not take cannot produce a plausible but
	 * wrong period.  (The owner field has no read-back; the counter check
	 * below covers its observable consequence.) */
	if (hx_drv_scu_set_timer_clk_en(TIMER_ID_0, 1u) != SCU_NO_ERROR ||
	    hx_drv_scu_get_timer_clk_en(TIMER_ID_0, &en_rb) != SCU_NO_ERROR ||
	    en_rb != 1u)
		return seam_refuse("timer seam: Timer0 SCU clock gate did not take");
	if (hx_drv_scu_set_timer_clkdiv(TIMER_ID_0, T0_CLKDIV) != SCU_NO_ERROR ||
	    hx_drv_scu_get_timer_clkdiv(TIMER_ID_0, &div_rb) != SCU_NO_ERROR ||
	    div_rb != T0_CLKDIV || div_rb == 0u)
		return seam_refuse("timer seam: Timer0 SCU divider did not read back");
	if (hx_drv_scu_set_timer_ctrl((uint32_t)TIMER_ID_0, SCU_TIMERCTRL_CPU)
	    != SCU_NO_ERROR)
		return seam_refuse("timer seam: Timer0 SCU owner set failed");
	if (hx_drv_scu_get_freq(SCU_CLK_FREQ_TYPE_SB_APB_1_CLK, &ref)
	    != SCU_NO_ERROR || ref == 0u)
		return seam_refuse("timer seam: Timer0 reference clock read failed");

	/* period is milliseconds.  Done in 64 bits and range-checked: a product
	 * that does not fit RELOAD must be refused, not wrapped into a wildly
	 * different watchdog interval. */
	t64 = ((uint64_t)cfg->period * (uint64_t)(ref / div_rb)) / 1000ull;
	if (t64 < 2ull || t64 > 0xFFFFFFFFull)
		return seam_refuse("timer seam: Timer0 period does not fit RELOAD");
	ticks = (uint32_t)t64;

	/* Stop first: everything below assumes the counter is not running and
	 * the line cannot fire between the vector swap and the enable. */
	T0_CTRL = 0u;
	__DSB();
	NVIC_DisableIRQ(TIMER0INT_IRQn);
	__DSB();
	__ISB();
	T0_INTSTATUS = 1u;
	NVIC_ClearPendingIRQ(TIMER0INT_IRQn);

	t0_cb      = cb_event;
	t0_oneshot = (cfg->mode == TIMER_MODE_ONESHOT) ? 1u : 0u;

	T0_RELOAD = ticks - 1u;
	T0_VALUE  = ticks - 1u;
	__DSB();
	if (T0_RELOAD != ticks - 1u)
		return seam_refuse("timer seam: Timer0 RELOAD did not read back");

	/*
	 * Install and register the vector, but leave the line MASKED at the
	 * NVIC until the counter has been checked below.  The timer is about to
	 * run, and a period short enough to expire during the check would
	 * otherwise fire the vendor's callback while this function is still
	 * deciding whether to accept the configuration at all.
	 */
	if (cb_event != NULL) {
		EPII_NVIC_SetVector(TIMER0INT_IRQn, vector);
		__DSB();
		__ISB();
		if (NVIC_GetVector(TIMER0INT_IRQn) != vector)
			return seam_refuse("timer seam: Timer0 vector swap did not take");
		/* Register BEFORE enabling: a 0 here means the accounting cannot
		 * cover this line, and an unaccounted enabled line is not a
		 * trade this port makes. */
		if (!tx_glue_profile_register_irq((int)TIMER0INT_IRQn, vector))
			return seam_refuse("timer seam: Timer0 irq not accountable");
		NVIC_SetPriority(TIMER0INT_IRQn, GROVE_TIMER0_IRQ_PRIORITY);
		T0_CTRL = T0_CTRL_VENDOR | T0_CTRL_IRQEN;
	} else {
		T0_CTRL = T0_CTRL_VENDOR;
	}
	__DSB();

	if (!t0_is_running(ticks, ref / div_rb)) {
		/* Undo everything: the counter is the one thing that cannot be
		 * verified by reading back a register, so a failure here means
		 * the configuration is not usable no matter how it read. */
		T0_CTRL = 0u;
		__DSB();
		NVIC_DisableIRQ(TIMER0INT_IRQn);
		T0_INTSTATUS = 1u;
		NVIC_ClearPendingIRQ(TIMER0INT_IRQn);
		if (cb_event != NULL)
			tx_glue_profile_unregister_irq((int)TIMER0INT_IRQn);
		t0_cb = NULL;
		return seam_refuse("timer seam: Timer0 was armed but is not "
		                   "counting steadily");
	}

	/* Proven running: only now does the line go live. */
	if (cb_event != NULL) {
		NVIC_ClearPendingIRQ(TIMER0INT_IRQn);
		NVIC_EnableIRQ(TIMER0INT_IRQn);
		__DSB();
		__ISB();
	}

	return TIMER_NO_ERROR;
}

/*
 * hx_drv_timer_hw_stop(TIMER_ID_0) -- ISR-SAFE.
 *
 * MMIO and NVIC only.  No logging, no ThreadX call, no SCU call, no loop: the
 * vendor reaches this from its own Timer0 callback.  The interrupt is dropped
 * from the EPK registry only by the caller that owns the lifecycle, not here --
 * unregistering touches a critical section, and this may run inside one.
 * Leaving the entry registered while the line is disabled is exactly the
 * benign case the registry allows.
 */
TIMER_ERROR_E __wrap_hx_drv_timer_hw_stop(TIMER_ID_E id)
{
	if (id != TIMER_ID_0)
		return seam_refuse("timer seam refused a non-Timer0 stop");

	T0_CTRL = 0u;
	__DSB();
	NVIC_DisableIRQ(TIMER0INT_IRQn);
	__DSB();
	__ISB();
	T0_INTSTATUS = 1u;
	NVIC_ClearPendingIRQ(TIMER0INT_IRQn);
	t0_cb = NULL;

	return TIMER_NO_ERROR;
}

/*
 * The two delay entry points.  The vendor spends TIMER_ID_3 on these; this port
 * spends DWT cycles, so no Himax timer is touched and both are ISR-safe.  The
 * `state` argument only selects which reference clock the vendor's timer would
 * have used -- irrelevant to a CPU cycle counter -- so it is accepted and
 * ignored rather than refused.
 */
TIMER_ERROR_E __wrap_hx_drv_timer_cm55x_delay_ms(uint32_t ms, TIMER_STATE_E state)
{
	(void)state;
	while (ms-- > 0u)
		udelay(1000u);
	return TIMER_NO_ERROR;
}

TIMER_ERROR_E __wrap_hx_drv_timer_cm55x_delay_us(uint32_t us, TIMER_STATE_E state)
{
	(void)state;
	udelay(us);
	return TIMER_NO_ERROR;
}

/*
 * board_delay_ms() -- referenced by libdriver.a (hx_drv_sensorctrl.o, the
 * xSleep path).  Its definition lives in the SDK's board/epii_evb/board.c,
 * which this port does not compile: that file also pulls in console_setup()
 * from the SDK clib this port replaces.  Same DWT busy-wait as above.
 */
void board_delay_ms(uint32_t ms)
{
	while (ms-- > 0u)
		udelay(1000u);
}
