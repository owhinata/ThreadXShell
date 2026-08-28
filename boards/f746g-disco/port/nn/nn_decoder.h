/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    nn_decoder.h
 * @brief   This board's adapter onto the shared BlazeFace decoder (issue #97).
 *
 * The decoder itself is svc/blazeface.c, shared with wio-lite-ai and
 * grove-vision-ai-v2.  It reads svc/tensor.h descriptors, so somebody has to turn
 * this board's `struct nn_tensor` into them, and somebody has to own the
 * decoder's state and its candidate scratch -- the shared translation unit owns
 * no storage at all, which is what lets the scratch stay in `.sdram.ai` here and
 * somewhere else on the other two boards.  This file is both.
 *
 *
 * [!] THIS FILE IS TWO LINES DIFFERENT FROM THE OTHER nn.h BOARD'S COPY of it,
 * and that is deliberate rather than overlooked.  The two differ only in where the
 * scratch is placed, because `struct nn_tensor` and `enum nn_dtype` happen to be
 * identical on both -- but `port/nn/nn.h` as a whole is not (184 of its 382 lines
 * differ), and neither is nn.c.  Folding these two adapters therefore means
 * folding that API first, which is a larger and separate question than issue #97
 * set out to answer.  Recorded here so the next person weighs it rather than
 * rediscovering it.
 *
 * IT TAKES THE MODEL HANDLE, not a tensor array, because that is what both
 * callers already have: the decoder is singleton-independent but this board's
 * inference API is not, and pulling the outputs is the adapter's job.
 */
#ifndef NN_DECODER_H
#define NN_DECODER_H

#include "blazeface.h"
#include "nn.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Decode the open model's outputs into @p out[0..max).
 *
 * @param res  filled on every path, failures included, so a caller never reports
 *             diagnostics left over from an earlier frame.  It also carries the
 *             threshold THIS decode applied.
 *
 * @return the number of detections (>= 0), or a BF_ERR_* code.  [!] Those codes
 *         are not interchangeable, and NONE of them is zero faces: this board's
 *         camera worker used to fold every negative into 0, which reads as a
 *         measurement and would have hidden a decoder that was never initialised
 *         behind a perfectly healthy-looking "no faces" (issue #57).
 */
int nn_decoder_run(struct nn_model *m, struct bf_det *out, int max,
                   struct bf_result *res);

/**
 * Score threshold as a milli-probability, on the same scale as bf_det.score.
 * Outside 1..999 the request is refused (BF_ERR_ARG) and nothing changes.
 *
 * This board has no `ai thresh` command yet -- unifying the command surface is
 * issue #50 -- so the default is what it uses.  That default is the same number
 * the compile-time BF_SCORE_LOGIT of 0.405 was, in the other unit.
 */
int      nn_decoder_set_thresh_milli(unsigned milli);
unsigned nn_decoder_get_thresh_milli(void);

#ifdef __cplusplus
}
#endif

#endif /* NN_DECODER_H */
