/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    cam_auto.h
 * @brief   Auto exposure and auto white balance (issue #35).
 *
 * WHY THIS EXISTS.  The HX6538 datapath is a demosaic and nothing else -- there
 * is no auto exposure and no auto white balance anywhere in it, because the
 * applications it was built for feed the output to a neural network, which does
 * not care what the picture looks like.  A human preview does: with fixed
 * exposure and fixed gains, a frame is correct only for the one lighting
 * condition somebody tuned it in, and every other room is too dark, too bright,
 * or the wrong colour.
 *
 * Swapping the sensor does not change this.  The exposure loop and the white
 * balance are missing from the PATH, not from the part.
 *
 * The control laws live here, as pure functions over measured means, so they
 * can be tested on the host -- an exposure loop that oscillates or a white
 * balance that runs away are both things worth catching without a camera.  The
 * measuring and the applying are in camera.c, on the producer thread.
 */
#ifndef CAM_AUTO_H
#define CAM_AUTO_H

#include <stdint.h>

#include "cam_convert.h"

#ifdef __cplusplus
extern "C" {
#endif

/** What the exposure loop drives.  Both are sensor registers. */
struct cam_ae {
	uint16_t exposure;  /**< coarse integration time, lines  */
	uint8_t  again;     /**< analogue gain, 0..232           */
};

/*
 * Target mean for the green plane, x100, measured BEFORE the black level and
 * the encode -- i.e. in the sensor's own linear units.
 *
 * [!] IT DEPENDS ON WHETHER THE OUTPUT IS ENCODED, and that coupling is not
 * optional.  The loop is steering toward "what the panel shows", and the
 * transfer curve between what it measures and what the panel shows is exactly
 * the thing the gamma switch changes:
 *
 *   gamma ON  -- a linear mean of 60 lands near 128 after the sRGB curve, which
 *                is where the eye wants a midtone.  Aiming a LINEAR loop at 128
 *                here would over-expose by more than a stop and clip every
 *                highlight in the frame.
 *   gamma OFF -- the measurement IS what the panel gets, so the target is the
 *                midtone itself.  Left at 60 the picture would simply be dark,
 *                and the loop would sit there insisting it had succeeded.
 */
#define CAM_AE_TARGET_ENCODED_X100 6000u
#define CAM_AE_TARGET_LINEAR_X100 11000u

/** Analogue gain ceiling.  232 is the sensor's limit (10.7x) and is far too
 *  noisy to reach for; 208 is about 5.3x, past which the picture is grain. */
#define CAM_AE_AGAIN_MAX 208u

/** Exposure ceiling, lines.  Beyond the frame length the sensor stretches the
 *  frame, so this is a frame-rate floor as much as a brightness ceiling. */
#define CAM_AE_EXPOSURE_MAX 2700u
#define CAM_AE_EXPOSURE_MIN 32u

/**
 * @brief  One exposure step toward the target.
 *
 * @param green_x100   measured mean of the green plane, x100, linear
 * @param target_x100  where to aim; see the note on the two constants above --
 *                     the caller picks by whether its output is encoded
 * @param ae           current values, updated in place
 * @return non-zero if @p ae changed
 *
 * Gain is moved before exposure -- gain is free and exposure costs frame rate
 * and motion blur -- and every step is DAMPED (it closes part of the gap, not
 * all of it) with a deadband around the target.  Both matter: an undamped loop
 * against a sensor that applies changes a frame or two later is an oscillator,
 * and without a deadband it hunts for ever on sensor noise alone.
 */
int cam_ae_step(uint32_t green_x100, uint32_t target_x100,
                struct cam_ae *ae);

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
