/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/*
 * OV5647, in the donor's 640x480 binned mode (issues #35, #36).
 *
 * ONE FILE PER PART is the point: what differs between sensors is more than a
 * register table -- exposure and gain have no layout in common, and SCCB
 * acknowledges a write to an address that means something else on another part,
 * so "wrong part" would look like "supported".  A file boundary makes the seam
 * something the compiler helps with rather than a convention.
 */
#include <stddef.h>
#include <stdint.h>

#include "hx_drv_CIS_common.h"
#include "hx_drv_hw5x5.h"
#include "hx_drv_inp.h"

#include "cam_sensor.h"

#define LOG_TAG "ov5647"
#include "log.h"

static int ov5647_do_exposure(uint16_t lines);
static int ov5647_do_gains(uint8_t again, uint16_t dgain);
static int ov5647_do_auto(int on);
static int ov5647_read_eg(uint16_t *lines, uint8_t *again);
static int ov5647_do_frame_length(uint16_t lines);
static int ov5647_read_frame_length(uint16_t *lines);

/*
 * [!] THE MODE TABLE DOES NOT PROGRAM VTS, so this port does (issue #38).
 *
 * The SDK's OV5647_mipi_2lane_640x480.i writes HTS (0x380C/0x380D = 1852) and
 * never touches 0x380E/0x380F, leaving the part on whatever frame length it
 * powered up with -- 1968 lines, which is what the 2592x1944 mode wants.
 * Programming it explicitly is the point, and the VALUE is a measured choice.
 *
 * TWO CONDITIONS DECIDE IT, and they are not the same condition.
 *
 * The CAPTURE period is quantised, because the datapath is one-shot: after the
 * producer arms it, the capture waits for the next frame start and then takes a
 * whole active frame to arrive.
 *
 *     period = T_s * ceil((W + T_active) / T_s)
 *
 * The DISPLAY needs the panel thread to keep up:
 *
 *     B <= period         B = the panel thread's service time (the `blit` row)
 *
 * [!] THAT USED TO BE `S + B < period` -- A SUM, not a max (issue #64), where S
 * was the inference the sink runs on the producer.  Issue #71 removed the S
 * term by returning the pipeline pin at the staging seam instead of after the
 * transfer: the release now happens inside consume(), so the window in which
 * the sink counts as busy is contained in S rather than following it.  The
 * measured hold is 775 us against a 26.5 ms transfer.  Anything derived from
 * the old sum -- the candidate table this comment used to carry, and the 880 it
 * chose -- was recomputed rather than adjusted.
 *
 * MEASURED at issue #71, board rev D, 200-frame runs:
 *
 *     T_active = 15.1 ms          B = 26,437..26,466 us
 *     hold     = 773..784 us      line time = 31.70..31.84 us
 *     W = 32,223 us for `nn preview` (S = 23,937), 9,201 for `camera preview`
 *
 * So `nn preview` needs T_s > W + T_active = 47,323 us to take N=1, and that is
 * now the only binding condition -- B is comfortably under it.
 *
 *     VTS   nn preview   camera preview   margin on 47,323
 *     880      17.8 fps       35.7 fps     (N=2)
 *    1480      10.6           --            -262 us
 *    1500      16.8           --             +317 us
 *    1550      20.2           20.3         +1,964 us
 *    1600      19.6           --           +3,554 us
 *
 * WHY 1550.  The best measured `nn preview` with a margin that survived, which
 * is not the same as the best measured `nn preview`.
 *
 * [!] 1500 IS WHY THE MARGIN IS NOT TRIMMED.  Its 317 us is 0.67%, and the line
 * time itself moves 31.70..31.84 across measurement points -- +-0.2%, or +-95 us
 * at this frame length -- so the margin sat inside the model's own error.  It
 * measured 16.8 fps against a predicted 21.0, and the period came out 59,469 us,
 * which is NOT a multiple of T_s: about a quarter of the frames slipped to N=2.
 * That is the second demonstration of this after issue #38's VTS 1540/1580, and
 * the first where it appeared as a FRACTION of frames rather than as every other
 * one.  1480 is the negative control: 411 us the other side of the boundary,
 * 10.6 fps, exactly N=2.
 *
 * [!] AND THE LINE TIME IS NOT 31.507 us.  That figure is HTS 1852 / PCLK
 * 58.8 MHz; every N=1 run gives period/VTS about 0.9% higher.  Predictions from
 * the datasheet figure run optimistic by roughly 450 us at these frame lengths
 * -- the same order as the margin being reasoned about.  `camera vts` explores
 * all of this without a flash.
 *
 * THE PRICE IS `camera preview`, and it is not small: 35.7 -> 20.3 fps.  Its own
 * W is only 9.2 ms, so its period is entirely T_s, and a single frame length
 * cannot serve both -- `nn preview` wants T_s above 47.3 ms, `camera preview`
 * wants it as small as B allows.  This value chooses `nn preview`, which is what
 * issue #56 is about; `camera vts 880` hands the other one back at runtime.
 *
 * The EXPOSURE ceiling, which the frame length also bounds, moves the other way
 * for once: 1550 allows 49.3 ms of integration against 28.0 at 880, so a dim
 * scene gets more light rather than less.
 *
 * [!] RE-MEASURE AFTER ANY CHANGE TO W OR B.  Issue #59 removes T_active from
 * the capture condition altogether, at which point every row above moves.
 */
#define OV5647_DEFAULT_VTS  1550u

/* The mode outputs 480 lines; VTS below that plus the part's own blanking is
 * not a frame.  Linux's 504 for this mode is the practical floor, and it is
 * what this refuses below. */
#define OV5647_MIN_VTS      504u

/* ---- register tables ----------------------------------------------------- */

/*
 * OV5647, in its 640x480 binned mode -- the sensor does the first reduction
 * itself, which is the whole reason this is the part the port keeps.
 *
 * The mode table comes straight out of the SDK tree.  Including it rather than
 * copying it keeps it tied to the pinned SHA: it is several hundred register
 * writes of undocumented sensor state, and a stale private copy of that is a
 * debugging session nobody would enjoy.
 */
/* The SDK's table ends with a stream-off written through these two names, so
 * they have to exist before the include.  Same values the donor's cisdp_cfg.h
 * gives them; the on/off tables below repeat them for the same registers. */
#define OV5647_MIPI_CTRL_OFF 0x01
#define OV5647_MIPI_CTRL_ON  0x14

static HX_CIS_SensorSetting_t ov5647_init_setting[] = {
#include "OV5647_mipi_2lane_640x480.i"
};

/* MIPI on/off, not the SMIA 0x0100 an IMX219-family part would use. */
static HX_CIS_SensorSetting_t ov5647_stream_on[] = {
	{ HX_CIS_I2C_Action_W, 0x4800, OV5647_MIPI_CTRL_ON },
	{ HX_CIS_I2C_Action_W, 0x4202, 0x00 },
};

static HX_CIS_SensorSetting_t ov5647_stream_off[] = {
	{ HX_CIS_I2C_Action_W, 0x4800, OV5647_MIPI_CTRL_OFF },
	{ HX_CIS_I2C_Action_W, 0x4202, 0x0F },
};

/*
 * Exposure is 20 bits across three registers in SIXTEENTHS of a line, and gain
 * is 10 bits where 16 means 1x -- a layout nothing else shares, which is why
 * these are function pointers on the descriptor and not a shared table with
 * different addresses.
 *
 * Setting either by hand also has to take the sensor's own AEC/AGC OFF
 * (0x3503), or the on-chip loop simply writes over the value on its next frame
 * and the command appears to do nothing.
 */
static int ov5647_do_exposure(uint16_t lines)
{
	uint32_t e16 = (uint32_t)lines * 16u;      /* 1/16-line units */
	HX_CIS_SensorSetting_t tbl[] = {
		{ HX_CIS_I2C_Action_W, 0x3503, 0x03 },      /* manual AEC+AGC */
		{ HX_CIS_I2C_Action_W, 0x3500, (e16 >> 16) & 0x0F },
		{ HX_CIS_I2C_Action_W, 0x3501, (e16 >> 8) & 0xFF },
		{ HX_CIS_I2C_Action_W, 0x3502, e16 & 0xFF },
	};

	return WRITE_TABLE(tbl);
}

static int ov5647_do_gains(uint8_t again, uint16_t dgain)
{
	/* The console's `again` is a 0..232 curve (inherited from the IMX219 the
	 * port also drove, and kept because it is what `camera gain` documents);
	 * map it onto the OV5647's linear 16-means-1x.  dgain has no OV5647
	 * equivalent and is ignored. */
	uint32_t g = (again >= 255u) ? 1023u : (16u * 256u) / (256u - again);

	HX_CIS_SensorSetting_t tbl[] = {
		{ HX_CIS_I2C_Action_W, 0x3503, 0x03 },
		{ HX_CIS_I2C_Action_W, 0x350a, (g >> 8) & 0x03 },
		{ HX_CIS_I2C_Action_W, 0x350b, g & 0xFF },
	};

	(void)dgain;
	if (g > 1023u)
		g = 1023u;
	return WRITE_TABLE(tbl);
}

static int ov5647_do_auto(int on)
{
	HX_CIS_SensorSetting_t tbl[] = {
		{ HX_CIS_I2C_Action_W, 0x3503, 0x00 },
	};
	HX_CIS_SensorSetting_t off[] = {
		{ HX_CIS_I2C_Action_W, 0x3503, 0x03 },
	};

	return on ? WRITE_TABLE(tbl) : WRITE_TABLE(off);
}

/*
 * OV5647 exposure is 20 bits in sixteenths of a line across 0x3500..0x3502;
 * gain is 10 bits at 0x350A/0x350B where 16 means 1x.  Both are converted into
 * the units the console uses -- lines, and the 0..232 gain curve -- so the
 * report and the command are on one scale.
 */
static int ov5647_read_eg(uint16_t *lines, uint8_t *again)
{
	uint8_t a = 0u, b = 0u, c = 0u;
	uint32_t e16, g;

	if (hx_drv_cis_get_reg(0x3500, &a) != HX_CIS_NO_ERROR ||
	    hx_drv_cis_get_reg(0x3501, &b) != HX_CIS_NO_ERROR ||
	    hx_drv_cis_get_reg(0x3502, &c) != HX_CIS_NO_ERROR)
		return -1;
	e16 = ((uint32_t)(a & 0x0Fu) << 16) | ((uint32_t)b << 8) | c;
	if (lines != NULL)
		*lines = (uint16_t)((e16 / 16u) > 0xFFFFu ? 0xFFFFu : e16 / 16u);

	if (hx_drv_cis_get_reg(0x350a, &a) != HX_CIS_NO_ERROR ||
	    hx_drv_cis_get_reg(0x350b, &b) != HX_CIS_NO_ERROR)
		return -1;
	g = ((uint32_t)(a & 0x03u) << 8) | b;
	if (again != NULL) {
		/* g/16 is the multiplier; invert 256/(256-again). */
		*again = (g <= 16u) ? 0u : (uint8_t)(256u - (256u * 16u) / g);
	}
	return 0;
}

static int ov5647_do_frame_length(uint16_t lines)
{
	HX_CIS_SensorSetting_t tbl[] = {
		{ HX_CIS_I2C_Action_W, 0x380e, (lines >> 8) & 0xFF },
		{ HX_CIS_I2C_Action_W, 0x380f, lines & 0xFF },
	};

	if (lines < OV5647_MIN_VTS) {
		LOG_ERR("frame length %u is below the %u this mode needs",
		        lines, (unsigned)OV5647_MIN_VTS);
		return -1;
	}
	return WRITE_TABLE(tbl);
}

static int ov5647_read_frame_length(uint16_t *lines)
{
	uint8_t hi = 0u, lo = 0u;

	if (hx_drv_cis_get_reg(0x380e, &hi) != HX_CIS_NO_ERROR ||
	    hx_drv_cis_get_reg(0x380f, &lo) != HX_CIS_NO_ERROR)
		return -1;
	if (lines != NULL)
		*lines = (uint16_t)(((uint16_t)hi << 8) | lo);
	return 0;
}

const struct cam_sensor_desc cam_sensor_ov5647 = {
	/*
	 * 640x480 straight out of the sensor, halved to 320x240 by a
	 * 4:2 INP bin.  Mirror is left off, so the phase is the native
	 * one; `camera bayer` settles it on the bench if this proves
	 * wrong.
	 */
	.name = "ov5647", .i2c_id = 0x36u,
	.id_reg = 0x300Au, .id_value = 0x5647u,
	.init = TBL(ov5647_init_setting),
	.on   = TBL(ov5647_stream_on),
	.off  = TBL(ov5647_stream_off),
	.tune = NULL, .tune_n = 0u,
	.sensor_w = 640u, .sensor_h = 480u,
	.crop_w = 0u, .crop_h = 0u,           /* no INP crop */
	.binning = (uint8_t)INP_BINNING_4TO2_B,
	.subsample = (uint8_t)INP_SUBSAMPLE_DISABLE,
	.mipi_clock_mhz = 220u,
	.dpp = 10u,                            /* RAW10, as the SDK cfg */
	/*
	 * BGGR: the OV5647's native order, with the mirror left off.
	 * An earlier RGGB here came from the donor cfg's
	 * mirror-to-phase table and was wrong -- on the bench a BLUE
	 * object came out YELLOW, which is the signature of red and
	 * blue swapped (the blue photosites read as red, and red plus
	 * the surviving green is yellow).  Linux's ov5647 driver
	 * reports SBGGR10 for the same reason.
	 */
	.bayer = (uint8_t)DEMOS_PATTENMODE_BGGR,
	.set_exposure = ov5647_do_exposure,
	.set_gains = ov5647_do_gains,
	.set_auto = ov5647_do_auto,
	.read_exposure_gain = ov5647_read_eg,
	.set_frame_length = ov5647_do_frame_length,
	.read_frame_length = ov5647_read_frame_length,
	.default_vts = OV5647_DEFAULT_VTS,
};
