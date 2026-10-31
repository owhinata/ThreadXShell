/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Host stand-in for tx_api.h, for test_nn_model_life.c and
 * test_nn_model_container.c (issue #131): the interrupt-masking macros
 * nn_svc_f746_model.c brackets its transition counter with.  The "mask" counts
 * its own depth so a test can tell that nothing is called inside it.  (The
 * open latch's one-tick wait is no longer reached through here: svc/nn/nn.c
 * calls the board hook, modelled by shell/test/nn_model_host.c, issue #161.)
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
