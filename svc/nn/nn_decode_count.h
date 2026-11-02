/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    nn_decode_count.h
 * @brief   How a decoder's answer is counted in `nn stream stats`
 *          (issue #130 step 6c, #122 P8; one table since issue #159 Step 4c).
 *
 * A decoder reaches wio-lite-ai and f746g-disco only in a plugin container
 * (issue #159 Step 4c removed f746g-disco's resident one).  Its decode()
 * returns a count or a negative BF_ERR_* (svc/nn/blazeface.h -- the
 * vocabulary, not the decoder), and the worker has no console of its own: a
 * refusal that is not counted apart cannot be told apart afterwards (issue
 * #97).  The table is grove-vision-ai-v2's rule:
 *
 *   count (>= 0)             not an error
 *   BF_ERR_MODEL             model_errors   -- load a different model
 *   any other negative       decoder_errors -- the firmware is wired wrong
 *
 * and either error counts in `errors` too.  What one publish adds to `nn stream
 * stats` is the tally below, the one rule of all three boards (issue #164):
 *
 *   the record did not take it    nothing -- a reader saw nothing
 *   a one-shot (`nn run`)         nothing -- it reports its own result
 *   a stream's raw publish        infers  -- found nothing, refused nothing
 *   a stream's decode, count      infers  (and its detections)
 *   a stream's decode, refused    the error above, NOT infers (issue #138)
 *
 * [!] BF_ERR_MODEL IS A MISMATCH, NOT A RESULT.  A plugin was shipped with the
 * model it reads, so its "not my model" means the container's two halves do not
 * belong together -- it must not read as an inference that found nobody.  (Until
 * Step 4c f746g-disco's resident decoder ran on every model and its BF_ERR_MODEL
 * was the top-5 result of a classifier stream; that table went with it.)
 *
 * [!] THE OTHER NEGATIVES ARE NOT FOLDED INTO IT, nor into "0 found": BF_ERR_UNINIT,
 * BF_ERR_ARG and a code nobody documented are wiring faults, and an unknown one
 * is never read as "the model" (that would send the operator to a different
 * model for a firmware bug).
 *
 * Pure, no storage: shell/test/test_nn_decode_count.c walks the table and the
 * tally.  The counters and where they move are each board's worker
 * (boards/<board>/port/nn/nn_worker.c: nncam_account() on wio-lite-ai and
 * f746g-disco, nn_ov_account() on grove-vision-ai-v2), and all three ask the
 * tally (issue #164).  Moved here from wio-lite-ai's and f746g-disco's port/nn/
 * (two copies of one table) by issue #163; the values of enum nn_decode_count
 * are wio-lite-ai's.
 */
#ifndef NN_DECODE_COUNT_H
#define NN_DECODE_COUNT_H

#ifdef __cplusplus
extern "C" {
#endif

/** Which `nn stream stats` counter one decode's answer moves. */
enum nn_decode_count {
	NN_DC_NONE = 0,   /**< not an error: a result                        */
	NN_DC_MODEL,      /**< model_errors and errors                       */
	NN_DC_DECODER,    /**< decoder_errors and errors                     */
};

/**
 * @param nd  the decoder's answer: a count, or a negative code
 * @return the counter it moves (a stream's; whether anything is counted at
 *         all is nn_decode_tally_of()'s)
 */
enum nn_decode_count nn_decode_count_of(int nd);

/** What the worker adds to `nn stream stats` for one publish. */
enum nn_decode_tally {
	NN_TALLY_NOTHING = 0,  /**< not taken, or a one-shot: nothing counts   */
	NN_TALLY_INFER,        /**< infers (and the detections it found)       */
	NN_TALLY_DECODER,      /**< decoder_errors and errors                  */
	NN_TALLY_MODEL,        /**< model_errors and errors                    */
};

/**
 * @brief  One publish's tally (issue #159 Step 4b-iii, #122 P18).
 *
 * [!] ONLY A PUBLISH THE RECORD TOOK IS COUNTED -- a decode's as a raw
 * publish's, and as on wio-lite-ai.  A publish the generation rule dropped
 * (a stop or a re-attach landed during the inference) produced nothing a reader
 * can see, so counting it would leave `nn stream stats` saying "1 infer" beside
 * `last : nothing decoded yet`.  @p dc is the table's answer for the decode
 * (nn_decode_count_of()), NN_DC_NONE for a raw publish.
 *
 * [!] A ONE-SHOT COUNTS NOTHING -- not an inference, not an error, raw or
 * decoded (issue #164).  `nn run` prints its own result; counting it would make
 * the next `nn stream stats` (its base is taken at the stream's commit) report
 * an inference the stream never made.
 *
 * @param took     non-zero when the record took the publish
 * @param oneshot  non-zero for a `nn run`'s session
 */
enum nn_decode_tally nn_decode_tally_of(int took, int oneshot,
                                        enum nn_decode_count dc);

#ifdef __cplusplus
}
#endif

#endif /* NN_DECODE_COUNT_H */
