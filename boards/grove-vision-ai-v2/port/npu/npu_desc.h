/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    npu_desc.h
 * @brief   npu_tensor -> tensor_desc, and nothing else (issues #97, #104).
 *
 * `struct npu_tensor` carries an opaque TfLiteType whose meaning is pinned by a
 * C++ static_assert in npu_tflm.cc; `struct tensor_desc` (svc/tensor.h) is the
 * neutral descriptor everything above the port reads.  This file is the
 * translation, and it is the only place on this board that knows both types.
 *
 * [!] IT WAS HALF OF nn_decoder.c UNTIL ISSUE #104, and the other half was a
 * BlazeFace decoder.  The two had nothing to do with each other: `nn out`,
 * `nn info` and the active-decoder shim all need this translation whatever
 * interprets the tensors -- or whether anything does.  When the resident decoder
 * left, the name `nn_decoder.c` would have described only the part that went.
 */
#ifndef NPU_DESC_H
#define NPU_DESC_H

#include "npu.h"
#include "tensor.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Output tensors this board's paths will look at.
 *
 * One definition, because several callers translate the same set and used to
 * spell this separately -- the miniature of the duplication issue #97 is about.
 * A model with more outputs than this is not one this firmware is reading in
 * full: the extras are ignored rather than misread.
 */
#define NPU_DESC_MAX_OUTPUTS 8

/**
 * @brief  Translate one native descriptor into the neutral one (issue #97).
 *
 * The rank passes through unchanged: npu_tflm.cc already refuses to truncate a
 * higher-rank tensor, so a rank of 0 arriving here means "not representable" and
 * every shape test downstream fails on it.
 */
void npu_desc_of(struct tensor_desc *d, const struct npu_tensor *t);

#ifdef __cplusplus
}
#endif

#endif /* NPU_DESC_H */
