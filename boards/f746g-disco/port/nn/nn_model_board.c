/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    nn_model_board.c
 * @brief   This board's half of svc/nn/nn.c: the dispatcher's state and its
 *          PRIMASK / tick / DWT hooks (issue #161).
 *
 * svc/nn/nn.c owns no storage and includes no HAL; what it needs from the core
 * is here (svc/nn/nn_dispatch_state.h states each hook's contract).
 *
 * DWT: the Cortex-M7 DWT has a software lock that leaves CYCCNT frozen at 0
 * with no debugger attached until the CoreSight lock key (0xC5ACCE55) is
 * written to the Lock Access Register.  Same technique as cmd_membench.c
 * (owhinata/stm32f746g-disco#57).  The counter only stalls in WFI; nn_run() is
 * CPU-bound and never sleeps, so the elapsed count is the true inference
 * latency (inflated by any preemption, which for a best-effort worker is
 * exactly what we want to measure).  The counter is never written.
 */
#include "nn_dispatch_state.h"

#include "tx_api.h"          /* tx_thread_sleep (the first-open latch)      */
#include "stm32f7xx_hal.h"   /* DWT / CoreDebug / __DSB / __ISB / PRIMASK   */

/* Plain .bss: the state is CPU-only and every field starts at 0. */
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

#define NN_DWT_LAR_ADDR 0xE0001FB0u   /* DWT Lock Access Register */
#define NN_DWT_LAR_KEY  0xC5ACCE55u   /* CoreSight lock access key */

int nn_model_board_cycles_start(void)
{
	int attempt;

	if (DWT->CTRL & DWT_CTRL_NOCYCCNT_Msk)
		return -1;                  /* CYCCNT not implemented */

	for (attempt = 0; attempt < 3; attempt++) {
		volatile uint32_t spin = 64u;
		uint32_t a;

		CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
		*((volatile uint32_t *)NN_DWT_LAR_ADDR) = NN_DWT_LAR_KEY; /* unlock (M7) */
		DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;

		a = DWT->CYCCNT;
		while (spin--)
			__NOP();
		if (DWT->CYCCNT != a)
			return 0;           /* counting */
	}
	return -1;
}

uint32_t nn_model_board_cycles(void)
{
	__DSB();
	__ISB();
	return DWT->CYCCNT;
}
