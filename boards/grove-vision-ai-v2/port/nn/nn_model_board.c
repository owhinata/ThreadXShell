/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    nn_model_board.c
 * @brief   What this board gives svc/nn/nn.c (issue #169): its state object and
 *          the four hooks of svc/nn/nn_dispatch_state.h.
 *
 * The dispatcher is shared with wio-lite-ai and f746g-disco and owns no storage;
 * the object it works on and the core's primitives are here, bound at link time.
 *
 * [!] THE CYCLE COUNTER IS REPORTED UNAVAILABLE, ON PURPOSE.  nn.c times nn_run()
 * with it, and on this board that number would be wrong in the direction that
 * looks good: the DWT stops while the core is in WFI, and an inference spends
 * most of its time with the calling thread asleep on the driver's semaphore and
 * the core idle.  So nn_model_board_cycles_start() refuses, nn.c records that the
 * counter is not usable and nn_last_cycles() answers 0.  `nn bench` and the
 * stream's figures keep their own clocks (ThreadX ticks, the EPK's TIMER2; the
 * board README's time-source section), which is where a real time comes from.
 */
#include "nn_dispatch_state.h"

#include "tx_api.h"          /* tx_thread_sleep (the first-open latch) */
#include "WE2_device.h"      /* PRIMASK                                 */

/* Plain .bss: the state is CPU-only. */
struct nn_dispatch_state nn_model_board_state;

/* PRIMASK, as this firmware's ThreadX port uses for its own critical sections
 * (TX_PORT_USE_BASEPRI undefined).  Restores what it found, so it nests inside
 * this board's TX_DISABLE sections -- the nn gate takes the session inside one. */
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

/* Unavailable -- see the file header. */
int nn_model_board_cycles_start(void)
{
	return -1;
}

uint32_t nn_model_board_cycles(void)
{
	return 0u;
}
