/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    blazeface.h
 * @brief   BlazeFace-front 128 face-detection decode
 * (owhinata/stm32f746g-disco#81 owhinata/stm32f746g-disco#8, Epic
 * owhinata/stm32f746g-disco#80).
 *
 * Model-specific post-processing for ST Model Zoo's BlazeFace Front 128x128 (a
 * MediaPipe/PINTO-derived SSD face detector).  Lives ABOVE the backend-agnostic
 * nn layer (the generic nn API stays model-agnostic).  Turns the 4 raw float32
 * output tensors (2 anchor scales x {box, score}) into a short list of face
 * bounding boxes via SSD anchor decode + non-max suppression.
 *
 * Anchors (896 total, MediaPipe BlazeFace-front layout):
 *   - 16x16 grid (stride 8), 2 anchors/cell  -> 512  (the 1x512x* tensors)
 *   - 8x8  grid (stride 16), 6 anchors/cell  -> 384  (the 1x384x* tensors)
 * Each anchor is a fixed-size cell-centre point (w=h=1, normalized).
 */
#ifndef BLAZEFACE_H
#define BLAZEFACE_H

#include <stdint.h>

struct nn_model;   /* port/nn/nn.h */

/** One detection: normalized [0,1] box (top-left origin) + confidence. */
struct bf_det {
	float x, y, w, h;   /* normalized to the 128x128 input frame */
	float score;        /* sigmoid confidence 0..1                */
};

/** Max detections returned (post-NMS). */
#define BF_MAX_DET 8

/**
 * Decode BlazeFace outputs from model @p m into @p out[0..max).  Returns the
 * number of detections (>=0), or -1 if @p m's outputs are NOT BlazeFace-shaped
 * (a safe no-op for other models -- callers may invoke it unconditionally).
 */
int blazeface_decode(struct nn_model *m, struct bf_det *out, int max);

/**
 * Diagnostic: highest raw (pre-sigmoid) score seen in the last decode.
 *
 * Over ALL 896 anchors, always -- the scan is never cut short (issue #47).  A
 * truncating scan reports the maximum of a prefix, and then "no faces, peak 0.2"
 * means nothing.
 */
float blazeface_last_max_score(void);

/**
 * Diagnostic: how many anchors passed the threshold in the last decode, BEFORE
 * NMS and BEFORE the candidate cap.  This is the honest count.
 *
 * Separates "the model responded to nothing" from "NMS collapsed everything into
 * one box" -- two states the detection count alone cannot tell apart.
 */
int blazeface_last_npass(void);

/**
 * Diagnostic: how many of those were actually KEPT as candidates.  Less than
 * blazeface_last_npass() means the cap bound; the kept set is still the
 * highest-scoring ones, but NMS then only saw those.
 */
int blazeface_last_nkept(void);

/** Candidates retained before NMS.  Reported so the cap is never a surprise. */
#define BF_MAX_CAND 64

#endif /* BLAZEFACE_H */
