/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    nn_sess_release.h
 * @brief   When `nn stream`'s hold on the nn session may be given back
 *          (issue #130, step 6b).
 *
 * WHY THE DRAIN IS NOW PART OF THE QUESTION.  Since issue #130 the camera
 * producer preprocesses a frame STRAIGHT INTO the model's input tensor, which is
 * in the arena the session guards.  Before, it wrote a private staging buffer, so
 * releasing the session while a consume() might still be running was harmless.
 * Now `nn bench` (which writes the input and runs the model) and `nn model load`
 * (which rebuilds the arena) are kept out by the session alone -- the stream
 * lifecycle's PENDING keeps out only `nn stream start` and `nn run`.  So the
 * session may go back only once the sink drain has CONFIRMED the producer out,
 * and the worker is out of the model.
 *
 * [!] THE ANSWER COMES FROM STATE, NOT FROM PRIORITY.  The producer runs at
 * priority 10 and the drain's 100 ms budget is rarely spent, but "rarely" is not
 * a guard: a drain that did not confirm keeps the hold, and the stop that later
 * confirms it releases.
 *
 * Who releases, by interleaving:
 *   drain DONE, worker parked                -> the stop
 *   drain DONE, stop returned -2, worker
 *     parks later                            -> the worker
 *   drain not DONE (-7)                      -> the retrying stop whose drain
 *                                               confirms it
 *   worker parks between the stop's settle
 *     poll and its commit                    -> the worker (drain already set)
 *
 * Pure, no storage: the caller reads its flags and clears the hold under its
 * own lock, so exactly one caller gives the session back.
 */
#ifndef NN_SESS_RELEASE_H
#define NN_SESS_RELEASE_H

#ifdef __cplusplus
extern "C" {
#endif

/** Who is asking, and what it knows about the worker. */
enum nn_sess_who {
	/** The worker, leaving its run loop: it is out of the model. */
	NN_SESS_WORKER_OUT = 0,
	/** A stop that saw the worker parked. */
	NN_SESS_STOP_PARKED,
	/** A stop whose wait for the worker ran out: the worker is still in the
	 *  model, and releases on its own way out. */
	NN_SESS_STOP_BUSY,
};

/**
 * May the hold be given back now?
 *
 * @param sink_drained  a sink drain since this stream's start returned DONE
 * @param holds         the stream still holds the session
 * @param who           the caller (an unknown value answers no)
 * @return 1 release (and clear the hold, in the same hold of the lock), 0 keep
 */
int nn_sess_may_release(int sink_drained, int holds, enum nn_sess_who who);

#ifdef __cplusplus
}
#endif

#endif /* NN_SESS_RELEASE_H */
