/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Host test for the CSI FIFO fill computation (issue #35,
 * port/camera/cam_mipi_calc.c).
 *
 * WHY THIS EXISTS.  The firmware replaced the SDK's floating-point formula with
 * integer arithmetic, for the good reason that this link has no libm -- but
 * "rewrote the vendor's formula" is exactly the kind of change that is right
 * for the configuration you checked and wrong for the one you did not, and the
 * symptom on hardware (occasional torn or dropped frames) is indistinguishable
 * from a marginal camera cable.
 *
 * So the test does not assert the rewrite against hand-computed numbers.  It
 * asserts it against THE ORIGINAL FORMULA, transcribed here in double precision
 * exactly as the SDK writes it, swept over every parameter that varies on this
 * board and a good deal more.  The two must agree on every input, not just on
 * the shipping one -- that is the property the rewrite claims, so that is the
 * property under test.
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>

#include "cam_mipi_calc.h"

static int failures;

#define CHECK(cond, ...)                                                       \
	do {                                                                   \
		if (!(cond)) {                                                 \
			printf("FAIL %s:%d: ", __FILE__, __LINE__);            \
			printf(__VA_ARGS__);                                   \
			printf("\n");                                          \
			failures++;                                            \
		}                                                              \
	} while (0)

/*
 * The reference: cis_imx219/cisdp_sensor.c set_mipi_csirx_enable(), verbatim
 * apart from the surrounding register writes.  Deliberately NOT tidied up --
 * every integer division here is one the original performs before converting to
 * double, and "cleaning that up" would silently change the answer.
 */
static void vendor_reference(uint32_t bitrate_1lane, uint32_t mipi_lnno,
                             uint32_t pixel_dpp, uint32_t line_length,
                             uint32_t mipi_pixel_clk,
                             uint16_t *rx_out, uint16_t *tx_out)
{
	uint32_t byte_clk = bitrate_1lane / 8;
	uint32_t n_preload = 15;
	uint32_t l_header = 4;
	uint32_t l_footer = 2;
	double t_input, t_output, t_preload, delta_t;
	uint16_t rx_fifo_fill = 0;
	uint16_t tx_fifo_fill = 0;

	t_input = (double)(l_header + line_length * pixel_dpp / 8 + l_footer) /
	          (mipi_lnno * byte_clk) + 0.06;
	t_output = (double)line_length / mipi_pixel_clk;
	t_preload = (double)(7 + (n_preload * 4) / mipi_lnno) / mipi_pixel_clk;

	delta_t = t_input - t_output - t_preload;

	if (delta_t <= 0) {
		delta_t = 0 - delta_t;
		tx_fifo_fill = (uint16_t)(ceil(delta_t * byte_clk * mipi_lnno /
		                               4 / (pixel_dpp / 2)) *
		                          (pixel_dpp / 2));
		rx_fifo_fill = 0;
	} else {
		rx_fifo_fill = (uint16_t)(ceil(delta_t * byte_clk * mipi_lnno /
		                               4 / (pixel_dpp / 2)) *
		                          (pixel_dpp / 2));
		tx_fifo_fill = 0;
	}

	*rx_out = rx_fifo_fill;
	*tx_out = tx_fifo_fill;
}

static void compare(uint32_t bitrate, uint32_t lanes, uint32_t dpp,
                    uint32_t line_length, uint32_t pixel_clk)
{
	struct cam_mipi_link link = { bitrate, lanes, dpp, line_length,
	                              pixel_clk };
	struct cam_mipi_fifo_fill got;
	uint16_t want_rx, want_tx;

	CHECK(cam_mipi_fifo_fill(&link, &got) == 0,
	      "refused a valid link (%lu/%lu/%lu/%lu/%lu)",
	      (unsigned long)bitrate, (unsigned long)lanes, (unsigned long)dpp,
	      (unsigned long)line_length, (unsigned long)pixel_clk);

	vendor_reference(bitrate, lanes, dpp, line_length, pixel_clk,
	                 &want_rx, &want_tx);

	CHECK(got.rx == want_rx && got.tx == want_tx,
	      "bitrate %lu lanes %lu dpp %lu line %lu pclk %lu: "
	      "got rx=%u tx=%u, vendor formula says rx=%u tx=%u",
	      (unsigned long)bitrate, (unsigned long)lanes, (unsigned long)dpp,
	      (unsigned long)line_length, (unsigned long)pixel_clk,
	      got.rx, got.tx, want_rx, want_tx);
}

/* ---- 1. the shipping configuration, pinned to its literal answer --------- */

static void test_shipping_configuration(void)
{
	/* IMX219 as this board drives it: 456 MHz link (so 912 Mbit/s/lane),
	 * two lanes, RAW10, 3280-pixel lines, and the 200 MHz MIPI pixel clock
	 * imx219_set_pll200() selects when the PLL is at 400 MHz.
	 *
	 * The literal is here on purpose.  Everything else in this file checks
	 * the rewrite against the reference formula, which would keep passing if
	 * BOTH were wrong about the units; this is the one assertion that would
	 * notice, and 85 is the number the vendor's code produces on the
	 * hardware the donor ships. */
	struct cam_mipi_link link = { 912u, 2u, 10u, 3280u, 200u };
	struct cam_mipi_fifo_fill got;

	CHECK(cam_mipi_fifo_fill(&link, &got) == 0, "the shipping link was "
	      "refused");
	CHECK(got.rx == 85u && got.tx == 0u,
	      "the shipping link gives rx=%u tx=%u, wanted rx=85 tx=0",
	      got.rx, got.tx);

	/* The result is a multiple of dpp/2 by construction; a rounding change
	 * that broke that would program a fill the FIFO cannot align to. */
	CHECK((got.rx % 5u) == 0u, "rx fill %u is not a multiple of dpp/2",
	      got.rx);
}

/* ---- 2. the other branch: a slow pixel clock puts the slack in TX -------- */

static void test_both_branches_are_reachable(void)
{
	struct cam_mipi_link link = { 912u, 2u, 10u, 3280u, 96u };
	struct cam_mipi_fifo_fill got;

	/* 96 MHz is the RC oscillator fallback the vendor's hscnt table calls
	 * "rc96" -- reachable on this board whenever the PLL read-back does not
	 * say 400 MHz.  There the datapath is the slow side, so the fill has to
	 * land in the transmitter instead. */
	CHECK(cam_mipi_fifo_fill(&link, &got) == 0, "the rc96 link was "
	      "refused");
	CHECK(got.tx != 0u && got.rx == 0u,
	      "at a 96 MHz pixel clock the fill went to rx=%u tx=%u; the slack "
	      "belongs in the transmitter", got.rx, got.tx);
	compare(912u, 2u, 10u, 3280u, 96u);
}

/* ---- 3. agreement with the vendor formula across a sweep ----------------- */

static void test_matches_vendor_formula(void)
{
	static const uint32_t bitrates[]  = { 400u, 456u * 2u, 800u, 1000u,
	                                      1500u, 2000u };
	static const uint32_t laneset[]   = { 1u, 2u, 4u };
	static const uint32_t dpps[]      = { 8u, 10u };
	static const uint32_t lines[]     = { 320u, 640u, 1280u, 3280u, 4056u };
	static const uint32_t pclks[]     = { 96u, 200u, 300u, 400u };
	size_t b, l, d, n, p;

	for (b = 0; b < sizeof bitrates / sizeof bitrates[0]; b++)
	 for (l = 0; l < sizeof laneset / sizeof laneset[0]; l++)
	  for (d = 0; d < sizeof dpps / sizeof dpps[0]; d++)
	   for (n = 0; n < sizeof lines / sizeof lines[0]; n++)
	    for (p = 0; p < sizeof pclks / sizeof pclks[0]; p++)
		compare(bitrates[b], laneset[l], dpps[d], lines[n], pclks[p]);
}

/* ---- 4. the boundary the sign test sits on ------------------------------- */

static void test_zero_delta_goes_to_tx(void)
{
	/*
	 * The vendor branches on `delta_t <= 0`, so an exactly-zero delta puts a
	 * zero fill in TX rather than in RX.  The distinction is invisible in
	 * the programmed value (both are zero) but the rewrite has to reproduce
	 * the comparison rather than an approximation of it, because `>= 0` and
	 * `> 0` differ for every input that lands on the boundary -- and integer
	 * arithmetic, unlike the double version, can land on it exactly.
	 *
	 * Sweeping the pixel clock is how the boundary gets crossed; every step
	 * is checked against the reference, so whichever one straddles zero is
	 * checked too.
	 */
	uint32_t pclk;

	for (pclk = 90u; pclk <= 400u; pclk++)
		compare(912u, 2u, 10u, 3280u, pclk);
}

/* ---- 5. inputs that would divide by zero are refused, not computed ------- */

static void test_degenerate_inputs_refused(void)
{
	struct cam_mipi_fifo_fill got;
	struct cam_mipi_link link;

	link = (struct cam_mipi_link){ 912u, 0u, 10u, 3280u, 200u };
	CHECK(cam_mipi_fifo_fill(&link, &got) == -1, "a zero lane count was "
	      "accepted");

	link = (struct cam_mipi_link){ 912u, 2u, 10u, 3280u, 0u };
	CHECK(cam_mipi_fifo_fill(&link, &got) == -1, "a zero pixel clock was "
	      "accepted -- this is what a dropped SCU read-back looks like");

	link = (struct cam_mipi_link){ 0u, 2u, 10u, 3280u, 200u };
	CHECK(cam_mipi_fifo_fill(&link, &got) == -1, "a zero bit rate was "
	      "accepted");

	link = (struct cam_mipi_link){ 912u, 2u, 1u, 3280u, 200u };
	CHECK(cam_mipi_fifo_fill(&link, &got) == -1, "dpp=1 was accepted; "
	      "dpp/2 is a divisor");

	CHECK(cam_mipi_fifo_fill(NULL, &got) == -1, "a NULL link was accepted");
}

int main(void)
{
	test_shipping_configuration();
	test_both_branches_are_reachable();
	test_matches_vendor_formula();
	test_zero_delta_goes_to_tx();
	test_degenerate_inputs_refused();

	if (failures != 0) {
		printf("test_cam_mipi_calc: %d failure(s)\n", failures);
		return 1;
	}
	printf("test_cam_mipi_calc: OK\n");
	return 0;
}
