/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    nn_decoder.h
 * @brief   This board's adapter onto the shared BlazeFace decoder (issue #97).
 *
 * The decoder itself is svc/blazeface.c, shared with grove-vision-ai-v2 and
 * f746g-disco.  It reads svc/tensor.h descriptors and owns no storage at all,
 * so somebody has to own the decoder's state and its candidate scratch -- which
 * is precisely what lets the scratch stay in `.psram_ai` here and somewhere else
 * on the other two boards.  This file is that.
 *
 * [!] THE TRANSLATION THAT USED TO BE HERE IS nn_desc.h (issue #116).  It never
 * belonged to a decoder: `nn out`, `nn info` and the active-decoder shim need
 * `nn_tensor` -> `tensor_desc` whatever interprets the tensors, or whether
 * anything does.  f746g-disco's copy of this file is still both halves, because
 * it still carries a resident decoder (issue #78 Step 4).
 *
 *
 * [!] THIS FILE IS A FEW LINES DIFFERENT FROM THE OTHER nn.h BOARD'S COPY of it,
 * and that is deliberate rather than overlooked.  The decoder halves differ only
 * in where the scratch is placed, because `struct nn_tensor` and `enum nn_dtype`
 * happen to be identical on both -- but `port/nn/nn.h` as a whole is not (184 of
 * its 382 lines differ), and neither is nn.c.  Folding these two adapters
 * therefore means folding that API first, which is a larger and separate question
 * than issue #97 set out to answer.  Recorded here so the next person weighs it
 * rather than rediscovering it.
 *
 * IT TAKES THE MODEL HANDLE, not a tensor array, because that is what both
 * callers already have: the decoder is singleton-independent but this board's
 * inference API is not, and pulling the four outputs is the adapter's job.
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
 *             threshold THIS decode applied -- report that rather than asking for
 *             the current one, which another console may have changed since.
 *
 * @return the number of detections (>= 0), or a BF_ERR_* code.  [!] Those codes
 *         are not interchangeable: BF_ERR_MODEL means the open model is not
 *         BlazeFace, the others mean this firmware is wired wrong.  Do not fold
 *         them together, and do not fold any of them into zero detections --
 *         "zero faces" reads as a measurement (issue #57).
 */
int nn_decoder_run(struct nn_model *m, struct bf_det *out, int max,
                   struct bf_result *res);

/**
 * Score threshold as a milli-probability, on the same scale as bf_det.score.
 * Outside 1..999 the request is refused (BF_ERR_ARG) and nothing changes: those
 * are the poles of the inverse sigmoid, and a silently clamped threshold is a
 * setting that does not do what it says.
 *
 * SAFE TO CALL WHILE A STREAM RUNS.  The decoder reads the threshold once per
 * frame, so a set here lands wholly before or wholly after a decode and the value
 * used is reported back in @ref bf_result -- which is why `nn thresh` takes no
 * guard and tuning against a live stream still works.
 */
int      nn_decoder_set_thresh_milli(unsigned milli);
unsigned nn_decoder_get_thresh_milli(void);

#ifdef __cplusplus
}
#endif

#endif /* NN_DECODER_H */
