/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    nn_active.h
 * @brief   Which decoder is in force, and the one place that decides
 *          (issues #103, #104).
 *
 * A container may carry a plugin, and on this board a plugin is the ONLY decoder
 * there is: issue #104 removed the resident BlazeFace decoder, so what this
 * chooses between is a loaded plugin and nothing at all.
 *
 * [!] ONE BRANCH POINT, NOT ONE PER CALLER.  The obvious change in issue #103
 * was to route the stream's decode call and stop there.  The code said
 * otherwise: `nn run` had its own decode path, stream admission asked its own
 * shape question, and `nn thresh` reached the decoder directly.  A plugin
 * carries its OWN threshold, so with the routing done in one place and not the
 * others, `nn thresh 700` changed a number the plugin never read -- a divergence
 * an operator meets in the first minute, and one a differential test giving both
 * decoders the same threshold would never see.
 *
 * [!] AND A PLUGIN'S RESULT IS PRIVATE.  The firmware does not know its shape,
 * which is the entire point of issue #78, so a plugin draws and reports it
 * itself.  @ref nn_active_decode therefore returns a count and nothing else --
 * it took a `struct bf_det` array until issue #104, and "a plugin must not write
 * the caller's boxes" was a rule in a comment.  With nothing in this firmware
 * left to fill such an array, it is a property of the signature instead.
 */
#ifndef NN_ACTIVE_H
#define NN_ACTIVE_H

#include <stddef.h>
#include <stdint.h>

#include "blazeface.h"  /* BF_ERR_* -- the shared decode vocabulary */
#include "npu.h"          /* struct npu_tensor */
#include "nn_svc.h"       /* nn_svc_write_fn, NN_SVC_THRESH_NONE */
#include "plugin_abi.h"

#ifdef __cplusplus
extern "C" {
#endif

struct nn_preproc_geom;

/**
 * @brief  Publish the transform the current decode was built with.
 *
 * [!] ONE GEOMETRY, FOR THE SAME REASON THERE IS ONE DECODER.  `nn run` and
 * `nn stream` each build their own input and each kept their own
 * nn_preproc_geom, and the plugin's coordinate transform was first wired to the
 * stream's -- so every box `nn run` produced came back "outside the frame",
 * because the geometry it consulted had never been set on that path.  Both
 * paths publish here now, and the nn gate is what makes "the last one set" the
 * right one: the two never run at once.
 */
void nn_active_set_geom(const struct nn_preproc_geom *g);

/** Forget it: there is no current decode to map boxes for. */
void nn_active_clear_geom(void);

/**
 * @brief  The base vtable's transform -- model input coordinates to frame
 *         pixels.
 *
 * @return 0 with @p out filled, or non-zero when there is nothing to draw (no
 *         current geometry, or a box a degenerate model made non-finite).
 */
int nn_active_to_frame(void *ctx, float x, float y, float w, float h,
                       struct plugin_rect *out);

/** Is a plugin in force?  When it is, the boxes belong to it. */
int nn_active_is_plugin(void);

/**
 * @brief  Can the active decoder read these tensors?
 *
 * Asked before something expensive is committed to -- Grove's `nn stream`
 * starts a camera, and finding out on every frame that the wrong model is open
 * would be a preview that runs and silently never annotates.
 *
 * Always false with no plugin: there is then nothing that reads any shape.
 */
int nn_active_shapes_ok(const struct npu_tensor *outs, unsigned n);

/**
 * @brief  Decode one set of outputs.
 *
 * @return the plugin's own count, or a negative BF_ERR_*.  The RESULT ITSELF
 *         stays with the plugin -- ask it to draw or to report.
 *
 * With no plugin loaded this returns BF_ERR_UNINIT, which is a backstop rather
 * than a path: the service adapter decides plugin-or-raw in the one helper both
 * its console callers reach, and the stream refuses admission before it lights
 * a camera.  Deliberately not BF_ERR_MODEL -- that means "not a detector" and
 * routes to the shared class report.
 */
int nn_active_decode(const struct npu_tensor *outs, unsigned n);

/**
 * @brief  Let the active decoder paint its own result.
 *
 * Called on the panel thread with the panel guard held.  A no-op with no plugin
 * loaded, which a stream can no longer be running under.
 */
void nn_active_draw(const struct plugin_painter *paint);

/**
 * @brief  Will the active decoder put anything on the panel?
 *
 * A plugin need not draw: DRAW is an optional slot, and a classifier has
 * nothing to draw until there is a font to draw it with.  A caller that is
 * about to light a camera and a panel for a live preview has to know, because
 * "the stream runs and never annotates" is indistinguishable from a broken one.
 * False with no plugin, for the stronger reason that nothing decodes at all.
 */
int nn_active_can_draw(void);

/**
 * @brief  Let the active decoder describe its own result.
 *
 * @return 0, or negative when the writer refused
 *
 * A no-op returning 0 with no plugin loaded, for the same reason as
 * @ref nn_active_draw.
 */
int nn_active_report(nn_svc_write_fn write, void *ctx);

/**
 * The score threshold, in milli, of whichever decoder is in force -- or
 * NN_SVC_THRESH_NONE when nothing holds one (no plugin, or a plugin that
 * declares no parameters, such as the classifier).
 */
unsigned nn_active_get_thresh_milli(void);

/**
 * @brief  Set it.
 *
 * [!] THREE ANSWERS, NOT TWO (issue #104).  "The value was refused" and "there
 * is nothing here to hold one" send an operator to different places -- a number
 * to change, or a container to load -- so they are not folded together the way a
 * plain success/failure return would have folded them.
 */
#define NN_ACTIVE_THRESH_OK           0
#define NN_ACTIVE_THRESH_REFUSED    (-1)
#define NN_ACTIVE_THRESH_NO_DECODER (-2)

int nn_active_set_thresh_milli(unsigned milli);

#ifdef __cplusplus
}
#endif

#endif /* NN_ACTIVE_H */
