/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    cam_auto.h
 * @brief   Auto white balance (issue #35).
 *
 * WHY THIS EXISTS.  The HX6538 datapath is a demosaic and nothing else -- there
 * is no auto white balance anywhere in it, because the applications it was
 * built for feed the output to a neural network, which does not care what the
 * picture looks like.  A human preview does: with fixed gains the frame is the
 * right colour only for the one light somebody tuned it in.
 *
 * Swapping the sensor does not change this.  The white balance is missing from
 * the PATH, not from the part.
 *
 * The control law lives here, as a pure function over measured means, so it can
 * be tested on the host -- a white balance that runs away is worth catching
 * without a camera.  The measuring and the applying are in camera.c, on the
 * producer thread.
 */
#ifndef CAM_AUTO_H
#define CAM_AUTO_H

#include <stdint.h>

#include "cam_convert.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * [!] THE EXPOSURE LOOP IS GONE (issue #54).  cam_ae_step() steered a sensor
 * that had no auto-exposure of its own; the part that needed it was the IMX219,
 * and it was removed.  The OV5647 runs its own on-chip AEC/AGC, and two
 * exposure loops on one sensor do not average out -- ours would measure a mean
 * the sensor had already corrected and correct it again, with the pair hunting
 * against each other.  What is left here is the white balance, which the
 * datapath has never provided for any part.
 */

/** Gain limits for the white balance, so a dark or monochrome scene cannot
 *  drive a channel to absurdity. */
#define CAM_AWB_GAIN_MIN 128u    /* 0.5x */
#define CAM_AWB_GAIN_MAX 1024u   /* 4.0x */

/**
 * @brief  One grey-world white balance step.
 *
 * @param b_x100,g_x100,r_x100  measured plane means, x100, linear
 * @param wb  gains updated in place; black level and gamma left alone
 * @return non-zero if @p wb changed
 *
 * Grey world assumes the scene averages to neutral, which is wrong for a frame
 * filled with one colour -- so the step is damped and clamped rather than
 * jumping to the computed answer, and green is left at unity so the other two
 * move toward it instead of all three drifting.
 */
int cam_awb_step(uint32_t b_x100, uint32_t g_x100, uint32_t r_x100,
                 struct cam_wb *wb);

#ifdef __cplusplus
}
#endif

#endif /* CAM_AUTO_H */
