/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    nn_handoff.h
 * @brief   Who may touch the inference worker's input, and when (issue #129;
 *          shared since issue #130, Epic #122 Phase 3b).
 *
 * The camera producer prepares a frame STRAIGHT INTO the model's input tensor
 * and hands it to the inference worker, which runs the invoke, the decode and
 * the publish.  There is no staging copy, so the input tensor -- and the
 * interpreter the producer asks about it -- has exactly one user at a time, and
 * this word says which.  grove-vision-ai-v2 wrote it for a producer that is
 * handed a whole frame at once; issue #130 added FILLING for one that is handed
 * a frame in parts (wio-lite-ai's four bands), and replaced that board's three
 * flags (`want_frame`, `filling`, `infer_active`) with it.
 *
 *   IDLE     the worker is parked and wants nothing.  Outside a stream.
 *   WANT     the worker is parked and wants a frame.  The producer may BEGIN
 *            one -- at the first part of a frame, never part way through.
 *   FILLING  the producer is writing the input, part by part.  Only the
 *            producer leaves it: HAND at the last part, or ABANDON.
 *   HANDED   the producer has finished writing and handed it over.  Nobody
 *            writes the input; the worker has not started on it yet.
 *   RUNNING  the worker is inside invoke .. publish.  Nobody writes the input:
 *            an inference may reuse that memory for its intermediates.
 *
 * [!] THE STATE IS THE TRUTH, NOT A SEMAPHORE COUNT.  The worker is woken by a
 * flag or a semaphore, and a wake-up can be stale (a hand-over that was already
 * taken, a stop that came in between).  A worker that wakes and cannot TAKE
 * goes back to sleep; it never runs on the strength of a token alone.
 *
 * [!] FILLING HAS EXACTLY TWO WAYS OUT BESIDES THE HAND.  ABANDON, by the
 * producer itself and only once it has stopped writing (a prep refused, a part
 * it could not use): the frame is dropped and the next one starts again from its
 * first part.  JOIN, by a stop and only after the board has confirmed the
 * producer is out (its stream stopped, its band drained, its camera lost).  A
 * worker whose wait ran out does NOT take FILLING away: it waits again.
 *
 * [!] EACH FUNCTION IS ONE WHOLE TRANSITION -- the test and the change -- and
 * the caller runs it inside ONE critical section.  Testing in one section and
 * changing in another would let the producer and the stop each see the other's
 * precondition and both proceed.  svc/nn_core_frame.c is the caller.
 *
 * WHY A PURE FUNCTION IN ITS OWN FILE.  None of the interesting sequences can be
 * typed: a stop that lands while a hand-over is in flight, a stale wake-up after
 * the worker already took its job, a part that arrives after its frame was
 * abandoned.  So the table is separated from the threads that run it, and
 * shell/test/test_nn_handoff.c walks every (state, operation) pair.
 *
 * No mutable storage.
 */
#ifndef NN_HANDOFF_H
#define NN_HANDOFF_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum nn_handoff_state {
	NN_HO_IDLE = 0,
	NN_HO_WANT,
	NN_HO_FILLING,
	NN_HO_HANDED,
	NN_HO_RUNNING,
};

/** What a thread asks of the word, with who may ask it. */
enum nn_handoff_op {
	/** Console or worker, when a stream or a one-shot starts: IDLE -> WANT.
	 *  Refused from anything else -- a start that finds the worker not
	 *  parked has broken an invariant and must not wait it out. */
	NN_HO_OP_ARM = 0,
	/** Producer, at the FIRST part of a frame: WANT -> FILLING.  Refused from
	 *  anything else, and a refusal means "the worker does not want this
	 *  frame" -- not one byte of the input is the producer's. */
	NN_HO_OP_BEGIN,
	/** Producer, after it has written the LAST part: FILLING -> HANDED. */
	NN_HO_OP_HAND,
	/** Producer, after it stopped writing a frame it cannot finish:
	 *  FILLING -> WANT.  The next frame starts again from its first part. */
	NN_HO_OP_ABANDON,
	/** Worker, on waking: HANDED -> RUNNING.  A refusal is a stale wake-up. */
	NN_HO_OP_TAKE,
	/** Worker, after the publish and after it has finished reading the
	 *  outputs: RUNNING -> WANT, asking for the next frame. */
	NN_HO_OP_DONE,
	/** Worker, the same but asking for nothing more: RUNNING -> IDLE.  What a
	 *  one-shot ends with, and what a worker that re-arms itself (sampling the
	 *  generation first) ends every job with. */
	NN_HO_OP_DONE_LAST,
	/** The stop, once the producer is confirmed out: IDLE, WANT or FILLING ->
	 *  IDLE.  Refused (and nothing changes) while a job is handed over or
	 *  running, which is what the stop then waits on. */
	NN_HO_OP_JOIN,
};

/**
 * Apply @p op to @p *state.
 *
 * Unknown states and unknown operations are refused and leave the word as it
 * is: a word nobody can explain is not evidence that the input is free, and a
 * JOIN refused for it keeps the stop waiting until its deadline says so.
 *
 * @return 1 moved (the new state is in *state), 0 refused (nothing changed)
 */
int nn_handoff_step(uint8_t *state, uint8_t op);

/**
 * Would a JOIN be accepted: is nothing handed over and nothing running?
 * Exposed for the test and for reports; the stop itself asks through
 * NN_HO_OP_JOIN, which tests and changes in one.  [!] FILLING answers yes, and
 * only because a JOIN is called once the producer is confirmed out -- it says
 * nothing about whether the input may be written.
 */
int nn_handoff_settled(uint8_t state);

#ifdef __cplusplus
}
#endif

#endif /* NN_HANDOFF_H */
