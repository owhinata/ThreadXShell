/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Host stand-in for tx_api.h, for test_nn_model.c only (issue #131): the one
 * call port/nn/nn.c makes, the sleep of a thread that lost the open latch.  The
 * test is single-threaded, so the latch is never contended and this fails the
 * test if it is ever reached.
 */
#ifndef NN_SHIM_TX_API_H
#define NN_SHIM_TX_API_H

typedef unsigned long ULONG;
typedef unsigned int  UINT;

UINT tx_thread_sleep(ULONG ticks);

#endif
