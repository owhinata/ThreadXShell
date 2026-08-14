/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    cmd_epk.c
 * @brief   `epk` shell command: inspect and MEASURE the execution-profile time
 *          source (Grove Vision AI V2, issue #25).
 *
 * The whole cpu% column rests on one property that cannot be established by
 * reading code: that Himax TIMER2 KEEPS COUNTING WHILE THE CORE IS ASLEEP.
 * With TX_ENABLE_WFI the idle path is DSB;WFI;ISB inside PendSV, so if TIMER2
 * stopped there too, idle time would simply vanish from the accounting and
 * every busy thread's share would be inflated toward 100% -- and the table
 * would still look perfectly plausible.  Watching the `(idle)` row is NOT a
 * test of this: TIMER2 advancing only while ISRs and the scheduler run would
 * still produce a non-zero window.
 *
 * So `epk sleep <ms>` measures it directly: it samples the RAW TIMER2 count
 * either side of a tx_thread_sleep() of known length and compares the delta
 * against what the SCU reference clock and the divider predict.  Run it on a
 * WFI build and on a -DBSP_ENABLE_WFI=OFF build; matching deltas mean the
 * counter is indifferent to the core sleeping, which is exactly the property
 * being claimed.
 *
 * Plain `epk` prints the time source's configuration and the sleep-related
 * control state, so a disagreement can be attributed rather than guessed at.
 */
#include "cli.h"
#include "timer_seam.h"
#include "tx_glue.h"

#include "WE2_device.h"
#include "tx_api.h"

#include <stdlib.h>   /* strtoul */
#include <string.h>   /* strcmp  */

#define EPK_SLEEP_MS_DEFAULT 500u
#define EPK_SLEEP_MS_MAX     10000u

static void epk_print_status(struct cli_instance *sh)
{
	uint32_t scr = SCB->SCR;
	/* Called once: tx_glue_profile_ok() re-validates the hardware on every
	 * call (MMIO reads plus an NVIC sweep), so it is not a free predicate. */
	int ok = tx_glue_profile_ok(NULL);

	/* Verdict only.  `thread` prints the reason next to the cpu% column it
	 * blanks, which is where it is actually needed; repeating it here was
	 * duplication (issue #27). */
	cli_print(sh, "cpu%% accounting : %s\r\n",
	          ok ? "live" : "NOT trustworthy (run `thread` for the reason)");

	cli_print(sh, "TIMER2 rate     : %lu Hz  (SCU ref %lu Hz / div %lu)\r\n",
	          (unsigned long)tx_glue_epk_timer_hz(),
	          (unsigned long)tx_glue_epk_ref_hz(),
	          (unsigned long)tx_glue_epk_clkdiv());
	cli_print(sh, "TIMER2 count    : %08lx (free-running, as the kit reads "
	              "it)\r\n", (unsigned long)tx_glue_epk_timer_ticks());
	cli_print(sh, "ThreadX tick    : %lu ms period, %lu ticks since boot\r\n",
	          (unsigned long)(1000u / TX_TIMER_TICKS_PER_SECOND),
	          (unsigned long)tx_time_get());
#ifdef TX_ENABLE_WFI
	cli_print(sh, "idle policy     : WFI (TX_ENABLE_WFI)\r\n");
#else
	cli_print(sh, "idle policy     : busy spin (built with "
	              "-DBSP_ENABLE_WFI=OFF)\r\n");
#endif
	cli_print(sh, "SCB->SCR        : %08lx (SLEEPDEEP=%lu SLEEPONEXIT=%lu; "
	              "both must read 0)\r\n",
	          (unsigned long)scr,
	          (unsigned long)((scr & SCB_SCR_SLEEPDEEP_Msk) ? 1u : 0u),
	          (unsigned long)((scr & SCB_SCR_SLEEPONEXIT_Msk) ? 1u : 0u));

	/* The vendor timer seam (issue #30).  Its refusal path cannot log -- it is
	 * reachable from the vendor's own Timer0 ISR callback -- so the latch it
	 * keeps instead is surfaced here.  Anything other than "none" means a
	 * prebuilt archive asked for a timer configuration this port does not
	 * reproduce, and whatever depended on that timer is not running. */
	{
		const char *seam = grove_timer_seam_fault();

		cli_print(sh, "timer seam      : %s", (seam != NULL) ? seam : "clean");
		if (seam != NULL)
			cli_print(sh, " (%lu refusals)",
			          (unsigned long)grove_timer_seam_refusals());
		cli_print(sh, "\r\n");
	}
}

/*
 * Sleep for a known number of ticks and report how far TIMER2 moved.
 *
 * The expected count is derived from the SCU reference and the divider that
 * port/threadx/tx_glue.c actually read back -- not from a constant -- and the
 * ACTUAL elapsed tick count is used rather than the requested one, so a sleep
 * that overruns by a tick does not show up as clock error.  The ratio is
 * printed in per-mille: 1000 means the counter ran at exactly the predicted
 * rate for the whole sleep, including the part spent in WFI.
 */
static int epk_sleep_measure(struct cli_instance *sh, uint32_t ms)
{
	uint32_t hz = tx_glue_epk_timer_hz();
	uint32_t t0, t1, k0, k1, ticks, measured;
	uint64_t expected;
	uint32_t permille;

	if (hz == 0u) {
		cli_error(sh, "epk: TIMER2 is not running; nothing to measure\r\n");
		return 1;
	}
	if (!tx_glue_systick_ok()) {
		cli_error(sh, "epk: no ThreadX tick; tx_thread_sleep would never "
		              "return\r\n");
		return 1;
	}

	/* Sample the fast counter innermost so the tick reads bracket it. */
	k0 = (uint32_t)tx_time_get();
	t0 = tx_glue_epk_timer_ticks();

	tx_thread_sleep((ULONG)ms * TX_TIMER_TICKS_PER_SECOND / 1000u);

	t1 = tx_glue_epk_timer_ticks();
	k1 = (uint32_t)tx_time_get();

	measured = t1 - t0;                       /* mod 2^32: wrap-safe */
	ticks    = k1 - k0;
	expected = ((uint64_t)hz * (uint64_t)ticks) /
	           (uint64_t)TX_TIMER_TICKS_PER_SECOND;

	cli_print(sh, "slept %lu ms (%lu ticks measured by the ThreadX tick)\r\n",
	          (unsigned long)ms, (unsigned long)ticks);
	cli_print(sh, "  TIMER2 delta  : %lu counts\r\n",
	          (unsigned long)measured);
	cli_print(sh, "  expected      : %lu counts (%lu Hz x %lu ticks)\r\n",
	          (unsigned long)expected, (unsigned long)hz,
	          (unsigned long)ticks);
	if (expected == 0u) {
		cli_warn(sh, "  ratio         : n/a (window too short)\r\n");
		return 0;
	}
	permille = (uint32_t)(((uint64_t)measured * 1000ULL) / expected);
	cli_print(sh, "  measured/expected: %lu.%lu%% "
	              "(1000 per-mille = 100.0%%: the counter did not stop)\r\n",
	          (unsigned long)(permille / 10u), (unsigned long)(permille % 10u));
	return 0;
}

static int cmd_epk(struct cli_instance *sh, int argc, char **argv)
{
	if (argc == 1) {
		epk_print_status(sh);
		return 0;
	}
	if (argc >= 2 && strcmp(argv[1], "sleep") == 0) {
		uint32_t ms = EPK_SLEEP_MS_DEFAULT;

		if (argc >= 3) {
			char *end = NULL;
			unsigned long v = strtoul(argv[2], &end, 0);

			if (end == argv[2] || *end != '\0' || v == 0u ||
			    v > EPK_SLEEP_MS_MAX) {
				cli_error(sh, "epk: sleep takes 1..%lu ms\r\n",
				          (unsigned long)EPK_SLEEP_MS_MAX);
				return 1;
			}
			ms = (uint32_t)v;
		}
		return epk_sleep_measure(sh, ms);
	}

	cli_error(sh, "epk: usage: epk [sleep [ms]]\r\n");
	return 1;
}

CLI_CMD_REGISTER_USAGE(epk, NULL,
                       "execution-profile time source status / sleep measurement",
                       "[sleep [ms]]", cmd_epk, 1, 2);
