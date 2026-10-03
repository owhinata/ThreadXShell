/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    nn_worker.h
 * @brief   The inference worker thread (issue #129, Epic #122 U1).
 *
 * The third of the producer / worker / panel threads wio-lite-ai already has:
 * the camera producer prepares a frame into the model's input and hands it
 * over, this thread runs the invoke, the decode and the publish, and the panel
 * draws whatever was published last.  The hand-over rules are nn_handoff.h's.
 *
 * A stream arms it at start (IDLE -> WANT) and its stop joins it after the
 * producer is confirmed out and before the record boundary.  The job it runs
 * is nn_overlay_work().  `nn run` arms it the same way and it parks after
 * that one frame (DONE_LAST).
 *
 * OWNERSHIP.  Everything here is static and never freed, like the overlay's: a
 * stop whose join never came back leaves the worker possibly still running, and
 * what it touches must still be there.
 */
#ifndef NN_WORKER_H
#define NN_WORKER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * [!] BELOW THE PRODUCER, ABOVE THE CONSOLE, APART FROM THE mve WORKER (issue
 * #129 decision 1, option C).  The camera's and the panel's timing is then
 * what it is today -- the worker never preempts either -- and a console that
 * waits for the plugin lease the worker holds inherits at most this, still
 * below the camera and the panel.  The spike measured the cost of waking late:
 * +0.3 % on the detector's invoke.  The order panel < producer < worker < mve
 * < console is asserted where each number lives (here and cmds/cmd_mve.c).
 */
#define NN_WORKER_PRIO 12u

/*
 * At least the console's stack.  From stage 3 on, the plugin's decode runs on
 * this thread ONLY, so this becomes the shallowest ceiling the decode allowance
 * is declared against; at the console's size or more, that allowance stays
 * what it is today (issue #129 consequence (ii)).  The producer's size, which
 * is the work it inherits.  A *_stack array, so it lands in DTCM
 * (cmake/check_placement_budget.py).
 */
#define NN_WORKER_STACK_BYTES 8192u

/** Create the worker and its wake-up flags.  From tx_application_define(),
 *  before any thread runs.  A failure is logged and leaves nn_worker_ready()
 *  false -- a board that cannot run the worker must refuse to start a stream,
 *  not infer without one. */
void nn_worker_create_objects(void);

/** Whether the worker exists. */
int nn_worker_ready(void);

/**
 * Wait, at most NN_WORKER_JOIN_TICKS of wall-clock time, until the worker is
 * parked with nothing handed over and nothing running, and then leave it
 * wanting nothing (IDLE).
 *
 * [!] ONLY AFTER THE PRODUCER IS CONFIRMED OUT.  That is what makes the parked
 * state stable: nothing can hand over a new job once the producer is gone.  It
 * is decided by the hand-over word, never by counting wake-ups.
 *
 * @return 0 joined; -1 the deadline passed and the worker may still be inside
 *         the NPU or the plugin (the stop's terminal "worker did not return")
 */
int nn_worker_join(void);

/** IDLE -> WANT, when a stream or a one-shot starts.  @return 1 armed, 0
 *  refused: the worker is not parked or does not exist. */
int nn_worker_arm(void);

/** Whether the worker wants a frame -- the producer's licence to write the
 *  input tensor.  Stable once seen: only nn_worker_hand() leaves WANT while a
 *  producer runs. */
int nn_worker_wants(void);

/** The producer's half: WANT -> HANDED and wake the worker.  Call only after
 *  the input is written.  @return 1 handed over, 0 the worker did not want a
 *  frame (it is busy, or not armed) and the input was not the caller's. */
int nn_worker_hand(void);

#ifdef __cplusplus
}
#endif

#endif /* NN_WORKER_H */
