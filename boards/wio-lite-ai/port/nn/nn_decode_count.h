/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    nn_decode_count.h
 * @brief   How this board counts a decoder's answer in `nn stream stats`
 *          (issue #130 step 6c, #122 P8).
 *
 * The plugin's decode() returns a count or a negative BF_ERR_* (svc/blazeface.h),
 * and the worker has no console of its own: a refusal that is not counted apart
 * cannot be told apart afterwards (issue #97).  Until step 6c this board counted
 * neither -- `model_errors` and `decoder_errors` were always 0 and a plugin that
 * refused every frame read as a healthy stream.  It now counts by
 * grove-vision-ai-v2's rule (port/npu/nn_overlay.c):
 *
 *   count (>= 0)             not an error
 *   BF_ERR_MODEL             model_errors   -- load a different model
 *   any other negative       decoder_errors -- the firmware is wired wrong
 *   a one-shot (`nn run`)    nothing        -- it reports its own result
 *
 * and both error rows count in `errors` too.
 *
 * [!] THE NEGATIVES ARE NOT FOLDED.  BF_ERR_MODEL is the one a user can cause;
 * BF_ERR_UNINIT, BF_ERR_ARG and a code nobody documented are wiring faults, and
 * an unknown one is never read as "the model" (that would send the operator to
 * a different model for a firmware bug).  Neither is any of them "0 found".
 *
 * Pure, no storage: test/test_nn_decode_count.c walks it.  The counters and
 * where they move are the worker's (src/nn_camera.c, nncam_account()).
 */
#ifndef NN_DECODE_COUNT_H
#define NN_DECODE_COUNT_H

#ifdef __cplusplus
extern "C" {
#endif

/** Which `nn stream stats` counter one decode's answer moves. */
enum nn_decode_count {
	NN_DC_NONE = 0,   /**< not an error: a count, or not counted at all */
	NN_DC_MODEL,      /**< model_errors and errors                     */
	NN_DC_DECODER,    /**< decoder_errors and errors                   */
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
