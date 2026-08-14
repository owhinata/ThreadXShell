/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    cam_mipi_calc.h
 * @brief   CSI RX/TX FIFO fill level for the IMX219 link (issue #35).
 *
 * The MIPI receiver and transmitter each have a FIFO that absorbs the
 * difference between how fast a line arrives on the D-PHY and how fast the
 * datapath consumes it.  Programming the wrong fill level does not fail
 * cleanly -- it tears or drops frames under load, which looks exactly like a
 * marginal cable.  The vendor's own bring-up computes it per configuration
 * rather than tabulating it, and this reproduces that computation.
 *
 * Why it is not a straight copy.  The SDK writes the formula in `double` and
 * finishes with `ceil()`, and this firmware links no libm -- and would rather
 * not pull soft-float doubles into a bring-up path to evaluate an expression
 * whose inputs are all small integers.  The version here is exact integer
 * arithmetic over the same rational, so it agrees with the vendor's result for
 * every input rather than approximately: the divisions the SDK performs in
 * integers stay integer divisions, the 0.06 microsecond fudge stays exactly
 * 3/50, and `ceil` becomes a ceiling division.
 *
 * Keeping it in its own translation unit is what makes it host-testable, which
 * matters more here than usual: the alternative way to find out that the fill
 * level is wrong is to flash the board and look at a picture.
 */
#ifndef CAM_MIPI_CALC_H
#define CAM_MIPI_CALC_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Inputs to the fill-level computation, in the vendor's own units. */
struct cam_mipi_link {
	uint32_t bitrate_1lane_mhz; /**< per-lane bit rate, MHz (2 * link clock) */
	uint32_t lanes;             /**< active D-PHY data lanes                 */
	uint32_t pixel_dpp;         /**< bits per pixel on the link (10 for RAW10)*/
	uint32_t line_length;       /**< sensor line length in pixels            */
	uint32_t pixel_clk_mhz;     /**< MIPI pixel clock, MHz, as the SCU reports*/
};

/** Where the slack lands: exactly one of these is non-zero. */
struct cam_mipi_fifo_fill {
	uint16_t rx; /**< receiver fill: the link delivers faster than we consume */
	uint16_t tx; /**< transmitter fill: the other way round                   */
};

/**
 * @brief  Compute the RX/TX FIFO fill levels for one link configuration.
 *
 * @return 0 on success, -1 if @p link is unusable (a zero lane count, pixel
 *         clock, or bit rate -- each of which would be a divide by zero, and
 *         each of which really does happen when an SCU read-back fails and its
 *         return value is dropped, as the vendor's platform init does).
 */
int cam_mipi_fifo_fill(const struct cam_mipi_link *link,
                       struct cam_mipi_fifo_fill *out);

#ifdef __cplusplus
}
#endif

#endif /* CAM_MIPI_CALC_H */
