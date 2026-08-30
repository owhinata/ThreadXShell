/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    nn_active.h
 * @brief   Which decoder is in force, and the one place that decides
 *          (issue #103).
 *
 * A container may carry a plugin; when it does, the plugin interprets the
 * model's output instead of the decoder built into the firmware.  Everything
 * that used to reach nn_decoder_* directly comes through here instead.
 *
 * [!] ONE BRANCH POINT, NOT ONE PER CALLER.  The obvious change was to route
 * the stream's decode call and stop there.  The code says otherwise: `nn run`
 * has its own decode path, stream admission asks its own shape question, and
 * `nn thresh` reaches the resident decoder directly.  A plugin carries its OWN
 * threshold, so with the routing done in one place and not the others,
 * `nn thresh 700` would have changed the firmware's number while the plugin
 * went on using its old one -- a divergence an operator meets in the first
 * minute, and one that a differential test giving both decoders the same
 * threshold would never see.  Routing everything through a single decision is
 * what makes that impossible rather than merely unlikely.
 *
 * [!] AND THE TWO DECODERS DO NOT PRODUCE THE SAME THING.  The resident decoder
 * fills in `struct bf_det` boxes and the base draws and prints them.  A plugin's
 * decode result is PRIVATE -- the firmware does not know its shape, which is the
 * entire point of issue #78 -- so a plugin draws and reports it itself.  That is
 * why @ref nn_active_decode does not promise to fill anything in, and why
 * @ref nn_active_is_plugin exists: a caller has to know whether the boxes are
 * its business or the plugin's.
 */
#ifndef NN_ACTIVE_H
#define NN_ACTIVE_H

#include <stddef.h>
#include <stdint.h>

#include "blazeface.h"
#include "npu.h"          /* struct npu_tensor */
#include "nn_svc.h"       /* nn_svc_write_fn  */
#include "plugin_abi.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Is a plugin in force?  When it is, the boxes belong to it. */
int nn_active_is_plugin(void);

/**
 * @brief  Can the active decoder read these tensors?
 *
 * Asked before something expensive is committed to -- Grove's `nn stream`
 * starts a camera, and finding out on every frame that the wrong model is open
 * would be a preview that runs and silently never annotates.
 */
int nn_active_shapes_ok(const struct npu_tensor *outs, unsigned n);

/**
 * @brief  Decode one set of outputs.
 *
 * @param out, max, res  filled ONLY when nn_active_is_plugin() is false
 * @return the detection count, or a negative BF_ERR_*
 *
 * [!] A PLUGIN LEAVES @p out AND @p res ALONE.  Its result has a shape this
 * firmware does not know; ask it to draw or to report instead.  A caller that
 * read the boxes anyway would be reading whatever the last resident decode left
 * there, which is worse than reading nothing.
 */
int nn_active_decode(const struct npu_tensor *outs, unsigned n,
                     struct bf_det *out, int max, struct bf_result *res);

/**
 * @brief  Let the active decoder paint its own result.
 *
 * Called on the panel thread with the panel guard held.  A no-op when the
 * resident decoder is in force -- the base draws those boxes itself, because it
 * knows what they are.
 */
void nn_active_draw(const struct plugin_painter *paint);

/**
 * @brief  Let the active decoder describe its own result.
 *
 * @return 0, or negative when the writer refused
 *
 * A no-op returning 0 when the resident decoder is in force, for the same
 * reason as @ref nn_active_draw.
 */
int nn_active_report(nn_svc_write_fn write, void *ctx);

/** The score threshold, in milli, of whichever decoder is in force. */
unsigned nn_active_get_thresh_milli(void);

/** @return non-zero on success.  Out-of-range values are refused, not clamped. */
int nn_active_set_thresh_milli(unsigned milli);

#ifdef __cplusplus
}
#endif

#endif /* NN_ACTIVE_H */
