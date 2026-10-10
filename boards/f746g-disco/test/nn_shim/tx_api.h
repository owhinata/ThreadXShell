/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Host stand-in for tx_api.h, for test_nn_model.c and test_nn_model_life.c
 * only (issue #131): the sleep of a thread that lost port/nn/nn.c's open latch,
 * and the interrupt-masking macros nn_svc_f746_model.c brackets its transition
 * counter with.  The tests are single-threaded, so the latch is never contended
 * (the sleep fails the test if it is ever reached), and the "mask" counts its
 * own depth so a test can tell that nothing is called inside it.
 */
#ifndef NN_SHIM_TX_API_H
#define NN_SHIM_TX_API_H

typedef unsigned long ULONG;
typedef unsigned int  UINT;

UINT tx_thread_sleep(ULONG ticks);

/* Depth of the shim's critical section; defined by the test that uses it. */
extern int test_tx_masked;

#define TX_INTERRUPT_SAVE_AREA  UINT interrupt_save;
#define TX_DISABLE              { interrupt_save = 0u; test_tx_masked++; }
#define TX_RESTORE              { (void)interrupt_save; test_tx_masked--; }

#endif
