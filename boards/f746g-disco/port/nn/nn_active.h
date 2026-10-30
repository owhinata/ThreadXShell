/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    nn_active.h
 * @brief   This board's facts for the shared decoder branch (issue #159 = #78
 *          Step 4b).
 *
 * The entries -- nn_active_is_plugin(), nn_active_shapes_ok(), nn_active_decode(),
 * the draw, report and threshold calls, and NN_ACTIVE_NOT_HELD -- are
 * svc/nn/nn_active_lease.h's, shared with wio-lite-ai, and this header includes
 * it so every caller reads them from here.  svc/nn/nn_active_core.c below them
 * makes the decision and the indirect calls.  What is this board's is in
 * port/nn/nn_active.c: reading an nn_tensor as a descriptor, where a depth
 * sample goes, the frame a box is mapped into, and the base vtable.
 *
 * [!] IN STEP 4b THE RESIDENT DECODER IS STILL HERE, AS THE NO-PLUGIN FALLBACK.
 * With a plugin in force everything goes through these entries; with none the
 * worker publishes through port/nn/nn_decoder.c as before.  Step 4c removes the
 * fallback (plan D7).
 *
 * [!] THE FRAME IS THE PREVIEW'S, AND IT IS NOT A CONSTANT HERE.  The producer
 * squashes the whole camera frame onto the model's input (nearest neighbour,
 * port/nn/nn_producer.c), so the model's normalised space and the frame's are
 * the same space, and a box is mapped by multiplying by the frame's size.  That
 * size is whatever the base capture delivers -- QVGA or QQVGA, the same frame
 * the GUIX preview stores into cam_view_buf -- so the transform reads the
 * geometry the stream attached with (nn_camera.c) instead of a literal.
 */
#ifndef NN_ACTIVE_H
#define NN_ACTIVE_H

#include <stddef.h>
#include <stdint.h>

#include "nn_active_lease.h" /* the entries, NN_ACTIVE_NOT_HELD */
#include "nn.h"              /* struct nn_model */
#include "plugin_abi.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief  The base vtable a plugin is handed.  Never NULL, never moves.
 *
 * `static const` in .rodata: the plugin keeps the pointer for its lifetime.
 */
const struct plugin_base_api *nn_active_base(void);

/** The base's transform -- model input coordinates to frame pixels, in the
 *  frame the stream attached with.  Declared here so that what a plugin is
 *  handed can be exercised directly. */
int nn_active_to_frame(void *ctx, float x, float y, float w, float h,
                       struct plugin_rect *out);

#ifdef __cplusplus
}
#endif

#endif /* NN_ACTIVE_H */
