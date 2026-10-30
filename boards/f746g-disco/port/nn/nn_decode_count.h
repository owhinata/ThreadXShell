/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    nn_decode_count.h
 * @brief   How this board counts a decoder's answer in `nn stream stats`
 *          (issue #130 step 6c, #122 P8; one table since issue #159 Step 4c).
 *
 * A decoder reaches this board only in a plugin container (issue #159 Step
 * 4c removed the resident one).  Its decode() returns a count or a negative
 * BF_ERR_* (svc/nn/blazeface.h -- the vocabulary, not the decoder):
 *
 *   count (>= 0)             not an error
 *   BF_ERR_MODEL             model_errors   -- load a different model
 *   any other negative       decoder_errors -- the firmware is wired wrong
 *   a one-shot (`nn run`)    nothing        -- it reports its own result
 *
 * and either error counts in `errors` too.  wio-lite-ai's and
 * grove-vision-ai-v2's table.
 *
 * [!] BF_ERR_MODEL IS A MISMATCH, NOT A RESULT.  A plugin was shipped with the
 * model it reads, so its "not my model" means the container's two halves do not
 * belong together -- it must not read as an inference that found nobody.  (Until
 * Step 4c the resident decoder ran on every model and its BF_ERR_MODEL was the
 * top-5 result of a classifier stream; that table went with it.)
 *
 * [!] THE OTHER NEGATIVES ARE NOT FOLDED INTO IT, nor into "0 found": BF_ERR_UNINIT,
 * BF_ERR_ARG and a code nobody documented are wiring faults.
 *
 * Pure, no storage: test/test_nn_decode_count.c walks the table and the tally.
 * The counters and where they move are the worker's (port/nn/nn_worker.c,
 * nncam_account()).
 */
#ifndef NN_DECODE_COUNT_H
#define NN_DECODE_COUNT_H

#ifdef __cplusplus
extern "C" {
#endif

/** Which `nn stream stats` counter one decode's answer moves. */
enum nn_decode_count {
	NN_DC_NONE = 0,   /**< not an error: a result, or not counted at all */
	NN_DC_DECODER,    /**< decoder_errors and errors                     */
	NN_DC_MODEL,      /**< model_errors and errors                       */
};

/**
 * @param nd       the decoder's answer: a count, or a negative code
 * @param oneshot  non-zero for a `nn run`'s session
 * @return the counter it moves
 */
enum nn_decode_count nn_decode_count_of(int nd, int oneshot);

/** What the worker adds to `nn stream stats` for one publish. */
enum nn_decode_tally {
	NN_TALLY_NOTHING = 0,  /**< the record did not take it: nothing counts */
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
 */
enum nn_decode_tally nn_decode_tally_of(int took, enum nn_decode_count dc);

#ifdef __cplusplus
}
#endif

#endif /* NN_DECODE_COUNT_H */
