/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    nn_active.h
 * @brief   Which decoder is in force, and the one place that decides
 *          (issue #110 = #78 Step 3b).
 *
 * A container may carry a plugin.  When one is loaded it decodes, draws and
 * describes its own result; WHEN NONE IS, NOTHING DECODES AT ALL.
 *
 * [!] THAT SECOND ARM IS EMPTY SINCE ISSUE #116 (= #78 Step 3c), as it has been
 * on grove-vision-ai-v2 since #104: this firmware carries no decoder of its own,
 * so a model with no plugin beside it runs and its OUTPUT TENSORS are what get
 * reported.  What this shim decides is therefore no longer "which of two", but
 * "is there one" -- and every answer below has to agree about that.  They did
 * not have to before: a firmware decoder made each of them a sensible fallback,
 * and the ones that are now wrong (a threshold, a panel that annotates) would
 * have read as working.
 *
 * [!] ONE BRANCH POINT, NOT ONE PER CALLER.  The obvious change was to route
 * the worker's decode call and stop there.  Grove's code said otherwise: its
 * one-shot had its own decode path, stream admission asked its own shape
 * question, and `nn thresh` reached the decoder directly.  A plugin carries its
 * OWN threshold, so with the routing done in one place and not the others,
 * `nn thresh 700` changed a number the plugin never read -- a divergence an
 * operator meets in the first minute, and one a differential test giving both
 * decoders the same threshold would never see.
 *
 * [!] AND A PLUGIN'S RESULT IS PRIVATE.  This firmware does not know its shape,
 * which is the entire point of issue #78, so a plugin draws and reports it
 * itself.  @ref nn_active_decode returns a count and nothing else.
 *
 * [!] THE GEOMETRY IS A CONSTANT HERE, AND THAT IS NOT LUCK.  Grove builds a
 * different crop for `nn run` than for `nn stream` and has to publish whichever
 * is current -- wiring only one of them left the other's boxes reporting
 * "outside the frame" on every call.  This board has ONE preprocessing path:
 * the band downsample squashes the whole 320x240 frame onto the model's square
 * input, and the landscape drawing surface is that same 320x240.  So the
 * transform is a multiply by the surface size, there is nothing to publish, and
 * no caller can forget to.
 *
 * [!] THE ENTRIES THEMSELVES ARE svc/nn/nn_active_lease.h's SINCE ISSUE #159.
 * nn_active_is_plugin(), nn_active_shapes_ok(), nn_active_decode(), the draw,
 * report and threshold calls and NN_ACTIVE_NOT_HELD were this board's until
 * f746g-disco needed the very same ones; they are declared and documented
 * there now, and this file includes that header, so every caller still reads
 * the same names from here.  What stays is what only this board knows: the
 * facts it hands the shared entries (port/nn/nn_active.c), the frame geometry
 * and the base vtable a plugin is handed.
 */
#ifndef NN_ACTIVE_H
#define NN_ACTIVE_H

#include <stddef.h>
#include <stdint.h>

#include "nn_active_lease.h" /* the entries, NN_ACTIVE_NOT_HELD (issue #159) */
#include "nn.h"           /* struct nn_model */
#include "plugin_abi.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * The frame a box is mapped into: the landscape drawing surface.
 *
 * [!] DECLARED HERE AND CHECKED AGAINST THE PANEL SOMEWHERE ELSE.  port/nn may
 * not include port/ltdc -- port modules do not reach sideways into each other,
 * which is why the preview lives in src/ -- so this is a second statement of
 * the same fact rather than a lookup.  src/cam_preview.c sees both and asserts
 * they agree; if the panel is ever re-sized, that assert is what says so.
 */
#define NN_ACTIVE_FRAME_W 320
#define NN_ACTIVE_FRAME_H 240

/**
 * @brief  The base vtable a plugin is handed.  Never NULL, never moves.
 *
 * `static const` in .rodata: the plugin keeps the pointer for its lifetime.
 */
const struct plugin_base_api *nn_active_base(void);

/** The base's transform -- model input coordinates to frame pixels.  Declared
 *  here so that what a plugin is handed can be exercised directly: it is the
 *  one piece of the base vtable with arithmetic in it. */
int nn_active_to_frame(void *ctx, float x, float y, float w, float h,
                       struct plugin_rect *out);

#ifdef __cplusplus
}
#endif

#endif /* NN_ACTIVE_H */
