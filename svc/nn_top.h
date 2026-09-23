/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    nn_top.h
 * @brief   The top classes of an output tensor, taken by the thread that ran
 *          the inference (issue #121, decision D3).
 *
 * [!] THIS USED TO BE THE SHARED COMMAND'S, AND IT READ THE WRONG FRAME.  `nn
 * run` and `nn dets` computed the classes at print time from whatever the
 * output tensor held then -- after a stream stop, the frame whose publish had
 * been dropped; after another console's `nn bench`, its pattern.  The worker
 * that ran the inference is the one thread that knows the tensor is still this
 * inference's, so it computes the classes before its next inference and
 * publishes them WITH the result.  The walk runs outside the record's lock;
 * only the copy into the record is under it.
 *
 * Pure: no storage, no board, so the host test runs the same code.
 */
#ifndef NN_TOP_H
#define NN_TOP_H

#include "nn_det_record.h"   /* struct nn_top5 */
#include "tensor.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Fill @p out from @p t, whose @ref tensor_desc::data must be readable for the
 * whole call.  int8, uint8 and float32 are read; anything else has no stride
 * this reads at and says so (NN_TOP_NO_STRIDE) rather than being walked at a
 * guessed one.  A null or bufferless tensor is NN_TOP_NO_OUTPUT.
 */
void nn_top_of(const struct tensor_desc *t, struct nn_top5 *out);

#ifdef __cplusplus
}
#endif

#endif /* NN_TOP_H */
