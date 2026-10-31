/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    nn_ethosu_io.h
 * @brief   The I/O rule a loaded model must meet, shared by the Ethos-U backend
 *          and the host gate (issue #169).
 *
 * ONE STATEMENT, TWO CALLERS, for the reason npu_verify.h has one: the host gate
 * (boards/grove-vision-ai-v2/scripts/verify_vela_model.cc) exists to reach the
 * board's verdict before a transfer, and a gate that accepts what the device then
 * refuses has only moved the failure to the serial line, after the erase.  The
 * backend (nn_ethosu.cc) refuses a model that fails these with NN_MODEL_ERR_SHAPE
 * (the counts) or NN_MODEL_ERR_TENSOR (a tensor), the tflm backend's rule, so
 * `nn` means the same thing by a loaded model on every board.
 *
 * C++ (TfLiteTensor).  static inline and no state.
 */
#ifndef NN_ETHOSU_IO_H
#define NN_ETHOSU_IO_H

#include <stddef.h>

#include "tensorflow/lite/c/common.h"

#include "nn.h"   /* NN_MAX_IO, NN_MAX_DIMS */

/* 1..NN_MAX_IO inputs, or outputs. */
static inline bool nn_ethosu_io_count_ok(size_t n)
{
	return n >= 1u && n <= (size_t)NN_MAX_IO;
}

/* A tensor this API can describe: present, with a buffer, rank <= NN_MAX_DIMS. */
static inline bool nn_ethosu_tensor_ok(const TfLiteTensor *t)
{
	return t && t->data.data && t->dims && t->dims->size <= NN_MAX_DIMS;
}

#endif /* NN_ETHOSU_IO_H */
