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
 * [!] THE MODE TABLE DOES NOT PROGRAM VTS, AND THAT COST 4x THE FRAME RATE.
 *
 * The SDK's OV5647_mipi_2lane_640x480.i writes HTS (0x380C/0x380D = 1852) and
 * never touches 0x380E/0x380F, so the part kept its power-on frame length --
 * 1968 lines, which is the value the 2592x1944 mode wants.  A VGA mode running
 * on the 5 MP mode's frame length is 62.5 ms per frame: measured, and it is
 * exactly 1968 * 1852 / 58.3 MHz.
 *
 * Linux's ov5647 driver programmes 504 for this same mode (HTS 1852 and PLL
 * 0x3036 = 0x46 agree with the SDK table byte for byte), which is 16.0 ms and
 * about 62 fps.
 *
 * 984 rather than 504 is a deliberate middle.  The frame rate this port can
 * actually consume is bounded by its own producer -- 43.5 ms of capture,
 * convert and blit, about 23 fps (#38) -- so 504 would buy frames nothing can
 * use while halving the exposure ceiling to ~500 lines, which is where the
 * on-chip AEC already sits in ordinary room light.  984 puts the sensor at
 * ~32 fps, comfortably past what the pipeline consumes, and leaves twice the
 * exposure headroom.  `camera vts` moves it either way without a flash cycle.
 */
#define OV5647_DEFAULT_VTS  984u

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
