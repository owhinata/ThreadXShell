/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * CSI RX/TX FIFO fill level (issue #35).  See cam_mipi_calc.h for why this is
 * integer arithmetic rather than the SDK's double + ceil().
 *
 * THE DERIVATION, so the next reader does not have to redo it.  The vendor
 * writes (cis_imx219/cisdp_sensor.c, set_mipi_csirx_enable):
 *
 *     byte_clk  = bitrate_1lane / 8
 *     t_input   = (l_header + line_length*dpp/8 + l_footer) / (lanes*byte_clk)
 *                 + 0.06
 *     t_output  = line_length / pixel_clk
 *     t_preload = (7 + n_preload*4/lanes) / pixel_clk
 *     delta_t   = t_input - t_output - t_preload
 *     fill      = ceil(|delta_t| * byte_clk * lanes / 4 / (dpp/2)) * (dpp/2)
 *
 * with the fill going to RX when delta_t > 0 and to TX otherwise.  Note which
 * of those divisions are integer ones in the original: everything inside
 * `l_header + line_length*dpp/8 + l_footer` and inside `7 + n_preload*4/lanes`
 * is uint32 arithmetic, evaluated BEFORE the conversion to double.  Those two
 * truncations are part of the answer and are preserved here as A and Q.
 *
 * Writing delta_t as a single fraction over 50*lanes*byte_clk*pixel_clk (50
 * because 0.06 == 3/50) gives
 *
 *     delta_num = A*50*pixel_clk + 3*D*pixel_clk - (line_length + Q)*50*D
 *     delta_den = 50 * D * pixel_clk              where D = lanes * byte_clk
 *
 * and the ceiling then becomes a ceiling division, exact for every input:
 *
 *     fill = ceil_div(|delta_num| * byte_clk * lanes,
 *                     delta_den * 4 * (dpp/2)) * (dpp/2)
 *
 * For the shipping configuration (912 MHz/lane, 2 lanes, RAW10, 3280-pixel
 * lines, 200 MHz pixel clock) that is delta_num/delta_den = 3383000/2280000 =
 * 1.4838 us, and 85 into the RX FIFO -- the same values the double version
 * produces.  test/test_cam_mipi_calc.c pins that and the other branch.
 */
#include "cam_mipi_calc.h"

/* The vendor's two magic constants, named as it names them. */
#define L_HEADER   4u
#define L_FOOTER   2u
#define N_PRELOAD 15u

static uint64_t ceil_div_u64(uint64_t a, uint64_t b)
{
	return (a + b - 1u) / b;
}

int cam_mipi_fifo_fill(const struct cam_mipi_link *link,
                       struct cam_mipi_fifo_fill *out)
{
	uint64_t byte_clk, d, a, q;
	int64_t  delta_num;
	uint64_t delta_den, mag, unit, fill;

	if (link == 0 || out == 0)
		return -1;

	out->rx = 0u;
	out->tx = 0u;

	byte_clk = (uint64_t)link->bitrate_1lane_mhz / 8u;
	if (link->lanes == 0u || link->pixel_clk_mhz == 0u || byte_clk == 0u)
		return -1;
	/* (dpp/2) is the quantum the fill is rounded up to, and it divides the
	 * result: a zero would be a divide by zero, and the vendor only ever
	 * passes 8 or 10 (it refuses anything else before reaching here). */
	unit = (uint64_t)link->pixel_dpp / 2u;
	if (unit == 0u)
		return -1;

	d = (uint64_t)link->lanes * byte_clk;
	a = L_HEADER + (uint64_t)link->line_length * link->pixel_dpp / 8u +
	    L_FOOTER;
	q = 7u + (N_PRELOAD * 4u) / link->lanes;

	delta_num = (int64_t)(a * 50u * link->pixel_clk_mhz) +
	            (int64_t)(3u * d * link->pixel_clk_mhz) -
	            (int64_t)(((uint64_t)link->line_length + q) * 50u * d);
	delta_den = 50u * d * link->pixel_clk_mhz;

	mag = (delta_num < 0) ? (uint64_t)(-delta_num) : (uint64_t)delta_num;
	fill = ceil_div_u64(mag * byte_clk * link->lanes,
	                    delta_den * 4u * unit) * unit;

	/* The registers are 16-bit.  A fill that does not fit is not a value to
	 * clamp -- it means the link parameters describe something this
	 * datapath cannot absorb, and programming a truncated version of it
	 * would be the silent-tearing failure this whole file exists to
	 * avoid. */
	if (fill > 0xFFFFu)
		return -1;

	/* delta_t > 0: the link delivers a line faster than the datapath drains
	 * it, so the slack has to sit in the RECEIVER.  The vendor's `<= 0`
	 * boundary is reproduced exactly, zero included. */
	if (delta_num > 0)
		out->rx = (uint16_t)fill;
	else
		out->tx = (uint16_t)fill;

	return 0;
}
