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
 * describes its own result; when none is, the resident BlazeFace decoder does
 * what it has always done.  Both arms exist on this board -- unlike
 * grove-vision-ai-v2, where issue #104 removed the resident one -- and keeping
 * them is what makes the routing testable: set a threshold through this shim
 * and observe that the OTHER decoder did not move.
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
 */
#ifndef NN_ACTIVE_H
#define NN_ACTIVE_H

#include <stddef.h>
#include <stdint.h>

#include "blazeface.h"    /* BF_ERR_* -- the shared decode vocabulary */
#include "nn.h"           /* struct nn_model */
#include "nn_svc.h"       /* nn_svc_write_fn, NN_SVC_THRESH_NONE */
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

/** Is a plugin in force?  When it is, the result belongs to it. */
int nn_active_is_plugin(void);

/**
 * @brief  Can the active decoder read this model's outputs?
 *
 * Asked before something expensive is committed to -- lighting a camera and
 * finding out per frame that the wrong model is open is a stream that runs and
 * silently never annotates.
 *
 * With no plugin this answers for the RESIDENT decoder, which is the honest
 * answer to "will anything read these shapes": non-zero, because the resident
 * one discovers a mismatch per frame and says BF_ERR_MODEL rather than
 * refusing up front, and refusing here would change what `nn run` does to
 * every non-BlazeFace model this board has always accepted.
 */
int nn_active_shapes_ok(struct nn_model *m);

/**
 * @brief  Decode this model's current outputs with the PLUGIN.
 *
 * @return the plugin's own count, or a negative BF_ERR_*.  The RESULT ITSELF
 *         stays with the plugin -- ask it to draw or to report.
 *
 * Only ever called when @ref nn_active_is_plugin; the resident decoder has its
 * own entry point because it fills the caller's array and this one cannot.
 * With no plugin it answers BF_ERR_UNINIT, deliberately not BF_ERR_MODEL --
 * that means "not a detector" and routes to the shared class report.
 *
 * [!] THE CALLER MUST HOLD THE RESULT LEASE.  See plugin_lease.h: the panel
 * runs at a higher priority than the worker, so without it a draw can read
 * state this call is halfway through writing.
 */
int nn_active_decode(struct nn_model *m);

/** Let the active decoder paint its own result.  A no-op with no plugin. */
void nn_active_draw(const struct plugin_painter *paint);

/**
 * @brief  Will the active decoder put anything on the panel?
 *
 * A plugin need not draw: DRAW is an optional slot.  A caller about to light a
 * camera and a panel has to know, because "the stream runs and never
 * annotates" is indistinguishable from a broken one.  Non-zero with no plugin:
 * the resident overlay draws its boxes as it always has.
 */
int nn_active_can_draw(void);

/** Will it describe its result in words?  REPORT is an optional slot, and
 *  "it said nothing" and "it has nothing to say with" are different answers. */
int nn_active_can_report(void);

/** Ask it to.  Returns what its own report returned, or 0 with no plugin. */
int nn_active_report(nn_svc_write_fn write, void *ctx);

/**
 * The threshold, from whichever decoder will actually use it.
 *
 * [!] A PLUGIN OWNS ITS OWN.  Routed here, `nn thresh` reaches the number that
 * decides something; reaching past this shim would change a firmware variable
 * the loaded decoder never reads.  A plugin that declares no parameters has
 * none, and says so rather than borrowing the resident decoder's.
 */
unsigned nn_active_get_thresh_milli(void);

enum {
	NN_ACTIVE_THRESH_OK = 0,
	NN_ACTIVE_THRESH_REFUSED,      /**< the decoder rejected the value   */
	NN_ACTIVE_THRESH_NO_DECODER,   /**< nothing holds a threshold        */
};

int nn_active_set_thresh_milli(unsigned milli);

/**
 * @brief  The base vtable a plugin is handed.  Never NULL, never moves.
 *
 * `static const` in .rodata: the plugin keeps the pointer for its lifetime.
 */
const struct plugin_base_api *nn_active_base(void);

/** The base's transform -- model input coordinates to frame pixels.  Public
 *  because the resident overlay maps boxes the same way and a second copy of
 *  the arithmetic is a second answer. */
int nn_active_to_frame(void *ctx, float x, float y, float w, float h,
                       struct plugin_rect *out);

#ifdef __cplusplus
}
#endif

#endif /* NN_ACTIVE_H */
