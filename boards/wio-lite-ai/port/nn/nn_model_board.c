/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Wio Lite AI ThreadX Shell Project
 */
/**
 * @file    nn_model_board.c
 * @brief   This board's half of svc/nn/nn.c: the dispatcher's state and its
 *          PRIMASK / tick / DWT hooks (issue #161).
 *
 * svc/nn/nn.c owns no storage and includes no HAL; what it needs from the core
 * is here (svc/nn/nn_dispatch_state.h states each hook's contract).
 *
 * WHY THIS DOES NOT ENABLE THE CYCLE COUNTER.  f746g-disco's hook enables DWT
 * itself: TRCENA, then the CoreSight lock key 0xC5ACCE55 to the Lock Access
 * Register, then CYCCNTENA.  Here svc/timebase.c owns the counter -- app/main.c
 * calls timebase_init() before the scheduler starts -- and udelay() is a
 * busy-wait whose only exit is CYCCNT advancing.  If the counter were frozen,
 * `usleep` would hang forever rather than return early, so a working `usleep`
 * is standing proof that CYCCNT runs on this part without the lock-key write.
 * Re-enabling it here would add a third copy of that sequence for nothing.
 * (cmd_membench.c keeps its own copy deliberately: it is a pinned instrument
 * and is meant to be self-sufficient.)
 *
 * WHAT THIS FILE MUST NEVER DO IS WRITE DWT->CYCCNT.  It is the shared
 * free-running timebase udelay() reads, so zeroing it would cut short a
 * concurrent `usleep N &`.  The start hook only *verifies* the counter
 * advances, so a broken DWT degrades to "reports 0 cycles" instead of
 * "reports a wrong number".
 */
#include "nn_dispatch_state.h"

#include "tx_api.h"          /* tx_thread_sleep (the first-open latch)      */
#include "stm32h7xx_hal.h"   /* DWT / __DSB / __ISB / PRIMASK               */

/* Plain .bss, where the old port/nn/nn.c statics were: the state is CPU-only
 * (no bus master reads it) and every field starts at 0. */
struct nn_dispatch_state nn_model_board_state;

/* PRIMASK, as this firmware's ThreadX port uses for its own critical sections
 * (TX_PORT_USE_BASEPRI is not defined), so this nests consistently with it. */
uint32_t nn_model_board_enter(void)
{
	uint32_t primask = __get_PRIMASK();

	__disable_irq();
	return primask;
}

void nn_model_board_exit(uint32_t saved)
{
	__set_PRIMASK(saved);
}

void nn_model_board_wait_tick(void)
{
	tx_thread_sleep(1);
}

/* Verify only -- see the file header.  A short spin is enough: at 550 MHz the
 * counter moves every ~1.8 ns, so any observable delay proves it. */
int nn_model_board_cycles_start(void)
{
	volatile uint32_t spin = 64u;
	uint32_t a;

	if (DWT->CTRL & DWT_CTRL_NOCYCCNT_Msk)
		return -1;                  /* CYCCNT not implemented on this core */

	a = DWT->CYCCNT;
	while (spin--)
		__NOP();

	return ((uint32_t)(DWT->CYCCNT - a) != 0u) ? 0 : -1;
}

/* DSB drains outstanding accesses and ISB flushes the pipeline, so the sampled
 * counter brackets the work and nothing either side leaks across (PM0253
 * sec 3.12.4 / 3.12.5) -- the same fencing cmd_membench.c uses. */
uint32_t nn_model_board_cycles(void)
{
	__DSB();
	__ISB();
	return DWT->CYCCNT;
}
