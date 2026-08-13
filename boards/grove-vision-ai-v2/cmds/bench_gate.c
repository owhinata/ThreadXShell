/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    bench_gate.c
 * @brief   Shared entry gate for `coremark` and `membench` (issue #25).
 *
 * Two things have to hold before either benchmark means anything:
 *
 * 1. THE THREADX TICK MUST BE RUNNING.  CoreMark's time base is tx_time_get()
 *    and its auto-calibration loop spins until a second of wall time has
 *    passed -- with a dead tick that loop never exits, so a missing SysTick
 *    turns `coremark` into a hang.  membench uses the tick only to reject runs
 *    that a SysTick interrupted, but a frozen tick would silently accept every
 *    run as "clean".  port/threadx/tx_glue.c already refuses to start SysTick
 *    from an implausible clock, so this is a read of that decision.
 *
 * 2. THE CORE FREQUENCY MUST BE THE ONE THE NUMBERS ASSUME.  DWT CYCCNT counts
 *    core clocks, so membench's MB/s and ns figures scale directly with it, and
 *    CoreMark's iterations/MHz does too.  SystemCoreClock is whatever the SDK's
 *    platform_driver_init() put there:
 *
 *        uint32_t freq;
 *        hx_drv_scu_get_freq(SCU_CLK_FREQ_TYPE_HSC_CLK, &freq);   <-- return
 *        SystemCoreClockUpdate(freq);                              value dropped
 *
 *    -- a failed SCU read is indistinguishable from a successful one there.  So
 *    this gate reads the CM55M frequency itself, CHECKS THE RETURN VALUE, and
 *    requires the two to agree.  In this SDK both selectors resolve to the same
 *    hsc_clk read, so a disagreement means one of the reads went wrong (or a
 *    future part gained a CM55M divider) -- either way the absolute numbers
 *    would be off by an unknown factor, and both values are printed so the
 *    reader can see which.
 *
 * Deliberately NOT done: calibrating DWT against the ThreadX tick.  The SysTick
 * reload is itself computed from SystemCoreClock, so that comparison is
 * circular and would "confirm" any wrong value.
 *
 * [!] WHAT THIS GATE CANNOT DO is establish that the SCU's number is TRUE.
 * Both selectors resolve to the same hsc_clk read in this SDK, so they can
 * agree on the same wrong value; a genuinely independent check needs a second
 * time reference (an RTC on its own oscillator, say), and this board has no
 * such reference brought up.  The results are therefore "measured under a
 * stated clock", not "verified absolute" -- which is why every command that
 * uses this gate prints the frequency next to its numbers and the board README
 * says the same thing.  What the gate does add is that the clock is READ
 * PROPERLY (return value checked), is plausible, and -- via
 * bench_gate_recheck() -- did not move during the run.
 */
#include "bench_gate.h"

#include "cli.h"
#include "tx_glue.h"

#include "WE2_device.h"
#include "hx_drv_scu.h"

/* Same plausibility window port/threadx/tx_glue.c applies to SysTick: the
 * compile-time SDK config is a 24 MHz placeholder and the datasheet caps the
 * CM55M at 400 MHz plus DVFS margin. */
#define BENCH_HZ_MIN 1000000u
#define BENCH_HZ_MAX 500000000u

int bench_gate_check(struct cli_instance *sh, const char *cmd,
                     uint32_t *core_hz)
{
	uint32_t scu_hz = 0u;
	uint32_t sys_hz = SystemCoreClock;

	if (!tx_glue_systick_ok()) {
		cli_error(sh, "%s: no ThreadX tick (the core clock read-back was "
		              "rejected at boot); refusing to run\r\n", cmd);
		return 0;
	}

	if (hx_drv_scu_get_freq(SCU_CLK_FREQ_TYPE_HSC_CM55M, &scu_hz)
	    != SCU_NO_ERROR) {
		cli_error(sh, "%s: SCU CM55M frequency read failed; refusing to "
		              "run\r\n", cmd);
		return 0;
	}
	if (scu_hz < BENCH_HZ_MIN || scu_hz > BENCH_HZ_MAX) {
		cli_error(sh, "%s: SCU CM55M frequency %lu Hz is implausible; "
		              "refusing to run\r\n", cmd, (unsigned long)scu_hz);
		return 0;
	}
	if (scu_hz != sys_hz) {
		cli_error(sh, "%s: SCU CM55M %lu Hz disagrees with SystemCoreClock "
		              "%lu Hz (SDK read it as HSC_CLK); every absolute "
		              "result would be off by that ratio -- refusing to "
		              "run\r\n",
		          cmd, (unsigned long)scu_hz, (unsigned long)sys_hz);
		return 0;
	}

	*core_hz = scu_hz;
	return 1;
}

int bench_gate_recheck(struct cli_instance *sh, const char *cmd,
                       uint32_t core_hz)
{
	uint32_t scu_hz = 0u;

	if (hx_drv_scu_get_freq(SCU_CLK_FREQ_TYPE_HSC_CM55M, &scu_hz)
	    != SCU_NO_ERROR) {
		cli_warn(sh, "%s: SCU CM55M frequency read failed after the run; "
		             "cannot confirm the clock held\r\n", cmd);
		return 0;
	}
	if (scu_hz != core_hz) {
		cli_warn(sh, "%s: core clock moved during the run (%lu -> %lu Hz); "
		             "the results above are scaled by the OLD value and are "
		             "not usable\r\n",
		         cmd, (unsigned long)core_hz, (unsigned long)scu_hz);
		return 0;
	}
	return 1;
}
