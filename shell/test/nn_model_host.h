/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Host stand-in for a board's port/nn/nn_model_board.c (issue #161): the
 * dispatcher's state object and its hooks, for every host test that links the
 * real svc/nn/nn.c (shell/test/test_nn_model.c, and f746g-disco's
 * test_nn_model_life.c / test_nn_model_container.c).
 *
 * The critical section is modelled as a mask bit and a depth, so a test can
 * tell that each section restores the value it found -- a caller that already
 * had the mask set must still have it set afterwards -- and that none is left
 * open.  The one-tick wait fails the test (the tests are single-threaded, so
 * the first-open latch is never contended) -- unless a test has armed
 * nn_host_wait_jmp: then the wait is counted and jumps back there, so a test
 * can start a second open while the first one is building and see that it
 * waits (issue #253).  The cycle counter is a variable the stub advances by a
 * fixed step per read.
 */
#ifndef NN_MODEL_HOST_H
#define NN_MODEL_HOST_H

#include <setjmp.h>
#include <stdint.h>

extern uint32_t nn_host_mask;          /* 1 while "interrupts" are masked      */
extern int      nn_host_depth;         /* sections entered and not yet exited  */
extern int      nn_host_enters;        /* sections entered, ever               */
extern int      nn_host_cycles_start_rc; /* what cycles_start answers (0: runs) */
extern uint32_t nn_host_cycles_step;   /* how far each read advances the counter */
extern jmp_buf *nn_host_wait_jmp;      /* armed: the one-tick wait jumps here  */
extern int      nn_host_waits;         /* one-tick waits taken while armed     */

/* The dispatcher's first-open latch, read from the board's state object: 1
 * while one thread is running the backend's init and open. */
int nn_host_opening(void);

#endif /* NN_MODEL_HOST_H */
