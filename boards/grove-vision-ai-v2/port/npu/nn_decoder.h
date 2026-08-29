/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    nn_decoder.h
 * @brief   This board's adapter onto the shared BlazeFace decoder (issue #97).
 *
 * The decoder itself is svc/blazeface.c, shared with wio-lite-ai and
 * f746g-disco.  It reads svc/tensor.h descriptors, which is a type svc/ may
 * define and port/npu/ may not: `struct npu_tensor` carries an opaque TfLiteType
 * whose meaning is pinned by a C++ static_assert in npu_tflm.cc, and that assert
 * belongs where the knowledge is.  This file is the translation, and it is the
 * only place on this board that knows both types.
 *
 * IT ALSO OWNS THE DECODER'S STATE AND SCRATCH.  The shared translation unit owns
 * no storage at all (cmake/check_no_mutable_storage.py enforces it), so somebody
 * has to; on this board they are plain .bss in DTCM, which is what they already
 * were.  Both callers -- `nn detect` on the shell thread and the live overlay on
 * the camera producer thread -- go through this one instance, which is correct
 * because `nn_busy` in cmd_nn.c already holds across every nn subcommand
 * including the whole of `nn preview`, so two decodes cannot overlap.
 */
#ifndef NN_DECODER_H
#define NN_DECODER_H

#include "blazeface.h"
#include "npu.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Output tensors this board's decode path will look at.
 *
 * One definition, because both callers translate the same set and used to spell
 * this 8 separately -- the miniature of the duplication issue #97 is about.  A
 * model with more outputs than this is not one this decoder can be looking at:
 * BlazeFace has four, and the extras would be ignored rather than misread.
 */
#define NN_DECODER_MAX_OUTPUTS 8

/**
 * @brief  Would these tensors decode as BlazeFace, without decoding them?
 *
 * For a caller that must decide BEFORE it commits to something expensive:
 * `nn preview` starts a camera stream, and discovering the wrong model is open
 * on every frame would be a preview that runs and silently never annotates.
 *
 * @return non-zero if a decode would find all four tensors.
 */
int nn_decoder_shapes_ok(const struct npu_tensor *outs, unsigned n);

/**
 * @brief  Translate one native descriptor into the neutral one (issue #97).
 *
 * Public because the nn_svc adapter (issue #50) needs the same translation to
 * publish tensors to the shared command, and a second copy of it is exactly the
 * duplication issue #97 removed.  The rank passes through unchanged: npu_tflm.cc
 * already refuses to truncate a higher-rank tensor, so a rank of 0 arriving here
 * means "not representable" and every shape test downstream fails on it.
 */
void nn_decoder_desc(struct tensor_desc *d, const struct npu_tensor *t);

/**
 * Decode @p n NPU output tensors into @p out[0..max).
 *
 * @param res  filled on every path, failures included, so a caller never reports
 *             diagnostics left over from an earlier frame.  It also carries the
 *             threshold THIS decode applied -- print that rather than asking for
 *             the current one, which another console may have changed since.
 *
 * @return the number of detections (>= 0), or a BF_ERR_* code.  [!] Those codes
 *         are not interchangeable: BF_ERR_MODEL means the open model is not
 *         BlazeFace, the others mean this firmware is wired wrong.  Do not fold
 *         them together, and do not fold any of them into zero detections.
 */
int nn_decoder_run(const struct npu_tensor *outs, unsigned n,
                   struct bf_det *out, int max, struct bf_result *res);

/**
 * Score threshold as a milli-probability, on the same scale as bf_det.score.
 * Outside 1..999 the request is refused (BF_ERR_ARG) and nothing changes: those
 * are the poles of the inverse sigmoid, and a silently clamped threshold is a
 * setting that does not do what it says.
 */
int      nn_decoder_set_thresh_milli(unsigned milli);
unsigned nn_decoder_get_thresh_milli(void);

#ifdef __cplusplus
}
#endif

#endif /* NN_DECODER_H */
