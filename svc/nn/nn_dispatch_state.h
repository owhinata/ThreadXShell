/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    nn_dispatch_state.h
 * @brief   What the board gives svc/nn/nn.c: the dispatcher's state and its
 *          hardware hooks (issue #161).
 *
 * svc/nn/nn.c is the one implementation of nn.h for wio-lite-ai and
 * f746g-disco, and like every shared translation unit it owns no mutable
 * storage (cmake/check_no_mutable_storage.py audits it per board).  The board
 * therefore DEFINES the state below as one named object, and nn.c reaches it
 * through the extern declaration here -- resolved at link time, the same way
 * nn_backend_vt_selected is.  Nothing is registered at init: a pointer the
 * shared file remembered would itself be mutable storage in the shared file.
 *
 * The board also defines the hooks: the facts of its core that nn.c needs and
 * may not include a HAL, CMSIS device header or tx_api.h to reach.  Their
 * definitions are boards/<board>/port/nn/nn_model_board.c.
 *
 * [!] ONLY nn.c AND THE BOARD'S DEFINITION INCLUDE THIS.  `struct nn_model`
 * stays opaque in nn.h, and the host tests that stand in for it (wio-lite-ai's
 * test_nn_active.c, test_mlperf.c) rely on that.
 */
#ifndef NN_DISPATCH_STATE_H
#define NN_DISPATCH_STATE_H

#include <stdint.h>

#include "nn.h"

#ifdef __cplusplus
extern "C" {
#endif

/** The public model handle: the backend's opaque handle and its timing. */
struct nn_model {
	void    *impl;
	uint32_t last_cycles;
	uint8_t  open;
};

/**
 * The dispatcher's state, grouped by why each part exists.  Zero is the boot
 * state of every field: the board defines the object without an initializer
 * (it lands in .bss, wherever the board's memory map puts plain data).
 *
 * Every field written under the critical section is volatile: a reader outside
 * it (the already-open fast path, the latch waiter) must load it again each
 * time it asks.
 */
struct nn_dispatch_state {
	/* Singleton publication: the one model a build runs.  `model.open` is the
	 * publication the first open's waiters poll. */
	struct nn_model model;

	/* First-open serialisation: exactly one thread runs backend init and open,
	 * any other waits a tick and asks again.  `inited` is nn_init()'s latch. */
	volatile int opening;
	uint8_t      inited;

	/* The single-session guard (nn_session_try_acquire()). */
	volatile int session_busy;

	/* An external claim on the backend's memory: f746g-disco's `sdram test`
	 * holding .sdram.ai (nn_scratch_claim(), issue #152).  A board with no such
	 * user never calls the claim and this stays 0. */
	volatile int scratch_claimed;

	/* Timing availability: the cycle counter was seen advancing at init.  0
	 * makes nn_last_cycles() report 0 instead of a frozen counter's delta. */
	uint8_t cycles_ok;
};

/** The board's one instance (boards/<board>/port/nn/nn_model_board.c). */
extern struct nn_dispatch_state nn_model_board_state;

/* ---- hooks the board defines ------------------------------------------------ */

/**
 * Enter a critical section that excludes every thread and interrupt that
 * calls nn.h: returns the mask state from BEFORE the call, which the matching
 * exit restores.  Restoring the saved value -- not unconditionally re-enabling
 * -- is what keeps a nested section, or a caller that already had interrupts
 * masked, masked on return.  Short and non-blocking; nothing in it may sleep.
 */
uint32_t nn_model_board_enter(void);
void     nn_model_board_exit(uint32_t saved);

/** Give up the CPU for one scheduler tick (a thread lost the first-open latch). */
void nn_model_board_wait_tick(void);

/**
 * Make sure the core's cycle counter runs, the way this board does that.
 * 0 when it was seen advancing, <0 when it is absent or frozen; timing is
 * best-effort, so nn_init() records the answer and goes on either way.  It
 * must never write the counter's value (wio-lite-ai's timebase reads it).
 */
int nn_model_board_cycles_start(void);

/**
 * The cycle counter, read behind a full barrier so nothing on either side of
 * the measured call leaks across the sample.  Wraps at 2^32.
 */
uint32_t nn_model_board_cycles(void);

#ifdef __cplusplus
}
#endif

#endif /* NN_DISPATCH_STATE_H */
