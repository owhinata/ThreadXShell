/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * OV5647 + HX6538 datapath glue (issue #35).  See cam_imx219.h for what this
 * is a port OF and why it is a port rather than a compile of the SDK file.
 *
 * Source: sdk/EPII_CM55M_APP_S/app/scenario_app/tflm_yolov8_od/cis_sensor/
 * cis_imx219/{cisdp_sensor.c,cisdp_cfg.h}, at the SHA cmake/himax_sdk.cmake
 * pins.  The call ORDER below is the donor's and should be treated as vendor
 * knowledge -- there is no public TRM for this part, so "why does the PA1 write
 * happen twice, once before the pinmux and once after" has no answer beyond
 * "that is what the shipping code does, on a bus that is not documented".
 *
 * [!] THE FILE NAME NO LONGER MATCHES ITS CONTENTS (issue #54).  The IMX219 was
 * removed and the OV5647 is the only part this port drives; splitting the
 * datapath out and renaming what remains is issue #36, deliberately kept
 * separate so that the removal reads as a removal.
 */
#include <stddef.h>
#include <stdint.h>

#include "hx_drv_CIS_common.h"
#include "hx_drv_csirx.h"
#include "hx_drv_gpio.h"
#include "hx_drv_hw5x5.h"
#include "hx_drv_inp.h"
#include "hx_drv_scu.h"
#include "hx_drv_scu_export.h"
#include "hx_drv_swreg_aon.h"
#include "driver_interface.h"
#include "sensor_dp_lib.h"

#include "cam_imx219.h"
#include "cam_mipi_calc.h"
#include "timebase.h"

#define LOG_TAG "camera"
#include "log.h"

/* ---- the link, as this board wires it ----------------------------------- */

/*
 * Board wiring, not sensor properties: the module connector's enable is a GPIO
 * on this board rather than the SDK's default xSleep path, and the link is two
 * lanes whatever is plugged into it.  Named CAM_* rather than after a part --
 * they outlived the IMX219 (issue #54) and would have been lies as IMX219_*.
 */
#define CAM_MIPI_LANES      2u
#define CAM_ENABLE_GPIO     AON_GPIO1
#define CAM_XSHUTDOWN_PIN   AON_GPIO2

/*
 * The WDMA3 landing buffer.
 *
 * In SRAM and NOT in TCM, for the same reason the LCD frame buffer is: the DMA
 * engines on this part cannot see TCM, and a transfer to a TCM address does not
 * fault -- it simply never arrives.  32-byte aligned because the CPU has to
 * invalidate it a cache line at a time before reading each frame, and an
 * invalidate that starts mid-line would either miss bytes or discard a
 * neighbour's.  Its own section so the placement gate can pin the address and
 * the size (cmake/check_placement_budget.py); the donor's `mm_reserve` bump
 * allocator puts it wherever the heap happened to be, which no gate can check.
 */
static uint8_t cam_raw_buf[CAM_RAW_BYTES]
	__attribute__((section(".cam_raw"), aligned(32)));

_Static_assert(sizeof cam_raw_buf == 230400u,
               "the WDMA3 buffer is not one 320x240 planar BGR frame");
_Static_assert((CAM_RAW_BYTES % 32u) == 0u,
               "the WDMA3 buffer is not a whole number of cache lines");

uint8_t *cam_imx219_raw_buffer(void)
{
	return cam_raw_buf;
}

/* ---- the sensor ----------------------------------------------------------- */

/*
 * ONE PART, AND WHY IT IS THIS ONE (issue #54).
 *
 * The port carried both the IMX219 and the OV5647 for a while, because the two
 * modules are physically interchangeable in the same connector.  What the bench
 * settled is that they were never a like-for-like choice -- the donor asks each
 * part to do something quite different, and only one of them is the SDK's
 * shipping configuration:
 *
 *   IMX219   streams its FULL 3280x2464 and makes the HX6538's INP do a 10.25x
 *            reduction -- a five-pixel box average followed by a decimation.
 *            Soft, and it aliases.  The sensor also has no auto-exposure, so
 *            the frame is correct only for whatever fixed exposure was last
 *            set, which is what the port's own AE loop existed to paper over.
 *
 *   OV5647   is programmed to emit 640x480 ITSELF, using the sensor's own
 *            binning, and the INP then halves it with a 4:2 bin.  Two clean
 *            steps instead of one crude one, and it runs its own on-chip
 *            AEC/AGC -- which is why the donor never writes an exposure for it.
 *
 * The descriptor and the function pointers stay even with a single entry: they
 * are the seam between "the datapath" and "the part", and issue #36 is about
 * making that seam a file boundary rather than removing it.
 */
struct cam_sensor_desc {
	const char *name;
	uint8_t  i2c_id;
	uint16_t id_reg;        /* first of two consecutive ID registers  */
	uint16_t id_value;      /* what those two bytes must read         */

	HX_CIS_SensorSetting_t *init;
	uint16_t init_n;
	HX_CIS_SensorSetting_t *on;
	uint16_t on_n;
	HX_CIS_SensorSetting_t *off;
	uint16_t off_n;
	/* Applied after `init`; NULL when the mode table is the whole story. */
	HX_CIS_SensorSetting_t *tune;
	uint16_t tune_n;

	uint16_t sensor_w, sensor_h;   /* what the sensor actually emits   */
	uint16_t crop_w, crop_h;       /* 0 = no INP crop                  */
	uint8_t  binning;              /* INP_BINNING_E                    */
	uint8_t  subsample;            /* INP_SUBSAMPLE_E                  */

	uint32_t mipi_clock_mhz;
	uint8_t  dpp;                  /* MIPI bits per pixel              */
	uint8_t  bayer;                /* default demosaic phase           */

	/*
	 * [!] Per sensor, because register maps have NOTHING in common between
	 * parts.  The OV5647 keeps exposure at 0x3500..0x3502 and gain at
	 * 0x350A/0x350B; the IMX219 kept them at 0x015A/0x015B and 0x0157.
	 * Writing one part's addresses to another does not fail -- it writes
	 * whatever those addresses happen to mean there, which is the worst kind
	 * of "supported", and is why this is a pointer and not a shared table
	 * with a different base.
	 */
	int (*set_exposure)(uint16_t lines);
	int (*set_gains)(uint8_t again, uint16_t dgain);
	/* Hand exposure back to the sensor's own loop.  NULL when it has none. */
	int (*set_auto)(int on);
	/*
	 * Read the sensor's CURRENT exposure and gain back.
	 *
	 * Needed for exactly the part whose exposure this port does not drive:
	 * with the OV5647's on-chip AEC running, shadow copies of what WE last
	 * wrote are meaningless -- they sit still while the real exposure moves.
	 * Reporting them made a working auto-exposure look broken.
	 */
	int (*read_exposure_gain)(uint16_t *lines, uint8_t *again);
};

static int ov5647_do_exposure(uint16_t lines);
static int ov5647_do_gains(uint8_t again, uint16_t dgain);
static int ov5647_do_auto(int on);
static int ov5647_read_eg(uint16_t *lines, uint8_t *again);

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
 * The demosaic's Bayer phase.  The descriptor's value is the DEFAULT, not a
 * certainty -- see the note on cam_imx219_set_bayer() for why, and for what a
 * wrong one looks like.
 */
static uint8_t cam_bayer = (uint8_t)DEMOS_PATTENMODE_BGGR;
/* Set once the console has chosen a phase, so that a bring-up after a fault
 * does not quietly put the sensor default back and undo a bench measurement. */
static uint8_t cam_bayer_user;

void cam_imx219_set_bayer(uint8_t pattern)
{
	if (pattern <= (uint8_t)DEMOS_PATTENMODE_RGGB) {
		cam_bayer = pattern;
		cam_bayer_user = 1u;
	}
}

uint8_t cam_imx219_bayer(void)
{
	return cam_bayer;
}

const char *cam_imx219_bayer_name(uint8_t pattern)
{
	switch (pattern) {
	case DEMOS_PATTENMODE_BGGR: return "bggr";
	case DEMOS_PATTENMODE_GBRG: return "gbrg";
	case DEMOS_PATTENMODE_GRBG: return "grbg";
	case DEMOS_PATTENMODE_RGGB: return "rggb";
	default:                    return "?";
	}
}

/*
 * The live exposure/gain values.  Updated by the setters and by the read-back,
 * so that the command reports what is actually in the sensor rather than a
 * default it hopes is still true.
 *
 * [!] These are a SHADOW, and on this part they are not the truth on their own:
 * the OV5647's on-chip AEC moves the real exposure without telling anyone, so
 * cam_imx219_refresh_exposure_gains() is what makes them mean anything.
 */
static uint16_t cam_exposure;
static uint8_t  cam_again;
static uint16_t cam_dgain = 0x0100u;   /* unity; the OV5647 has no dgain */

#define TBL(t) (t), (uint16_t)(sizeof (t) / sizeof (t)[0])

static struct cam_sensor_desc cam_sensors[] = {
	{
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
	},
};

#define CAM_SENSOR_COUNT (sizeof cam_sensors / sizeof cam_sensors[0])

/* The one that answered on I2C.  Defaults to the first so that everything has
 * a sane geometry before a probe has happened. */
static struct cam_sensor_desc *sens = &cam_sensors[0];

const char *cam_imx219_sensor_name(void) { return sens->name; }
uint16_t cam_imx219_sensor_id(void)      { return sens->id_value; }

/* ---- helpers ------------------------------------------------------------- */

static int write_table(HX_CIS_SensorSetting_t *tbl, uint16_t n,
                       const char *what)
{
	if (hx_drv_cis_setRegTable(tbl, n) != HX_CIS_NO_ERROR) {
		LOG_ERR("sensor i2c write failed: %s", what);
		return -1;
	}
	return 0;
}

#define WRITE_TABLE(t) \
	write_table((t), (uint16_t)(sizeof (t) / sizeof (t)[0]), #t)

/* ---- power / identity ---------------------------------------------------- */

int cam_imx219_power_on(void)
{
	if (hx_drv_cis_init((CIS_XHSHUTDOWN_INDEX_E)CAM_XSHUTDOWN_PIN,
	                    SENSORCTRL_MCLK_DIV3) != HX_CIS_NO_ERROR) {
		LOG_ERR("hx_drv_cis_init failed");
		return -1;
	}

	/* The donor's order, kept: drive the level, THEN mux the pad to the
	 * GPIO, then drive it again.  Setting it once after the mux is the
	 * obvious simplification and is also how you get a glitch on the
	 * module's enable while the pad is still in its reset function. */
	(void)hx_drv_gpio_set_output(CAM_ENABLE_GPIO, GPIO_OUT_HIGH);
	(void)hx_drv_scu_set_PA1_pinmux(SCU_PA1_PINMUX_AON_GPIO1, 1);
	(void)hx_drv_gpio_set_out_value(CAM_ENABLE_GPIO, GPIO_OUT_HIGH);

	/* The datasheet asks for a settling time after the supply comes up
	 * before the first I2C transaction; the donor spends it inside its own
	 * bring-up.  udelay() rather than the vendor timer API: TIMER2 belongs
	 * to the profile kit and hx_drv_timer_* is barred from this image. */
	udelay(1000u);

	/* Which part is actually in the connector decides the geometry, the
	 * link rate and the register tables, so it has to be settled before
	 * anything else is programmed. */
	if (cam_imx219_detect() != 0)
		return -1;
	if (hx_drv_cis_set_slaveID(sens->i2c_id) != HX_CIS_NO_ERROR) {
		LOG_ERR("hx_drv_cis_set_slaveID(0x%02X) failed", sens->i2c_id);
		return -1;
	}
	/* The detected part's default -- unless somebody has already measured
	 * an answer from the console, in which case theirs wins. */
	if (!cam_bayer_user)
		cam_bayer = sens->bayer;
	return 0;
}

void cam_imx219_power_off(void)
{
	(void)hx_drv_gpio_set_out_value(CAM_ENABLE_GPIO, GPIO_OUT_LOW);
}

/* Read the two ID bytes of whichever descriptor is currently selected. */
static int read_id_of(const struct cam_sensor_desc *d, uint16_t *id)
{
	uint8_t hi = 0u, lo = 0u;

	if (hx_drv_cis_set_slaveID(d->i2c_id) != HX_CIS_NO_ERROR)
		return -1;
	if (hx_drv_cis_get_reg(d->id_reg, &hi) != HX_CIS_NO_ERROR ||
	    hx_drv_cis_get_reg((uint16_t)(d->id_reg + 1u), &lo) != HX_CIS_NO_ERROR)
		return -1;
	*id = (uint16_t)(((uint16_t)hi << 8) | lo);
	return 0;
}

int cam_imx219_read_id(uint16_t *id)
{
	if (id == NULL)
		return -1;
	if (read_id_of(sens, id) != 0) {
		LOG_ERR("%s model id read failed", sens->name);
		return -1;
	}
	return 0;
}

/*
 * Ask each known part, at its own I2C address, who it is.
 *
 * Auto-detection rather than a build option because the two modules are
 * physically interchangeable in the same connector -- a build flag would mean a
 * flash cycle to swap camera, on a part whose NOR is rated ~100k of them, to
 * discover something the sensor will simply tell you.
 */
int cam_imx219_detect(void)
{
	uint32_t i;

	for (i = 0u; i < CAM_SENSOR_COUNT; i++) {
		uint16_t id = 0u;

		/* Say what is being tried BEFORE trying it.  A probe of an
		 * address with nothing on it makes the vendor I2C driver log
		 * its own failure, and an unexplained "dw_iic_write err_code:
		 * -60" in dmesg reads as a fault rather than as this loop
		 * doing exactly what it is for. */
		LOG_INF("probing %s at 0x%02X (an i2c error here just means "
		        "it is not the one fitted)",
		        cam_sensors[i].name, cam_sensors[i].i2c_id);

		if (read_id_of(&cam_sensors[i], &id) == 0 &&
		    id == cam_sensors[i].id_value) {
			sens = &cam_sensors[i];
			LOG_INF("sensor %s detected (id %04X at 0x%02X)",
			        sens->name, id, sens->i2c_id);
			return 0;
		}
	}
	LOG_ERR("no known sensor answered on I2C");
	return -1;
}

int cam_imx219_sensor_init(void)
{
	/* Stream off first: the mode table is not safe to apply to a streaming
	 * sensor, and the module may still be streaming from a previous run
	 * that ended in a reset rather than a stop. */
	if (write_table(sens->off, sens->off_n, "stream off") != 0 ||
	    write_table(sens->init, sens->init_n, "mode table") != 0)
		return -1;
	if (sens->tune != NULL &&
	    write_table(sens->tune, sens->tune_n, "tuning") != 0)
		return -1;

	/*
	 * Nothing to re-apply after the mode table on this part: it IS the whole
	 * configuration, and the sensor's own AEC runs the exposure.  (A part
	 * without one needs exposure, gain and mirror written here, because
	 * re-running the mode table puts the vendor's constants back and would
	 * otherwise silently undo whatever the console had dialled in.  The
	 * IMX219 was that part; issue #54 removed it.)
	 */
	return 0;
}

/*
 * OV5647.  Exposure is 20 bits across three registers in SIXTEENTHS of a line,
 * and gain is 10 bits where 16 means 1x -- a layout nothing else shares, which
 * is why these are function pointers on the descriptor and not a shared table
 * with different addresses.
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

int cam_imx219_refresh_exposure_gains(void)
{
	uint16_t lines = cam_exposure;
	uint8_t again = cam_again;

	if (sens->read_exposure_gain == NULL)
		return 0;                    /* the shadow is already true */
	if (sens->read_exposure_gain(&lines, &again) != 0)
		return -1;
	cam_exposure = lines;
	cam_again = again;
	return 0;
}

int cam_imx219_set_exposure(uint16_t lines)
{
	if (sens->set_exposure == NULL)
		return -1;
	if (sens->set_exposure(lines) != 0)
		return -1;
	cam_exposure = lines;
	return 0;
}

int cam_imx219_set_gains(uint8_t again, uint16_t dgain)
{
	if (sens->set_gains == NULL)
		return -1;
	if (sens->set_gains(again, dgain) != 0)
		return -1;
	cam_again = again;
	cam_dgain = dgain;
	return 0;
}

int cam_imx219_set_sensor_auto(int on)
{
	/* A part with no on-chip AEC/AGC has nothing to hand the sensor half to,
	 * so both directions are already satisfied and reporting a failure would
	 * make the caller announce a fault that does not exist. */
	if (sens->set_auto == NULL)
		return 0;
	return sens->set_auto(on);
}

void cam_imx219_get_exposure_gains(uint16_t *lines, uint8_t *again,
                                   uint16_t *dgain)
{
	if (lines != NULL)
		*lines = cam_exposure;
	if (again != NULL)
		*again = cam_again;
	if (dgain != NULL)
		*dgain = cam_dgain;
}

int cam_imx219_stream_on(void)
{
	return write_table(sens->on, sens->on_n, "stream on");
}

int cam_imx219_stream_off(void)
{
	return write_table(sens->off, sens->off_n, "stream off");
}

/* ---- MIPI receiver ------------------------------------------------------- */

/*
 * Point the MIPI clock at the PLL.  This is a DP-domain clock mux and divider,
 * NOT the system or CPU clock tree: the board rule is that the app inherits
 * what the bootloader configured, and this does not touch any of it.
 */
static uint32_t imx219_select_mipi_clock(void)
{
	SCU_PDHSC_DPCLK_CFG_T cfg;
	uint32_t pllfreq = 0u;
	uint32_t mipi_pixel_clk = 0u;

	if (hx_drv_scu_get_pdhsc_dpclk_cfg(&cfg) != SCU_NO_ERROR) {
		LOG_ERR("dp clock config read failed");
		return 0u;
	}

	hx_drv_swreg_aon_get_pllfreq(&pllfreq);

	cfg.mipiclk.hscmipiclksrc = SCU_HSCMIPICLKSRC_PLL;
	cfg.mipiclk.hscmipiclkdiv = (pllfreq == 400000000u) ? 1u : 0u;

	if (hx_drv_scu_set_pdhsc_dpclk_cfg(cfg, 0, 1) != SCU_NO_ERROR) {
		LOG_ERR("dp clock config write failed");
		return 0u;
	}

	/* Read the rate BACK rather than predicting it.  The SDK's own platform
	 * init drops this call's return value, so a failed read there is
	 * indistinguishable from a good one; here a failure has to be a
	 * failure, because everything downstream (the HS counts and the FIFO
	 * fill) is computed from this number. */
	if (hx_drv_scu_get_freq(SCU_CLK_FREQ_TYPE_HSC_MIPI_RXCLK,
	                        &mipi_pixel_clk) != SCU_NO_ERROR) {
		LOG_ERR("mipi rx clock read-back failed");
		return 0u;
	}
	return mipi_pixel_clk / 1000000u;
}

static void imx219_set_hscnt(uint32_t mipi_pixel_clk_mhz)
{
	MIPIRX_DPHYHSCNT_CFG_T hscnt;

	hscnt.mipirx_dphy_hscnt_clk_en = 0;
	hscnt.mipirx_dphy_hscnt_ln0_en = 1;
	hscnt.mipirx_dphy_hscnt_ln1_en = 1;
	hscnt.mipirx_dphy_hscnt_clk_val = 0x03;

	/* The vendor's table, unchanged.  There is no formula behind it in any
	 * document available for this part. */
	if (mipi_pixel_clk_mhz == 200u) {
		hscnt.mipirx_dphy_hscnt_ln0_val = 0x10;
		hscnt.mipirx_dphy_hscnt_ln1_val = 0x10;
	} else if (mipi_pixel_clk_mhz == 300u) {
		hscnt.mipirx_dphy_hscnt_ln0_val = 0x18;
		hscnt.mipirx_dphy_hscnt_ln1_val = 0x18;
	} else {
		hscnt.mipirx_dphy_hscnt_ln0_val = 0x06;
		hscnt.mipirx_dphy_hscnt_ln1_val = 0x06;
	}

	sensordplib_csirx_set_hscnt(hscnt);
}

int cam_imx219_csirx_enable(void)
{
	struct cam_mipi_link link;
	struct cam_mipi_fifo_fill fill;
	SCU_DP_SWRESET_T swrst;
	SCU_VMUTE_CFG_T vmute;
	uint32_t mipi_pixel_clk_mhz;

	mipi_pixel_clk_mhz = imx219_select_mipi_clock();
	if (mipi_pixel_clk_mhz == 0u)
		return -1;

	link.bitrate_1lane_mhz = sens->mipi_clock_mhz * 2u;
	link.lanes             = CAM_MIPI_LANES;
	link.pixel_dpp         = sens->dpp;
	link.line_length       = sens->sensor_w;
	link.pixel_clk_mhz     = mipi_pixel_clk_mhz;

	if (cam_mipi_fifo_fill(&link, &fill) != 0) {
		LOG_ERR("mipi fifo fill undefined for a %lu MHz pixel clock",
		        (unsigned long)mipi_pixel_clk_mhz);
		return -1;
	}

	/* Reset both PHYs.  Note this reads the CURRENT reset state and puts
	 * back exactly what it found for every other block -- the DP software
	 * reset register covers more than MIPI, and writing a whole-register
	 * constant here would reset peripherals this port does not own. */
	if (drv_interface_get_dp_swreset(&swrst) != DRIVER_INTERFACE_NO_ERROR) {
		LOG_ERR("dp software reset state read failed");
		return -1;
	}
	swrst.HSC_MIPIRX = 0;
	swrst.HSC_MIPITX = 0;
	(void)hx_drv_scu_set_DP_SWReset(swrst);
	udelay(50u);
	swrst.HSC_MIPIRX = 1;
	swrst.HSC_MIPITX = 1;
	(void)hx_drv_scu_set_DP_SWReset(swrst);

	imx219_set_hscnt(mipi_pixel_clk_mhz);

	sensordplib_csirx_set_pixel_depth(sens->dpp);
	sensordplib_csirx_set_deskew(0);
	sensordplib_csirx_set_fifo_fill(fill.rx);
	sensordplib_csirx_enable(CAM_MIPI_LANES);

	/*
	 * The transmitter leg.  This port sends nothing out over CSI, but the
	 * datapath's INP stage is fed through the TX block on this part and the
	 * vendor brings it up unconditionally -- dropping it is not a saving,
	 * it is a datapath that never produces a frame.
	 */
	sensordplib_csitx_set_dphy_clkmode(CSITX_DPHYCLOCK_CONT);
	sensordplib_csitx_set_pixel_depth(sens->dpp);
	sensordplib_csitx_set_deskew(0);
	sensordplib_csitx_set_fifo_fill(fill.tx);
	sensordplib_csitx_enable(CAM_MIPI_LANES,
	                         sens->mipi_clock_mhz * 2u,
	                         sens->sensor_w, sens->sensor_h);

	vmute.timingsrc = SCU_VMUTE_CTRL_TIMING_SRC_VMUTE;
	vmute.txphypwr  = SCU_VMUTE_CTRL_TXPHY_PWR_DISABLE;
	vmute.ctrlsrc   = SCU_VMUTE_CTRL_SRC_SW;
	vmute.swctrl    = SCU_VMUTE_CTRL_SW_ENABLE;
	(void)hx_drv_scu_set_vmute(&vmute);

	return 0;
}

void cam_imx219_csirx_disable(void)
{
	sensordplib_csirx_disable();
}

int cam_imx219_csirx_clear_errors(void)
{
	int rc = 0;

	if (hx_drv_csirx_clr_errirq_state() != CSIRX_NO_ERROR)
		rc = -1;
	if (hx_drv_csirx_clr_dphyerrirq_state() != CSIRX_NO_ERROR)
		rc = -1;
	if (rc != 0)
		LOG_ERR("csirx error status clear failed");
	return rc;
}

void cam_imx219_csirx_errors(struct cam_csirx_errors *out)
{
	uint32_t err = 0u, dphy = 0u;

	if (out == NULL)
		return;

	out->readable = 1;
	if (hx_drv_csirx_get_errirq_state(&err) != CSIRX_NO_ERROR)
		out->readable = 0;
	if (hx_drv_csirx_get_dphyerrirq_state(&dphy) != CSIRX_NO_ERROR)
		out->readable = 0;

	out->err     = err;
	out->dphyerr = dphy;
}

/* ---- datapath ------------------------------------------------------------ */

/* The INP chain: crop, bin, subsample.  Shared by both output legs. */
static int cam_imx219_inp_config(void)
{
	INP_CROP_T crop = { 0 };

	crop.start_x = 0;
	crop.start_y = 0;
	crop.last_x  = (sens->crop_w != 0u) ? (uint16_t)(sens->crop_w - 1u) : 0u;
	crop.last_y  = (sens->crop_h != 0u) ? (uint16_t)(sens->crop_h - 1u) : 0u;

	/*
	 * Two very different reductions, which is the point of carrying both
	 * parts:
	 *   imx219  3280x2464 -> crop 3200x2400 -> 10:2 bin -> 640x480 -> 4:2
	 *           subsample -> 320x240.  One 10.25x step, box-averaged then
	 *           decimated -- soft, and it aliases.
	 *   ov5647  640x480 out of the SENSOR -> 4:2 bin -> 320x240.  The
	 *           sensor does the hard part with its own binning.
	 * Every "NTO2" here works in Bayer PAIRS, which is what keeps the
	 * mosaic phase intact across the reduction.
	 */
	if (sensordplib_set_sensorctrl_inp_wi_crop_bin(
	            SENSORDPLIB_SENSOR_HM2130, SENSORDPLIB_STREAM_NONEAOS,
	            sens->sensor_w, sens->sensor_h,
	            (INP_SUBSAMPLE_E)sens->subsample, crop,
	            (INP_BINNING_E)sens->binning) != 0) {
		LOG_ERR("inp crop/bin/subsample config failed");
		return -1;
	}
	return 0;
}

/*
 * The RAW leg: INP -> WDMA2, no demosaic.
 *
 * This exists to answer questions the demosaiced output cannot.  What lands in
 * the buffer is the Bayer MOSAIC the HW5x5 would have been fed, so the true
 * phase can be READ OFF it -- the two green positions are the highest and
 * nearly equal -- instead of inferred from the sensor's mirror setting; and the
 * 8-bit samples the MIPI receiver made of the sensor's RAW10 can be looked at
 * directly rather than argued about.
 *
 * One frame is 320x240 = 76,800 bytes, so it shares the WDMA3 buffer.
 */
int cam_imx219_datapath_config_raw(void)
{
	sensordplib_set_xDMA_baseaddrbyapp(0u, (uint32_t)(uintptr_t)cam_raw_buf,
	                                   0u);
	if (cam_imx219_inp_config() != 0)
		return -1;
	sensordplib_set_raw_wdma2(CAM_FRAME_WIDTH, CAM_FRAME_HEIGHT, NULL);
	return 0;
}

int cam_imx219_datapath_config(void)
{
	/*
	 * [!] ZERO-INITIALISED, and that is not decoration.
	 *
	 * HW5x5_CFG_T has twelve fields and the demosaic path uses eight of
	 * them; the other three (fir_procmode, firlpf_bndmode, fir_lbp_th)
	 * belong to the FIR path.  The donor leaves them unset -- i.e. passes
	 * whatever was on the stack -- and the vendor entry point is called
	 * hx_drv_hw5x5_set_allCfg(), which is not a name that suggests it skips
	 * the fields the current path does not use.
	 *
	 * Two of them are shared with the demosaic in the SDK's own words:
	 * firlpf_bndmode is "FIR *and LPF* boundary extend mode" and the
	 * neighbouring demoslpf_roundmode is "Demosaic *and LPF* rounding mode".
	 * So stack garbage here can plausibly reach the block that produces the
	 * picture.  Zeroing gives FIR_PROCMODE_LBP1 / FIRLPF_BNDODE_EXTEND0 /
	 * threshold 0 -- defined values, the same on every boot, whether or not
	 * the hardware ends up caring.
	 */
	HW5x5_CFG_T hw5x5 = { 0 };

	/*
	 * WDMA1 and WDMA2 are left at zero deliberately.  Disassembly of
	 * libsensordp.a shows the HW5x5 -> WDMA3 path (setup_dp_hw5x5_wdma3 ->
	 * setup_sensor_dp_hw5x5_xdma1) reads g_dp_wdma3_addr and nothing else;
	 * the other two are for the JPEG and raw legs this port does not build.
	 * Zero rather than a scratch buffer so that a future path change which
	 * DOES use them faults at once instead of quietly writing into memory
	 * that belongs to something else.
	 */
	sensordplib_set_xDMA_baseaddrbyapp(0u, 0u,
	                                   (uint32_t)(uintptr_t)cam_raw_buf);

	if (cam_imx219_inp_config() != 0)
		return -1;

	hw5x5.hw5x5_path         = HW5x5_PATH_THROUGH_DEMOSAIC;
	hw5x5.demos_bndmode      = DEMOS_BNDODE_REFLECT;
	hw5x5.demos_color_mode   = DEMOS_COLORMODE_RGB;
	hw5x5.demos_pattern_mode = (DEMOS_PATTENMODE_E)cam_bayer;
	hw5x5.demoslpf_roundmode = DEMOSLPF_ROUNDMODE_FLOOR;
	hw5x5.hw55_crop_stx      = 0;
	hw5x5.hw55_crop_sty      = 0;
	hw5x5.hw55_in_width      = CAM_FRAME_WIDTH;
	hw5x5.hw55_in_height     = CAM_FRAME_HEIGHT;

	/* NULL callback: this port registers its own with
	 * hx_dplib_register_cb() instead of going through the SDK's
	 * event_handler scheduler. */
	sensordplib_set_hw5x5_wdma3(hw5x5, NULL);
	return 0;
}

int cam_imx219_capture_start(void)
{
	sensordplib_set_mclkctrl_xsleepctrl_bySCMode();
	if (sensordplib_set_sensorctrl_start() != 0) {
		LOG_ERR("sensor controller start failed");
		return -1;
	}
	return 0;
}

void cam_imx219_retrigger(void)
{
	sensordplib_retrigger_capture();
}

void cam_imx219_full_stop(void)
{
	/* The vendor's order, and every failure path in this port funnels
	 * through it.  Nothing here checks a return value or bails out early:
	 * this runs when things have ALREADY gone wrong, and a stop that gives
	 * up half way leaves a datapath that is neither running nor quiet. */
	sensordplib_stop_capture();
	sensordplib_start_swreset();
	sensordplib_stop_swreset_WoSensorCtrl();
	(void)cam_imx219_stream_off();
	sensordplib_csirx_disable();
}

/* ---- chip revision ------------------------------------------------------- */

uint32_t cam_imx219_chip_version(void)
{
	uint32_t chipid = 0u, version = 0u;

	if (hx_drv_scu_get_version(&chipid, &version) != SCU_NO_ERROR)
		return 0u;
	return chipid;
}

int cam_imx219_needs_rev_c_bounce(void)
{
	return cam_imx219_chip_version() == CAM_CHIP_VERSION_C;
}
