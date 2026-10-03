/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    nn_run_wait.h
 * @brief   How `nn run`'s wait for its one inference ends (issue #129).
 *
 * `nn run` hands one frame to the inference worker and waits on the console for
 * the result to be published.  The wait can end five ways and each is its own
 * status, because each sends the operator somewhere else -- the same four
 * wio-lite-ai's `nn run` reports (issue #122 P7), plus the one this board can
 * tell apart because its worker says when it is done with a one-shot:
 *
 *   INFERRED   the record accepted a publish since this run's base
 *   NO_RESULT  the worker (or the producer, preparing the frame) finished with
 *              this run and published nothing -- an invoke that failed, a lease
 *              that was not had in time; the reason is the worker's
 *   LOST       the camera stopped streaming by itself before either
 *   CANCELLED  the operator pressed Ctrl+C
 *   TIMEOUT    the deadline passed
 *
 * [!] THE ORDER IS THE ANSWER WHEN SEVERAL HOLD AT ONCE.  A result that was
 * published wins over everything: it is sitting in the record, and a cancel or a
 * deadline that arrived in the same instant must not throw it away.  A finished
 * worker comes next: it is the reason nothing came.  Then the hardware fact
 * (lost), the operator (cancelled), the clock (timeout).
 *
 * [!] READ "FINISHED" BEFORE "PUBLISHED".  The worker publishes and only then
 * says it is finished, so a caller that samples in that order cannot see
 * finished-without-published for a run that did publish.
 *
 * Pure, and walked over every combination by test/test_nn_run_wait.c.
 */
#ifndef NN_RUN_WAIT_H
#define NN_RUN_WAIT_H

#ifdef __cplusplus
extern "C" {
#endif

enum nn_run_end {
	NN_RUN_WAITING = 0,
	NN_RUN_INFERRED,
	NN_RUN_NO_RESULT,
	NN_RUN_LOST,
	NN_RUN_CANCELLED,
	NN_RUN_TIMEOUT,
};

/** One poll of the wait.  Each argument is a yes/no; @return NN_RUN_WAITING
 *  to keep waiting, else how the wait ended. */
enum nn_run_end nn_run_wait_step(int published, int finished, int lost,
                                 int cancelled, int expired);

/**
 * The ending, re-read once more after the wait: a TIMEOUT whose result made it
 * into the record after all is INFERRED (the deadline and the publish can land
 * in the same moment).  Only a timeout is promoted -- a cancel is the
 * operator's decision and a lost stream is a hardware fact.
 */
enum nn_run_end nn_run_wait_final(enum nn_run_end end, int published);

#ifdef __cplusplus
}
#endif

#endif /* NN_RUN_WAIT_H */
