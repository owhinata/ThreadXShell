/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    nn_decode_count.h
 * @brief   How this board counts its resident decoder's answer in `nn stream
 *          stats` (issue #130 step 6c, #122 P8).
 *
 * The resident BlazeFace decoder (port/nn/nn_decoder.c over svc/blazeface.c)
 * returns a count or a negative BF_ERR_*.  Until step 6c this board counted no
 * negative at all -- `decoder_errors` was always 0, so a decoder that was never
 * initialised (a build fault, on every frame) read as a healthy stream.
 *
 *   count (>= 0)             not an error
 *   BF_ERR_MODEL             NOT AN ERROR (D6) -- see below
 *   any other negative       decoder_errors -- the firmware is wired wrong
 *   a one-shot (`nn run`)    nothing        -- it reports its own result
 *
 * and a decoder error counts in `errors` too.
 *
 * [!] BF_ERR_MODEL IS A RESULT HERE, NOT A REFUSAL (D6).  On this board the
 * decoder is resident and runs on every model; for one that is not BlazeFace-
 * shaped it answers BF_ERR_MODEL and the worker publishes the top 5 classes of
 * output 0 with it (issue #121) -- that is the normal result of a classifier
 * stream, and counting it would make `model_errors` equal `infers` on every one.
 * grove-vision-ai-v2 and wio-lite-ai count it, because there a decoder is a
 * plugin chosen for the model and BF_ERR_MODEL means the two do not match.
 *
 * [!] THE OTHER NEGATIVES ARE NOT FOLDED INTO IT, nor into "0 found": BF_ERR_UNINIT,
 * BF_ERR_ARG and a code nobody documented are wiring faults.
 *
 * Pure, no storage: test/test_nn_decode_count.c walks it.  The counters and
 * where they move are the worker's (port/nn/nn_camera.c, nncam_publish()).
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
};

/**
 * @param nd       the decoder's answer: a count, or a negative code
 * @param oneshot  non-zero for a `nn run`'s session
 * @return the counter it moves
 */
enum nn_decode_count nn_decode_count_of(int nd, int oneshot);

#ifdef __cplusplus
}
#endif

#endif /* NN_DECODE_COUNT_H */
