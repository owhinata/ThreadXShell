/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    nn_outputs.h
 * @brief   What the inference worker does with a model's outputs (issue #129).
 *
 * Three callers used to read the outputs three ways, and when `nn run` moved
 * onto the worker they had to stay three ways (review of a438f76):
 *
 *   - a STREAM refuses a model with more outputs than this board decodes
 *     (NPU_DESC_MAX_OUTPUTS), and refuses the frame if any is unreadable;
 *   - `nn run` with a PLUGIN decodes the first NPU_DESC_MAX_OUTPUTS and names
 *     the output it could not read;
 *   - `nn run` with NO plugin reports the outputs as tensors: every output the
 *     model has is COUNTED, the first NN_RAW_OUTPUTS_MAX that can be read are
 *     described, and a read that fails ends the list there rather than the
 *     report (svc/nn_det_record.h: `count` is the model's, `n` what was kept).
 *
 * The decision is taken under the plugin lease, after the plugin question, so
 * the bare model's count is never refused for a limit only a decoder has.
 *
 * Pure, with the reads behind a callback, so test/test_nn_outputs.c can drive a
 * nine-output model and a read that fails half way -- neither of which the
 * models this board ships can produce.
 */
#ifndef NN_OUTPUTS_H
#define NN_OUTPUTS_H

#include <stdint.h>

#include "nn_det_record.h"   /* struct nn_raw_outputs */

#ifdef __cplusplus
extern "C" {
#endif

enum nn_out_plan {
	NN_OUT_RAW = 0,   /**< no plugin: describe the outputs, decode nothing */
	NN_OUT_DECODE,    /**< hand the first *n_use outputs to the plugin     */
	NN_OUT_REFUSE,    /**< a stream over a model with too many outputs     */
};

/**
 * @param oneshot     `nn run` (non-zero) or a stream
 * @param has_plugin  whether a plugin is loaded, asked under the lease
 * @param n_out       the model's output count
 * @param max         how many this board decodes (NPU_DESC_MAX_OUTPUTS)
 * @param n_use       out, for NN_OUT_DECODE: how many to collect
 */
enum nn_out_plan nn_out_plan(int oneshot, int has_plugin, unsigned n_out,
                             unsigned max, unsigned *n_use);

/** Read output @p i into whatever the caller collects; 0 on success. */
typedef int (*nn_out_read_fn)(void *ctx, unsigned i);

/** Read outputs 0 .. n-1 in order.  @return n when every read succeeded,
 *  else the index of the first that failed (the later ones are not read). */
unsigned nn_out_collect(unsigned n, nn_out_read_fn read, void *ctx);

/** Describe output @p i into @p out; 0 on success. */
typedef int (*nn_out_desc_fn)(void *ctx, unsigned i, struct tensor_desc *out);

/**
 * Fill @p raw for a model with @p count outputs: `count` is @p count, and the
 * first NN_RAW_OUTPUTS_MAX are described until one cannot be (`n` says how
 * many were).  @p raw is cleared first.
 */
void nn_out_raw_fill(struct nn_raw_outputs *raw, unsigned count,
                     nn_out_desc_fn desc, void *ctx);

#ifdef __cplusplus
}
#endif

#endif /* NN_OUTPUTS_H */
