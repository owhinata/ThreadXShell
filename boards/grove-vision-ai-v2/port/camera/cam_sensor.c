/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/*
 * Sensor dispatch and lifecycle (issues #35, #36).
 *
 * Which part is fitted, powering it, and every call that reaches it -- all of
 * which go through the descriptor rather than at a register directly, so the
 * per-part files stay the only place a register address appears.
 *
 * The board wiring lives here too (the connector's enable and xshutdown), not
 * in the per-part file: it is a property of this board, not of what is plugged
 * into it.
 */
#include <stddef.h>
#include <stdint.h>

#include "hx_drv_CIS_common.h"
#include "hx_drv_gpio.h"
#include "hx_drv_scu.h"
#include "hx_drv_scu_export.h"

#include "cam_dp.h"
#include "cam_sensor.h"
#include "timebase.h"

#define LOG_TAG "sensor"
#include "log.h"

/*
 * Board wiring, not sensor properties: the module connector's enable is a GPIO
 * on this board rather than the SDK's default xSleep path.  Named CAM_* rather
 * than after a part -- they outlived the IMX219 (issue #54) and would have been
 * lies as IMX219_*.
 */
#define CAM_ENABLE_GPIO     AON_GPIO1
#define CAM_XSHUTDOWN_PIN   AON_GPIO2

/*
 * The live exposure/gain values.  Updated by the setters and by the read-back,
 * so that the command reports what is actually in the sensor rather than a
 * default it hopes is still true.
 *
 * [!] These are a SHADOW, and on this part they are not the truth on their own:
 * the OV5647's on-chip AEC moves the real exposure without telling anyone, so
 * cam_sensor_refresh_exposure_gains() is what makes them mean anything.
 */
static uint16_t cam_exposure;
static uint8_t  cam_again;
static uint16_t cam_dgain = 0x0100u;   /* unity; the OV5647 has no dgain */

/* The frame length this port last programmed.  0 until a bring-up has run, and
 * deliberately NOT seeded from the descriptor: what matters is what was
 * written, and before a bring-up nothing was. */
static uint16_t cam_vts;

/* The parts this port knows how to drive.  One entry today; it stays a table
 * because that is the shape a second part arrives in, and because detection has
 * to have something to iterate. */
static const struct cam_sensor_desc *const cam_sensors[] = {
	&cam_sensor_ov5647,
};

#define CAM_SENSOR_COUNT (sizeof cam_sensors / sizeof cam_sensors[0])

/* The one that answered on I2C.  Defaults to the first so that everything has
 * a sane geometry before a probe has happened. */
static const struct cam_sensor_desc *sens = cam_sensors[0];

const struct cam_sensor_desc *cam_sensor_current(void) { return sens; }

const char *cam_sensor_name(void) { return sens->name; }
uint16_t cam_sensor_id(void)      { return sens->id_value; }

/* ---- helpers ------------------------------------------------------------- */

int cam_sensor_write_table(HX_CIS_SensorSetting_t *tbl, uint16_t n,
                           const char *what)
{
	if (hx_drv_cis_setRegTable(tbl, n) != HX_CIS_NO_ERROR) {
		LOG_ERR("sensor i2c write failed: %s", what);
		return -1;
	}
	return 0;
}

/* ---- power / identity ---------------------------------------------------- */

int cam_sensor_power_on(void)
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
	if (cam_sensor_detect() != 0)
		return -1;
	if (hx_drv_cis_set_slaveID(sens->i2c_id) != HX_CIS_NO_ERROR) {
		LOG_ERR("hx_drv_cis_set_slaveID(0x%02X) failed", sens->i2c_id);
		return -1;
	}
	/* The detected part's default -- unless somebody has already measured
	 * an answer from the console, in which case theirs wins.  The phase is
	 * the DATAPATH's setting (it configures the HW5x5), seeded from the part
	 * that is fitted, which is why it is handed over rather than kept here. */
	cam_dp_seed_bayer(sens->bayer);
	return 0;
}

void cam_sensor_power_off(void)
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

int cam_sensor_read_id(uint16_t *id)
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
int cam_sensor_detect(void)
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
		        cam_sensors[i]->name, cam_sensors[i]->i2c_id);

		if (read_id_of(cam_sensors[i], &id) == 0 &&
		    id == cam_sensors[i]->id_value) {
			sens = cam_sensors[i];
			LOG_INF("sensor %s detected (id %04X at 0x%02X)",
			        sens->name, id, sens->i2c_id);
			return 0;
		}
	}
	LOG_ERR("no known sensor answered on I2C");
	return -1;
}

int cam_sensor_init(void)
{
	/* Stream off first: the mode table is not safe to apply to a streaming
	 * sensor, and the module may still be streaming from a previous run
	 * that ended in a reset rather than a stop. */
	if (cam_sensor_write_table(sens->off, sens->off_n, "stream off") != 0 ||
	    cam_sensor_write_table(sens->init, sens->init_n, "mode table") != 0)
		return -1;
	if (sens->tune != NULL &&
	    cam_sensor_write_table(sens->tune, sens->tune_n, "tuning") != 0)
		return -1;

	/*
	 * [!] THE FRAME LENGTH, WHICH THE MODE TABLE DOES NOT SET (issue #38).
	 *
	 * The SDK's OV5647 table writes HTS and never writes VTS, so without this
	 * the part runs a VGA mode on whatever frame length it powered up with --
	 * measured at 1968 lines, the value the 2592x1944 mode wants, and 62.5 ms
	 * per frame.  Re-applied at every bring-up for the same reason the auto
	 * mode is: the mode table has just gone back in.
	 *
	 * A console setting wins over the descriptor default, so a value found on
	 * the bench is not undone by a fault recovery -- the same rule the Bayer
	 * phase follows.
	 *
	 * Exposure and gain need no such re-apply on this part: its mode table IS
	 * the whole configuration and its own AEC runs the exposure.  (A part
	 * without one would need them written here.  The IMX219 was that part;
	 * issue #54 removed it.)
	 */
	if (sens->set_frame_length != NULL) {
		uint16_t want = (cam_vts != 0u) ? cam_vts : sens->default_vts;

		if (want != 0u) {
			if (sens->set_frame_length(want) != 0)
				return -1;
			cam_vts = want;
		}
	}
	return 0;
}

int cam_sensor_set_frame_length(uint16_t lines)
{
	if (sens->set_frame_length == NULL)
		return -1;
	if (sens->set_frame_length(lines) != 0)
		return -1;
	cam_vts = lines;
	return 0;
}

int cam_sensor_read_frame_length(uint16_t *lines)
{
	if (sens->read_frame_length == NULL)
		return -1;
	return sens->read_frame_length(lines);
}

uint16_t cam_sensor_frame_length(void)
{
	return cam_vts;
}

int cam_sensor_refresh_exposure_gains(void)
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

int cam_sensor_set_exposure(uint16_t lines)
{
	if (sens->set_exposure == NULL)
		return -1;
	if (sens->set_exposure(lines) != 0)
		return -1;
	cam_exposure = lines;
	return 0;
}

int cam_sensor_set_gains(uint8_t again, uint16_t dgain)
{
	if (sens->set_gains == NULL)
		return -1;
	if (sens->set_gains(again, dgain) != 0)
		return -1;
	cam_again = again;
	cam_dgain = dgain;
	return 0;
}

int cam_sensor_set_auto(int on)
{
	/* A part with no on-chip AEC/AGC has nothing to hand the sensor half to,
	 * so both directions are already satisfied and reporting a failure would
	 * make the caller announce a fault that does not exist. */
	if (sens->set_auto == NULL)
		return 0;
	return sens->set_auto(on);
}

void cam_sensor_get_exposure_gains(uint16_t *lines, uint8_t *again,
                                   uint16_t *dgain)
{
	if (lines != NULL)
		*lines = cam_exposure;
	if (again != NULL)
		*again = cam_again;
	if (dgain != NULL)
		*dgain = cam_dgain;
}

int cam_sensor_stream_on(void)
{
	return cam_sensor_write_table(sens->on, sens->on_n, "stream on");
}

int cam_sensor_stream_off(void)
{
	return cam_sensor_write_table(sens->off, sens->off_n, "stream off");
}
