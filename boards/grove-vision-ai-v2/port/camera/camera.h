/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    camera.h
 * @brief   IMX219 camera driver for Grove Vision AI V2 (issue #35).
 *
 * Same entry-point names and the same error codes as the f746g-disco and
 * wio-lite-ai camera drivers, so the shell layer reads identically across all
 * three firmwares even though nothing below this header is shared.
 *
 * WHAT THIS OWNS.  Lifecycle (bring-up as a transaction, one stop path), the
 * producer thread that turns datapath interrupts into published frames, and the
 * error state machine.  The sensor and datapath registers belong to
 * cam_imx219.h; the pixel packing to cam_convert.h; the ring and its sinks to
 * svc/frame_pipeline.h.
 *
 * THE FRAME PATH.  Datapath callback (interrupt) classifies the event and posts
 * a semaphore -> producer thread invalidates the WDMA3 buffer, packs planar
 * B/G/R into an RGB565 pipeline slot, and publishes it -> attached sinks
 * consume synchronously on that thread.  The interrupt never touches the ring.
 */
#ifndef CAMERA_H
#define CAMERA_H

#include <stdint.h>

#include "cam_convert.h"
#include "cam_imx219.h"
#include "frame_pipeline.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Error codes.  Same names and meanings as the other two boards' camera
 * drivers -- deliberately, so that a reader who knows one knows all three.
 */
#define CAM_OK             0
#define CAM_ERR_PARAM     -1  /**< bad argument                              */
#define CAM_ERR_HAL       -2  /**< sensor I2C or datapath call reported an error */
#define CAM_ERR_TIMEOUT   -3  /**< no frame arrived inside the bounded wait   */
#define CAM_ERR_STATE     -4  /**< wrong state for this call                  */
#define CAM_ERR_NO_SENSOR -5  /**< the sensor did not answer, or answered wrong */
#define CAM_ERR_NO_FRAME  -6  /**< nothing has been captured yet              */
#define CAM_ERR_BUSY      -7  /**< a stream already owns the datapath         */

/** @return a short description of a CAM_* code (never NULL). */
const char *camera_strerror(int rc);

/**
 * @brief  Create the driver's ThreadX objects.
 *
 * Call from tx_application_define(), next to lcd_create_objects().  The port's
 * rule is that a ThreadX object exists before any interrupt that touches it can
 * be enabled, and this driver's semaphores are posted from a datapath
 * interrupt.  Touches no hardware and performs no I/O.
 */
void camera_create_objects(void);

/** Snapshot of what `camera probe` reports. */
struct camera_probe_info {
	uint32_t chip_version; /**< SCU chip id; 0 if unreadable            */
	uint16_t sensor_id;    /**< 0x0219 (IMX219) or 0x5647 (OV5647)      */
	int      rev_c;        /**< chip needs the per-frame MIPI bounce    */
};

/**
 * @brief  Power the module, identify it, and leave it powered but idle.
 *
 * Brings the port up if it is not up already (see the transaction note in
 * camera.c) and reads the sensor's model ID.  This is the cheapest end-to-end
 * check that the module is present, seated and strapped as expected.
 */
int camera_probe(struct camera_probe_info *out);

/**
 * @brief  Capture exactly one frame and stop.
 *
 * Blocking, bounded, and it leaves the raw planar frame in the WDMA3 buffer for
 * camera_raw_frame() -- which is what lets `camera capture` report per-channel
 * statistics straight off the hardware's own output, before any packing this
 * firmware does can be blamed for the colours.
 */
int camera_capture(void);

/**
 * @brief  Capture one frame with the demosaic OUT of the path.
 *
 * camera_raw_frame() then holds the 320x240 8-bit Bayer mosaic the demosaic
 * would have been fed -- which is what lets the true Bayer phase be read off
 * the hardware instead of inferred from the sensor's mirror setting.
 */
int camera_capture_raw(void);

/** @return the WDMA3 buffer (CAM_RAW_BYTES: B, G then R planes), never NULL. */
const uint8_t *camera_raw_frame(void);

/**
 * @brief  Start / stop continuous capture on the producer thread.
 *
 * camera_stream_start() returns once the first frame is on its way; it does not
 * wait for one.  camera_stream_stop() is SYNCHRONOUS: it returns only after the
 * producer has passed through the full stop sequence and gone idle, so a caller
 * that then detaches a sink cannot race an in-flight consume.
 */
int camera_stream_start(void);
int camera_stream_stop(void);

/** Producer and pipeline counters, as `camera stats` prints them. */
struct camera_stats {
	int      streaming;
	uint32_t frames;        /**< frames published                         */
	uint32_t timeouts;      /**< bounded waits that expired               */
	uint32_t retries;       /**< restarts attempted after a timeout       */
	uint32_t csirx_errors;  /**< non-zero latched CSI receiver status     */
	uint32_t relock_fails;  /**< rev-C bounces that did not come back     */
	uint32_t dp_errors;     /**< terminal datapath events                 */
	int32_t  last_dp_status;/**< the datapath status that stopped us      */
	uint32_t overruns;      /**< no free slot at publish time             */
	const char *fault;      /**< first terminal reason, or NULL           */
};
void camera_stream_stats(struct camera_stats *out);

/**
 * @brief  Software white balance applied to every published frame.
 *
 * Unity by default, so an unbalanced preview shows what the sensor produced --
 * which is what keeps `camera capture`'s channel statistics meaning something.
 * The IMX219 has no per-channel gain registers, so a colour cast cannot be
 * corrected at the sensor; see cam_convert.h.
 */
/**
 * @brief  Set the sensor's exposure / gains, safely with respect to a stream.
 *
 * While a stream is running these QUEUE the change for the producer to apply
 * between frames, and return CAM_OK immediately.  Sensor I2C goes through the
 * vendor CIS driver, which has no locking, and the producer uses it too; and
 * between frames is also the only moment a rolling-shutter exposure change
 * does not tear a frame.  When nothing is streaming they apply directly.
 */
int camera_set_exposure(uint16_t lines);
int camera_set_gains(uint8_t again, uint16_t dgain);

/**
 * @brief  MIPI bits per pixel, 8 or 10 (see cam_imx219.h for why it matters).
 *
 * The receiver's half is programmed at the next datapath configuration, so a
 * change asked for mid-stream fully lands only on the next start.
 */
int camera_set_depth(uint8_t bits);

/** Set the frame length (see cam_imx219.h). */
int camera_set_frame_length(uint16_t lines);

/**
 * @brief  Read exposure and frame length back FROM the sensor.
 *
 * @return CAM_ERR_BUSY while a stream runs -- the read is I2C and the producer
 *         owns that driver.
 */
int camera_read_timing(uint16_t *exposure, uint16_t *frame_length);

/**
 * @brief  Auto exposure and auto white balance, on by default.
 *
 * The datapath provides neither, so a fixed setting is correct only for the
 * lighting it was tuned in.  Turn them OFF before taking measurements that
 * assume the sensor is holding still -- comparing Bayer phases, or reading
 * `camera capture` statistics twice -- since a loop adjusting the exposure
 * between two captures is a variable nobody asked for.
 *
 * Switching this also switches the sensor's own AEC/AGC where it has one, so it
 * writes I2C: queued for the producer while a stream runs (CAM_OK means queued,
 * not yet applied), otherwise applied under the API mutex, bringing the camera
 * up first so the write reaches the sensor that is actually fitted.  The mode is
 * kept here either way and re-applied at every bring-up, so it survives a camera
 * that is not up yet -- and survives the mode table being written again, which
 * on a sensor with its own AEC is what would otherwise switch it back on.
 *
 * @return CAM_OK -- including when the camera would not come up, since the
 *         request is kept and applied later; CAM_ERR_* only when a sensor that
 *         IS up refused the write.
 */
int camera_set_auto(int on);
int camera_auto(void);

void camera_set_wb(const struct cam_wb *wb);
void camera_get_wb(struct cam_wb *out);

/**
 * @brief  Attach / detach a frame sink (svc/frame_pipeline.h contract).
 *
 * Sinks may be attached before or during a stream.  Detaching while a stream
 * runs is allowed but the caller must stop the stream first if it intends to
 * free anything the sink points at -- consume() runs on the producer thread.
 */
int camera_subscribe(struct frame_sink *sink);
int camera_unsubscribe(struct frame_sink *sink);

/** Balance one push delivery.  Sinks call this exactly once per consume(). */
void camera_frame_put(struct frame_sink *sink, const struct frame_desc *f);

#ifdef __cplusplus
}
#endif

#endif /* CAMERA_H */
