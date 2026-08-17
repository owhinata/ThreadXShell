/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    cam_sensor.h
 * @brief   The camera SENSOR: identity, mode tables, exposure and gain (#36).
 *
 * The seam between "the part in the connector" and "the HX6538 datapath that
 * reduces and demosaics what it sends".  cam_dp.h is the other side.
 *
 * Everything here is thread context only.  Nothing may be called from the
 * datapath callback: these functions do I2C, spin on read-backs and take
 * milliseconds.
 */
#ifndef CAM_SENSOR_H
#define CAM_SENSOR_H

#include <stdint.h>

#include "hx_drv_CIS_common.h"

#ifdef __cplusplus
extern "C" {
#endif

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

	/*
	 * Frame length in lines -- the sensor's VTS.  NULL on a part whose
	 * frame length this port does not know how to reach.
	 *
	 * [!] THIS IS THE FRAME RATE AND THE EXPOSURE CEILING AT ONCE (#38).
	 * A frame is VTS line times long, so the period is VTS * HTS / PCLK;
	 * and integration time cannot exceed the frame, so lowering VTS for
	 * frame rate lowers the longest exposure available with it.  Any
	 * default here is a choice between the two, which is why it is also a
	 * runtime knob.
	 *
	 * The `default_vts` below is what the port programmes after the mode
	 * table.  0 means "leave whatever the table left", which is what this
	 * port did until #38 found that the OV5647 table never writes VTS at
	 * all and the part was running a VGA mode on the 5 MP mode's frame
	 * length.
	 */
	int (*set_frame_length)(uint16_t lines);
	int (*read_frame_length)(uint16_t *lines);
	uint16_t default_vts;
};

/** The part that answered on I2C.  Never NULL: it starts at the first entry so
 *  the geometry is sane before a probe has happened. */
const struct cam_sensor_desc *cam_sensor_current(void);

/**
 * @brief  Push a register table, naming it if the write fails.
 *
 * Shared with the per-sensor files rather than duplicated: every table write in
 * this port reports the same way, and the one thing worth getting right is that
 * a failure says WHICH table.
 */
int cam_sensor_write_table(HX_CIS_SensorSetting_t *tbl, uint16_t n,
                           const char *what);

#define WRITE_TABLE(t) \
	cam_sensor_write_table((t), (uint16_t)(sizeof (t) / sizeof (t)[0]), #t)

/** Table-and-count, for filling a descriptor's register-table fields. */
#define TBL(t) (t), (uint16_t)(sizeof (t) / sizeof (t)[0])

/** The parts this port knows how to drive; one file each. */
extern const struct cam_sensor_desc cam_sensor_ov5647;

/**
 * @brief  Ask each known part, at its own I2C address, who it is.
 *
 * With one part in the table this is an identity CHECK rather than a choice,
 * and it stays a loop over descriptors because that is the shape a second part
 * would arrive in -- and because asking is still the only way to tell a module
 * that is absent from one that is wired wrong.  Called from the power-up path;
 * the answer selects the register tables, the geometry, the link rate and the
 * default Bayer phase.
 */
int cam_sensor_detect(void);

/** @return the detected sensor's name ("ov5647"). */
const char *cam_sensor_name(void);

/** @return the model ID the DETECTED sensor is expected to answer with. */
uint16_t cam_sensor_id(void);

/**
 * @brief  Power the module and open the sensor's I2C channel.
 *
 * Drives PA1/AON_GPIO1 high -- on this board that is the IMX219's enable, not
 * the SDK's default xSleep path -- and initialises the CIS layer.  Idempotent.
 */
int cam_sensor_power_on(void);

/** Drop the module's enable line.  Safe to call when it is already down. */
void cam_sensor_power_off(void);

/**
 * @brief  Read the sensor's 16-bit model ID over I2C.
 *
 * The cheapest end-to-end proof that the module is powered, strapped to the
 * expected I2C address and talking.  Reads whichever part cam_sensor_detect()
 * selected -- the OV5647 answers 0x5647 at 0x300A/0x300B.
 */
int cam_sensor_read_id(uint16_t *id);

/** Push the init tables: mode, binning, exposure, gains, mirror. */
int cam_sensor_init(void);

/**
 * @brief  Set the sensor's exposure and gains at RUNTIME.
 *
 * Being able to change them from the console is what keeps finding the right
 * values from costing a flash cycle each, on a board whose external NOR is
 * rated ~100k of those.
 *
 * [!] On this part BOTH also switch its on-chip AEC/AGC to manual (0x3503), or
 * the sensor's own loop writes over the value on its next frame and the command
 * appears to do nothing.
 *
 * @param lines  integration time, in lines (0x3500..0x3502, sixteenths)
 * @param again  analogue gain on the console's 0..232 curve (0x350A/0x350B)
 * @param dgain  digital gain, 0x0100 == 1.0; no OV5647 equivalent, ignored
 */
int cam_sensor_set_exposure(uint16_t lines);
int cam_sensor_set_gains(uint8_t again, uint16_t dgain);

/**
 * @brief  Hand exposure back to (or take it from) the sensor's own AEC.
 *
 * The OV5647 has an on-chip AEC/AGC and the IMX219 does not, so setting an
 * exposure by hand on the OV5647 must also switch that loop off -- otherwise it
 * overwrites the value on its next frame and the command appears to do nothing.
 *
 * @return 0 on success, including on a part with no on-chip loop -- there both
 *         directions are already true and there is nothing to write.
 */
int cam_sensor_set_auto(int on);

/**
 * @brief  Re-read the sensor's exposure and gain into the reported values.
 *
 * A no-op for a part this port drives itself -- there the shadow copies ARE the
 * truth.  For a part running its own AEC they are the only truth available, and
 * without this the console reports whatever was last written by hand while the
 * real exposure moves underneath, which makes a working auto-exposure look
 * broken.  I2C: producer thread only while a stream runs.
 */
int cam_sensor_refresh_exposure_gains(void);

/** The values currently programmed (what this driver last wrote, or read). */
void cam_sensor_get_exposure_gains(uint16_t *lines, uint8_t *again,
                                   uint16_t *dgain);

/** Sensor stream on/off.  Checked, unlike the donor's. */
int cam_sensor_stream_on(void);
int cam_sensor_stream_off(void);

/**
 * @brief  Set / read the sensor's frame length (VTS), in lines.
 *
 * [!] One register, two effects (issue #38): the frame period is
 * VTS * HTS / PCLK, and the integration time cannot exceed the frame -- so
 * asking for a faster frame rate is asking for a shorter longest exposure.
 *
 * @return 0 on success; -1 on a part whose VTS this port cannot reach, and -1
 *         from the setter for a value below what the mode actually outputs.
 */
int cam_sensor_set_frame_length(uint16_t lines);
int cam_sensor_read_frame_length(uint16_t *lines);

/** The frame length this port last programmed, or 0 before it programmed one. */
uint16_t cam_sensor_frame_length(void);

#ifdef __cplusplus
}
#endif

#endif /* CAM_SENSOR_H */
