/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    nn_desc.h
 * @brief   nn_tensor -> tensor_desc, and nothing else (issues #97, #116, #159;
 * one file since issue #161).
 *
 * `struct nn_tensor` is the nn inference API's tensor (svc/nn/nn.h); `struct
 * tensor_desc` (svc/nn/tensor.h) is the neutral descriptor everything above the
 * port reads.  This file is the translation, and it is the only place that
 * knows both types -- on grove-vision-ai-v2 too since issue #169, when its
 * own tensor type and translation (port/nn/npu/npu_desc.c) went.
 *
 * [!] IT IS THE HALF OF nn_decoder.c THAT OUTLIVED THE RESIDENT DECODER.  `nn
 * out`, `nn info`, the active-decoder shim and the raw publish all need this
 * translation whatever interprets the tensors -- or whether anything does.
 *
 * IT IS BUILT IN EVERY BACKEND: none of them changes what a descriptor is, and
 * `nn out` / `nn info` exist in all of them.
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
 * [!] AND A dtype THE API CANNOT NAME BECOMES UNSUPPORTED rather than the
 * nearest one that is readable: a reader then refuses the tensor instead of
 * reading its bytes as a type they are not.
 */
void nn_desc_of(struct tensor_desc *d, const struct nn_tensor *t);

#ifdef __cplusplus
}
#endif

#endif /* NN_DESC_H */
