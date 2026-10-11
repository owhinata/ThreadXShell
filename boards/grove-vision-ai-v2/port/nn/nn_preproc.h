/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    nn_preproc.h
 * @brief   Camera frame -> model input tensor, and detection box -> frame
 *          pixels (issue #48).
 *
 * WHY THIS IS ITS OWN FILE.  Every bug it can have -- a crop origin off by one,
 * a half-pixel convention applied to the wrong quantity, a fraction that
 * truncates the wrong way -- shows up on the board as a PLAUSIBLE box in the
 * wrong place, on an image nobody can check by eye.  Each hypothesis otherwise
 * costs a flash cycle on a part whose NOR is rated ~100k of them.  So it
 * depends on nothing: no hardware, no ThreadX, no camera singleton, no NPU
 * singleton.  test/test_nn_preproc.c compiles THIS file and drives it with
 * synthetic frames, exactly as test_blazeface.c does for the decoder.
 *
 * WHAT REPLACED WHAT.  Until #48 the input was a centre CROP of the model's own
 * size -- a 128x128 window out of a 320x240 frame, a field of view so narrow
 * the detector was nearly useless at any normal working distance.  Now the
 * largest centred rectangle with the INPUT's aspect ratio is SCALED into the
 * tensor: 240x240 for a square model on this frame.
 *
 * [!] THE TWO MAPPINGS ARE NOT THE SAME, AND THAT IS NOT A BUG.  Sampling maps
 * a destination sample CENTRE to a source sample INDEX and therefore carries
 * the half-pixel term; a box EDGE is already a continuous coordinate and must
 * not.  Applying the sample convention to an edge biases every box by half a
 * source pixel -- which looks like a slightly mis-drawn box, not like an error.
 * nn_preproc_box() and nn_preproc_fill() are two expressions of ONE transform.
 *
 * No vectorisation even by accident: board.cmake carries -fno-tree-vectorize on
 * this translation unit, because the ThreadX M55 port does not preserve VPR and
 * check_mve_predication.py is fail-closed.
 */
#ifndef NN_PREPROC_H
#define NN_PREPROC_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * The source rectangle a model input is taken from: the largest centred
 * rectangle of the frame with the input's aspect ratio.
 */
struct nn_preproc_geom {
	uint32_t x, y;   /**< crop origin in frame pixels        */
	uint32_t w, h;   /**< crop extent in frame pixels        */
	uint32_t dst_w;  /**< model input width                  */
	uint32_t dst_h;  /**< model input height                 */
};

/**
 * @brief  Work out the crop for a frame and a model input.
 *
 * Upscaling is allowed: the crop follows the ASPECT RATIO, not the direction of
 * the scale, so an input larger than the frame is a perfectly ordinary case.
 * (The code this replaces refused it, for no reason anyone recorded.)
 *
 * @return 0, or -1 for a degenerate shape -- a zero dimension, or one large
 *         enough that the fixed-point mapping could not be computed. Refusing
 *         is the point: the alternative is computing with it.
 */
int nn_preproc_geom(uint32_t frame_w, uint32_t frame_h,
                    uint32_t dst_w, uint32_t dst_h,
                    struct nn_preproc_geom *out);

/**
 * @brief  Scale the crop into the model's input tensor.
 *
 * Source is the camera's WDMA3 output: three PLANAR 8-bit channels in B, G, R
 * order, each @p frame_w * @p frame_h bytes. Destination is interleaved R, G, B
 * int8, which is what the model wants, written as (pixel - 128) -- a fixed
 * shift, correct for scale 1/255 with zero point -128 and progressively wrong
 * for anything else.
 *
 * [!] AND NOTHING CHECKS THAT ANY MORE (issue #104). The check belonged to the
 * resident BlazeFace decoder, whose arithmetic assumed it; with that decoder
 * gone the question has no owner, because a plugin ships WITH its model and
 * shipping it is the statement that the two agree. This shift is therefore a
 * board CONVENTION a container is written against, not a promise about the
 * model that is loaded -- see the board README.
 *
 * Bilinear with half-pixel sample centres, all fixed point, no libm. The
 * neighbour indices are clamped to the crop, which is what makes the first and
 * last output pixel well defined rather than a read one pixel outside it.
 *
 * @param bgr    planar B, G, R planes of a @p frame_w x @p frame_h frame
 * @param g      geometry from nn_preproc_geom()
 * @param dst    dst_w * dst_h * 3 bytes
 * @return 0, or -1 on a null argument
 */
int nn_preproc_fill(const uint8_t *bgr, uint32_t frame_w, uint32_t frame_h,
                    const struct nn_preproc_geom *g, uint8_t *dst);

/** A box in frame pixels, left/top inclusive, right/bottom EXCLUSIVE. */
struct nn_preproc_box {
	int32_t x0, y0, x1, y1;
};

/**
 * @brief  Map a detection box, normalised to the model input, to frame pixels.
 *
 * The inverse of the sampling transform, expressed for EDGES -- so no
 * half-pixel term (see the file header). Rasterised as floor(left) and
 * ceil(right), then clipped to the crop, so a box can never name a pixel the
 * model did not see.
 *
 * [!] The decoder neither clamps nor bounds its floats. A non-finite
 * coordinate is REJECTED and an absurd one is clamped before any conversion to
 * integer, because converting an out-of-range float to an int is undefined
 * behaviour and this is the only place that can catch it.
 *
 * @param nx,ny,nw,nh  normalised top-left origin and extent
 * @return 0 with @p out filled, or -1 if the box is not finite or clips away
 *         entirely -- in both cases nothing should be drawn.
 */
int nn_preproc_box(const struct nn_preproc_geom *g,
                   float nx, float ny, float nw, float nh,
                   struct nn_preproc_box *out);

#ifdef __cplusplus
}
#endif

#endif /* NN_PREPROC_H */
