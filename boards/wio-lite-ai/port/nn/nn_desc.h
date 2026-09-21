/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    nn_desc.h
 * @brief   nn_tensor -> tensor_desc, and nothing else (issues #97, #116).
 *
 * `struct nn_tensor` is this board's inference API (port/nn/nn.h); `struct
 * tensor_desc` (svc/tensor.h) is the neutral descriptor everything above the
 * port reads.  This file is the translation, and it is the only place on this
 * board that knows both types.
 *
 * [!] IT IS HALF OF WHAT nn_decoder.c USED TO BE, and the other half is a
 * BlazeFace decoder.  The two have nothing to do with each other: `nn out`,
 * `nn info` and the active-decoder shim all need this translation whatever
 * interprets the tensors -- or whether anything does.  Issue #116 takes the
 * resident decoder away, and the name `nn_decoder.c` would then have described
 * only the part that went.  (grove-vision-ai-v2 split the same file the same
 * way for issue #104; its half is port/npu/npu_desc.c.)
 *
 * IT IS BUILT IN BOTH BACKENDS.  Neither `null` nor `tflm` changes what a
 * descriptor is, and `nn out` / `nn info` exist in both.
 */
#ifndef NN_DESC_H
#define NN_DESC_H

#include "nn.h"
#include "tensor.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief  Translate one native descriptor into the neutral one (issue #97).
 *
 * [!] A RANK ABOVE FOUR IS REFUSED, NOT TRUNCATED.  A shortened shape can still
 * match a lookup, and then a reader takes a tensor it was not looking for while
 * every check it makes passes.  Rank 0 is left in place to say "not
 * representable", and every shape test downstream fails on it.
 *
 * [!] AND A dtype THIS BOARD CANNOT NAME BECOMES UNSUPPORTED rather than the
 * nearest one that is readable: a reader then refuses the tensor instead of
 * reading its bytes as a type they are not.
 */
void nn_desc_of(struct tensor_desc *d, const struct nn_tensor *t);

#ifdef __cplusplus
}
#endif

#endif /* NN_DESC_H */
