/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Host test for the vendor timer API seam (issue #30,
 * port/sdk_seam/timer_seam.c).
 *
 * WHY THIS EXISTS AS A HOST TEST.  The seam's refusal path is the one piece of
 * this port that cannot be exercised on hardware in M-G3a -- the camera
 * archives are not linked yet, so every wrapper is garbage-collected out of the
 * firmware.  cmake/check_timer_seam.py proves the LINK-level claim (no vendor
 * timer code survives the --wrap); this proves the BEHAVIOURAL one, which no
 * amount of symbol inspection can reach:
 *
 *   - a call for any timer other than TIMER_ID_0 is REFUSED and writes NOTHING,
 *     to Timer0 or to Timer2.  Timer2 is the execution-profile time source: a
 *     seam that "helpfully" reprogrammed it on a stray id would break cpu%
 *     accounting exactly where nobody is looking.
 *   - a configuration the seam does not reproduce exactly (PMU control, PMU
 *     state, an unsupported mode, a zero period) is refused rather than
 *     approximated.
 *   - the refusal is ISR-SAFE: it latches a string literal and returns.  There
 *     is no allocation, no lock and no blocking call to observe here, which is
 *     the point -- hx_drv_timer_hw_stop() is reached from the vendor's own
 *     Timer0 ISR callback.
 *   - the first reason is latched and later ones do not overwrite it, so `epk`
 *     reports the failure that started the trouble.
 *
 * The register block is pointed at a plain array (GROVE_TIMER_SEAM_T0_BASE), so
 * "wrote nothing" is checked byte for byte rather than argued.  The SDK headers
 * are shimmed here (shim/) because the real ones drag in the whole WE2 device
 * tree; the shim is deliberately tiny and every type in it is copied from the
 * SDK's hx_drv_timer.h, which is the ABI that actually matters.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "hx_drv_timer.h"
#include "timer_seam.h"
#include "seam_host_env.h"

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

TIMER_ERROR_E __wrap_hx_drv_timer_hw_start(TIMER_ID_E id, TIMER_CFG_T *cfg,
                                           Timer_ISREvent_t cb_event);
TIMER_ERROR_E __wrap_hx_drv_timer_hw_stop(TIMER_ID_E id);
TIMER_ERROR_E __wrap_hx_drv_timer_cm55x_delay_ms(uint32_t ms, TIMER_STATE_E st);
TIMER_ERROR_E __wrap_hx_drv_timer_cm55x_delay_us(uint32_t us, TIMER_STATE_E st);

static void cb_never(uint32_t ev) { (void)ev; seam_host_env.cb_calls++; }

/* A configuration the seam DOES accept, so each negative case below differs
   from a working one in exactly one field. */
static TIMER_CFG_T good_cfg(void)
{
	TIMER_CFG_T c;

	c.period = 500u;
	c.mode   = TIMER_MODE_PERIODICAL;
	c.ctrl   = TIMER_CTRL_CPU;
	c.state  = TIMER_STATE_DC;
	return c;
}

/* Every register the seam could possibly reach, zeroed, so "unchanged" is a
   memcmp and not a field-by-field opinion. */
static void arm_registers(void)
{
	memset(seam_host_env.regs, 0, sizeof seam_host_env.regs);
	memset(seam_host_env.vector, 0, sizeof seam_host_env.vector);
	memset(seam_host_env.nvic.ISER, 0, sizeof seam_host_env.nvic.ISER);
	memset(seam_host_env.registry, 0, sizeof seam_host_env.registry);
	seam_host_env.nvic_enabled = 0;
	seam_host_env.nvic_vector  = 0u;
	seam_host_env.cb_calls     = 0u;
	seam_host_env.registered   = 0;
	seam_host_env.fail_register_irqn = -1;
	seam_host_env.timer_step   = 0u;
	seam_host_env.timer_stalls = 0u;
	seam_host_env.udelay_calls = 0u;
}

static int registers_untouched(void)
{
	static const uint8_t zero[sizeof seam_host_env.regs];

	return memcmp(seam_host_env.regs, zero, sizeof zero) == 0 &&
	       seam_host_env.nvic_enabled == 0 &&
	       seam_host_env.nvic_vector == 0u;
}

/* ---- 1. a non-Timer0 id is refused and writes nothing ------------------- */

static void test_wrong_id_writes_nothing(void)
{
	TIMER_CFG_T cfg = good_cfg();
	int id;

	for (id = TIMER_ID_1; id < TIMER_ID_MAX; id++) {
		arm_registers();
		CHECK(__wrap_hx_drv_timer_hw_start((TIMER_ID_E)id, &cfg, cb_never)
		      == TIMER_ERROR_INVALID_PARAMETERS,
		      "hw_start(id=%d) was not refused", id);
		CHECK(registers_untouched(),
		      "hw_start(id=%d) touched hardware on the refusal path", id);
		CHECK(seam_host_env.scu_calls == 0u,
		      "hw_start(id=%d) reached the SCU driver before refusing", id);

		arm_registers();
		CHECK(__wrap_hx_drv_timer_hw_stop((TIMER_ID_E)id)
		      == TIMER_ERROR_INVALID_PARAMETERS,
		      "hw_stop(id=%d) was not refused", id);
		CHECK(registers_untouched(),
		      "hw_stop(id=%d) touched hardware on the refusal path", id);
	}

	/* The specific one the whole seam exists to protect.  TIMER_ID_2 is the
	 * execution-profile time source; port/threadx/tx_glue.c owns it alone. */
	arm_registers();
	(void)__wrap_hx_drv_timer_hw_stop(TIMER_ID_2);
	CHECK(registers_untouched(), "hw_stop(TIMER_ID_2) wrote to a timer");
}

/* ---- 2. configurations the seam does not reproduce are refused ---------- */

static void test_bad_config_refused(void)
{
	struct {
		const char *what;
		TIMER_CFG_T cfg;
	} cases[4];
	unsigned i;

	cases[0].what = "PMU control";
	cases[0].cfg = good_cfg();  cases[0].cfg.ctrl = TIMER_CTRL_PMU;
	cases[1].what = "PMU state";
	cases[1].cfg = good_cfg();  cases[1].cfg.state = TIMER_STATE_PMU;
	cases[2].what = "DELAY mode";
	cases[2].cfg = good_cfg();  cases[2].cfg.mode = TIMER_MODE_DELAY;
	cases[3].what = "zero period";
	cases[3].cfg = good_cfg();  cases[3].cfg.period = 0u;

	for (i = 0u; i < sizeof cases / sizeof cases[0]; i++) {
		arm_registers();
		CHECK(__wrap_hx_drv_timer_hw_start(TIMER_ID_0, &cases[i].cfg,
		                                   cb_never)
		      == TIMER_ERROR_INVALID_PARAMETERS,
		      "hw_start with %s was accepted", cases[i].what);
		CHECK(registers_untouched(),
		      "hw_start with %s touched hardware", cases[i].what);
	}

	arm_registers();
	CHECK(__wrap_hx_drv_timer_hw_start(TIMER_ID_0, NULL, cb_never)
	      == TIMER_ERROR_INVALID_PARAMETERS, "hw_start(NULL cfg) accepted");
	CHECK(registers_untouched(), "hw_start(NULL cfg) touched hardware");
}

/* ---- 3. the fault latch keeps the FIRST reason -------------------------- */

static void test_latch_keeps_first_reason(void)
{
	TIMER_CFG_T cfg = good_cfg();
	const char *first;
	uint32_t n0;

	arm_registers();
	(void)__wrap_hx_drv_timer_hw_stop(TIMER_ID_2);
	first = grove_timer_seam_fault();
	CHECK(first != NULL, "the seam latched no reason at all");

	n0 = grove_timer_seam_refusals();
	cfg.ctrl = TIMER_CTRL_PMU;
	(void)__wrap_hx_drv_timer_hw_start(TIMER_ID_0, &cfg, cb_never);
	CHECK(grove_timer_seam_fault() == first,
	      "a later refusal overwrote the first reason");
	CHECK(grove_timer_seam_refusals() > n0,
	      "the refusal counter did not advance");
}

/* ---- 4. the delays spend no Himax timer -------------------------------- */

static void test_delays_touch_no_timer(void)
{
	arm_registers();
	CHECK(__wrap_hx_drv_timer_cm55x_delay_us(3u, TIMER_STATE_DC)
	      == TIMER_NO_ERROR, "delay_us did not succeed");
	CHECK(__wrap_hx_drv_timer_cm55x_delay_ms(1u, TIMER_STATE_DC)
	      == TIMER_NO_ERROR, "delay_ms did not succeed");
	CHECK(registers_untouched(), "a delay wrote to a Himax timer");
	CHECK(seam_host_env.udelay_calls > 0u,
	      "the delays did not go through udelay()");
	CHECK(seam_host_env.scu_calls == 0u,
	      "the delays reached the SCU driver");
}

/* ---- 5a. a Timer0 that RUNS is accepted -------------------------------- */

/*
 * The mock advances VALUE from udelay(), which is what the seam uses to pace
 * its own samples, so a healthy counter can be modelled without intercepting
 * reads.  6 MHz for 200 us is 1200 counts per window -- the rate the seam
 * predicts from the SCU, so the amount check passes as well as the motion one.
 */
static void test_running_timer_accepted(void)
{
	TIMER_CFG_T cfg = good_cfg();

	arm_registers();
	seam_host_env.timer_step = 1200u;      /* 6 MHz * 200 us */
	seam_host_env.timer_stalls = 0u;       /* never stops */

	CHECK(__wrap_hx_drv_timer_hw_start(TIMER_ID_0, &cfg, cb_never)
	      == TIMER_NO_ERROR, "a Timer0 that counts steadily was refused");
	CHECK(seam_host_env.regs[0] == (1u | (1u << 2) | (1u << 3)),
	      "Timer0 CTRL is %08x, wanted the vendor value 0x0D",
	      seam_host_env.regs[0]);
	CHECK(seam_host_env.regs[2] != 0u, "Timer0 RELOAD was left at zero");
	CHECK(seam_host_env.nvic_enabled == 1,
	      "the Timer0 interrupt was not enabled after a successful check");
	CHECK(seam_host_env.registered == 1,
	      "the Timer0 interrupt was not registered with the EPK accounting");

	/* ... and the stop path takes it all back down, ISR-safely. */
	CHECK(__wrap_hx_drv_timer_hw_stop(TIMER_ID_0) == TIMER_NO_ERROR,
	      "a valid Timer0 stop was refused");
	CHECK(seam_host_env.regs[0] == 0u, "Timer0 CTRL was left enabled");
	CHECK(seam_host_env.nvic_enabled == 0,
	      "the Timer0 interrupt was left enabled after stop");
}

/* ---- 5b. a Timer0 that moves ONCE and stops is refused ------------------ */

/*
 * The case a single-sample check accepts and this one must not: the counter
 * advances for the first window and then freezes.  A frame watchdog in that
 * state reports armed and never expires, so the datapath would wait forever
 * for a timeout that cannot arrive.
 */
static void test_timer_that_stalls_is_refused(void)
{
	TIMER_CFG_T cfg = good_cfg();

	arm_registers();
	seam_host_env.timer_step = 1200u;
	seam_host_env.timer_stalls = 1u;       /* one window, then frozen */

	CHECK(__wrap_hx_drv_timer_hw_start(TIMER_ID_0, &cfg, cb_never)
	      == TIMER_ERROR_INVALID_PARAMETERS,
	      "a Timer0 that moved once and stopped was accepted");
	CHECK(seam_host_env.regs[0] == 0u,
	      "a refused Timer0 was left with CTRL %08x", seam_host_env.regs[0]);
	CHECK(seam_host_env.nvic_enabled == 0,
	      "a refused Timer0 left its interrupt enabled");
	CHECK(seam_host_env.registered == 0 ||
	      !((seam_host_env.registry[34 >> 5] >> (34 & 31)) & 1u),
	      "a refused Timer0 left its interrupt registered");
}

/* ---- 5c. a Timer0 running at the wrong rate is refused ------------------ */

static void test_timer_wrong_rate_refused(void)
{
	TIMER_CFG_T cfg = good_cfg();

	arm_registers();
	/* Moves, but two orders of magnitude slower than its clock says. */
	seam_host_env.timer_step = 4u;
	seam_host_env.timer_stalls = 0u;

	CHECK(__wrap_hx_drv_timer_hw_start(TIMER_ID_0, &cfg, cb_never)
	      == TIMER_ERROR_INVALID_PARAMETERS,
	      "a Timer0 counting far below its clock rate was accepted");
	CHECK(seam_host_env.regs[0] == 0u, "a refused Timer0 was left running");
}

/* ---- 5. an armed-but-stationary Timer0 is refused ----------------------- */

/*
 * The datapath uses Timer0 as its frame watchdog, so a timer that is armed but
 * NOT COUNTING is worse than one that was refused: the caller believes it has a
 * timeout and simply never gets one.  The seam therefore watches the counter
 * before reporting success.
 *
 * On the host that check is what the accepted path runs into -- the register
 * block is a plain array, so VALUE never moves and the start is (correctly)
 * refused.  That makes this the test of the fail-closed behaviour rather than
 * of the happy path, which is the more valuable of the two here: the happy
 * path's register values are fixed by the vendor disassembly quoted in
 * timer_seam.c, while "armed but dead" is a runtime state only this check
 * catches.
 */
static void test_stationary_timer_refused(void)
{
	TIMER_CFG_T cfg = good_cfg();

	arm_registers();
	seam_host_env.scu_calls = 0u;
	CHECK(__wrap_hx_drv_timer_hw_start(TIMER_ID_0, &cfg, cb_never)
	      == TIMER_ERROR_INVALID_PARAMETERS,
	      "a Timer0 that never counts was accepted");
	/* It got far enough to program the hardware -- RELOAD was derived from
	 * the period and the SCU was configured -- and then backed out. */
	CHECK(seam_host_env.regs[2] != 0u,
	      "Timer0 RELOAD was never programmed, so the refusal came too "
	      "early to be the counting check");
	CHECK(seam_host_env.scu_calls > 0u, "the SCU was never configured");
	CHECK(seam_host_env.regs[0] == 0u,
	      "a refused Timer0 was left with CTRL %08x", seam_host_env.regs[0]);
	CHECK(seam_host_env.nvic_enabled == 0,
	      "a refused Timer0 left its interrupt enabled");

	/* The stop path still works, and is ISR-safe about it. */
	CHECK(__wrap_hx_drv_timer_hw_stop(TIMER_ID_0) == TIMER_NO_ERROR,
	      "a valid Timer0 stop was refused");
	CHECK(seam_host_env.regs[0] == 0u, "Timer0 CTRL was left enabled");
	CHECK(seam_host_env.nvic_enabled == 0,
	      "the Timer0 interrupt was left enabled after stop");
}

/* ---- 6. an unaccountable interrupt refuses the whole start ------------- */

static void test_unaccountable_irq_refuses_start(void)
{
	TIMER_CFG_T cfg = good_cfg();

	arm_registers();
	seam_host_env.deny_registration = 1;
	CHECK(__wrap_hx_drv_timer_hw_start(TIMER_ID_0, &cfg, cb_never)
	      == TIMER_ERROR_INVALID_PARAMETERS,
	      "a Timer0 start whose IRQ cannot be accounted for was accepted");
	CHECK(seam_host_env.nvic_enabled == 0,
	      "an interrupt with no accounting wrapper was enabled anyway");
	CHECK((seam_host_env.regs[0] & 1u) == 0u,
	      "the counter was left running after a refused start");
	/* Refused at REGISTRATION, before the counting check -- so RELOAD was
	 * programmed but the interrupt never went live. */
	CHECK(seam_host_env.registered == 0,
	      "an unaccountable irq was registered anyway");
	seam_host_env.deny_registration = 0;
}

int main(void)
{
	test_wrong_id_writes_nothing();
	test_bad_config_refused();
	test_latch_keeps_first_reason();
	test_delays_touch_no_timer();
	test_running_timer_accepted();
	test_timer_that_stalls_is_refused();
	test_timer_wrong_rate_refused();
	test_stationary_timer_refused();
	test_unaccountable_irq_refuses_start();

	if (failures != 0) {
		printf("test_timer_seam: %d failure(s)\n", failures);
		return 1;
	}
	printf("test_timer_seam: OK\n");
	return 0;
}
