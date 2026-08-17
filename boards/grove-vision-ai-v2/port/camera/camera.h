/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    camera.h
 * @brief   OV5647 camera driver for Grove Vision AI V2 (issue #35).
 *
 * Same entry-point names and the same error codes as the f746g-disco and
 * wio-lite-ai camera drivers, so the shell layer reads identically across all
 * three firmwares even though nothing below this header is shared.
 *
 * WHAT THIS OWNS.  Lifecycle (bring-up as a transaction, one stop path), the
 * producer thread that turns datapath interrupts into published frames, and the
 * error state machine.  The sensor and datapath registers belong to
 * cam_dp.h and cam_sensor.h; the pixel packing to cam_convert.h; the ring and
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
#include "cam_dp.h"
#include "cam_sensor.h"
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
 * The producer thread's ThreadX priority.
 *
 * Exported since issue #57, when this port grew a SECOND frame-path thread: the
 * LCD sink's panel thread is placed relative to THIS number, and since issue #64
 * it sits strictly above it.  The reasoning lives at CAM_PANEL_PRIO, next to the
 * assert that enforces it -- a relationship that matters is asserted where the
 * second thread is declared rather than restated as a second literal, because
 * two numbers describing one ordering is how the ordering gets broken.  Its
 * stack size stays private to camera.c; nothing else needs it.
 */
#define CAM_PRODUCER_PRIO 10u

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
	uint16_t sensor_id;    /**< 0x5647 (OV5647)                         */
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

/**
 * @return the completed raw frame (CAM_RAW_BYTES: B, G then R planes), never
 *         NULL.
 *
 * [!] EXCLUSIVITY CONTRACT (issue #59).  There are TWO landing buffers and the
 * pointer MOVES between frames, so a plain index is the only synchronisation
 * there is -- which is sound exactly as long as every caller obeys this:
 *
 *  - DURING A STREAM: producer thread only, called synchronously from inside a
 *    sink's consume().  The producer commits the index before publishing and
 *    moves it again only on the next frame-ready, so the frame is stable for
 *    the whole call.
 *  - OTHERWISE: only after a successful camera_capture() / camera_capture_raw()
 *    with the producer idle -- which those calls guarantee, since they refuse
 *    while a stream runs and quiesce before returning.
 *  - EITHER WAY the pointer is valid for that call's duration only.  Do not
 *    store it.
 *
 * Those are exactly today's callers: the nn overlay's process() (on the
 * producer, inside consume()) and the command paths behind camera_capture().
 * A new caller from any other context is a data race, not a grey area.
 */
const uint8_t *camera_raw_frame(void);

/**
 * @brief  Start / stop continuous capture on the producer thread.
 *
 * camera_stream_start() returns once the first frame is on its way; it does not
 * wait for one.  @p sink may be NULL (`camera bench`, which measures the
 * datapath with nothing watching).
 *
 * [!] STARTING A STREAM AND SUBSCRIBING ITS SINK ARE ONE OPERATION (issue #63),
 * and this is the ONLY way a sink gets attached.  Both halves happen under the
 * API mutex, which is what makes the ownership rule checkable:
 *
 *     a stream may start only when the camera is stopped AND no sink is linked.
 *
 * Neither half alone is enough.  Without the first, a caller that attached and
 * then started could find a stream already running -- and CAM_ERR_BUSY is
 * precisely the failure it must not detach on (see the stop note below), so its
 * sink would be stranded, attached with no owner, until reboot.  Without the
 * second, a sink that is stopped but not yet unlinked -- the window its owner
 * holds between a confirmed stop and its own teardown, and the whole time the
 * producer has exited by itself while the owner is still asleep in its poll
 * loop -- would be delivered frames from somebody else's stream.
 *
 * So a linked sink is an OWNERSHIP RESERVATION: CAM_ERR_BUSY here also means
 * "somebody else's sink is still attached", and no new stream runs until that
 * owner unlinks.  The reservation ends at the UNLINK, not at the end of the
 * owner's teardown: once a sink is out of the registry no producer can reach it,
 * whatever its owner is still doing with its own threads.
 *
 * Every failure leaves NOTHING attached, which is the property callers are
 * entitled to rely on: unwind your own state and return, with no detach to
 * reason about.
 *

 * [!] camera_stream_stop() IS A SUCCESS-ONLY JOIN (issue #48).
 *
 * CAM_OK means the producer has been through the full stop sequence and is
 * idle -- and only then may a caller detach a sink, free anything a sink points
 * at, or release ownership of a device the sink was driving.
 *
 * ANY OTHER RETURN PROVES NOTHING, and CAM_ERR_TIMEOUT specifically means the
 * producer is still running somewhere.  It used to be described as
 * unconditionally synchronous, which was never quite true -- the wait is
 * bounded -- and became actively dangerous once a sink could run an NPU
 * inference inside consume().  On that path the camera enters an unrecoverable
 * state and refuses everything afterwards, so the caller's job is simply to
 * report it and hold on to whatever it owns: do NOT detach, do NOT tear down,
 * do NOT release.
 */
int camera_stream_start(struct frame_sink *sink);
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

	/*
	 * Per-stage profile of the producer loop, SINCE THE LAST STREAM START
	 * (issue #38) -- unlike the counters above, which are cumulative.  All
	 * microseconds, summed over `prof_iters` loop iterations.
	 *
	 * prof_total is measured loop-top to loop-top, so the stages sum to it
	 * by construction and prof_other is the remainder rather than an
	 * independent measurement.  Zero-length when prof_ok is 0: the time
	 * source is the EPK's TIMER2 and an untrustworthy one must say so rather
	 * than publish a plausible number.
	 */
	int      prof_ok;       /**< 0 = the time source is not trustworthy   */
	const char *prof_why;   /**< why not, when prof_ok is 0               */
	uint32_t prof_iters;
	uint32_t prof_total_us; /**< loop top to loop top                     */
	uint32_t prof_wait_us;  /**< asleep on frame-ready                    */
	uint32_t prof_inval_us; /**< D-cache invalidate, one landing buffer   */
	uint32_t prof_arm_us;   /**< arm the next capture + wrap reassert     */
	uint32_t prof_pack_us;  /**< planar B/G/R -> RGB565                   */
	uint32_t prof_sink_us;  /**< sinks consume (LCD blit + SPI DMA)       */
	uint32_t prof_tune_us;  /**< means, sensor read-back, white balance   */
	uint32_t prof_other_us; /**< total minus the six above                */

	/*
	 * The double-buffer evidence (issue #59).  buf_frames counts frames
	 * committed readable per landing buffer since the last configuration --
	 * equal-ish counts under a stream is what says the alternation is
	 * real, since a no-op flip produces a working picture at the old frame
	 * rate.  premature_disables is CUMULATIVE and counts the
	 * disable-before-finish statuses the arm had to acknowledge; the mask
	 * around the arm's disable exists to keep it zero, and acceptance
	 * requires that.
	 */
	uint32_t buf_frames[2];
	uint32_t premature_disables;
};
void camera_stream_stats(struct camera_stats *out);

/**
 * @brief  Software white balance applied to every published frame.
 *
 * Unity by default, so an unbalanced preview shows what the sensor produced --
 * which is what keeps `camera capture`'s channel statistics meaning something.
 * The sensor has no per-channel gain registers, so a colour cast cannot be
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
 *
 * [!] A SUCCESSFUL CALL TURNS camera_auto() OFF (issue #39).  Writing an
 * exposure by hand is what taking manual control means, and the sensor's own
 * AEC/AGC is already switched to manual by the write itself -- leaving the flag
 * set would report an auto mode that is not running.  Note this also freezes
 * the software white balance, which shares the flag; camera_set_auto(1) hands
 * both back.  A refused write changes nothing.
 */
int camera_set_exposure(uint16_t lines);
int camera_set_gains(uint8_t again, uint16_t dgain);

/**
 * @brief  Set / read the sensor's frame length (VTS), in lines.
 *
 * [!] One register, two effects (issue #38): the frame period is
 * VTS * HTS / PCLK, and integration time cannot exceed the frame -- so a faster
 * frame rate costs the longest exposure available.
 *
 * Unlike the exposure setters above this does NOT turn camera_auto() off: VTS
 * is the frame the on-chip AEC works inside, not a manual exposure, and the AEC
 * keeps adapting within the new ceiling.
 *
 * camera_read_frame_length() reads the SENSOR, not this port's shadow, and
 * returns CAM_ERR_BUSY while a stream runs (it is I2C, which the producer
 * owns).  Reading the part rather than the shadow is what found #38.
 */
int camera_set_frame_length(uint16_t lines);
int camera_read_frame_length(uint16_t *lines);

/**
 * @brief  Auto exposure and auto white balance, on by default.
 *
 * The datapath provides neither, so a fixed setting is correct only for the
 * lighting it was tuned in.  "Auto exposure" here means the SENSOR's own
 * AEC/AGC (the port's software loop went with the IMX219 in issue #54); the
 * white balance is this port's, in software, on the packed frame.
 *
 * Turn them OFF before taking measurements that assume the sensor is holding
 * still -- comparing Bayer phases, or reading `camera capture` statistics twice
 * -- since a loop adjusting the exposure between two captures is a variable
 * nobody asked for.
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
 * @brief  Detach a frame sink (svc/frame_pipeline.h contract).
 *
 * There is no matching subscribe: attaching happens inside
 * camera_stream_start(), because "the camera is stopped" and "subscribe" have to
 * be one indivisible step (issue #63 -- the sentence that used to stand here
 * said sinks may be attached before or during a stream, and that is exactly what
 * let a sink be attached to somebody else's stream).
 *
 * The asymmetry is deliberate rather than an oversight: a sink must OUTLIVE the
 * stream it served.  Unlinking is only safe once the producer is confirmed idle
 * (camera_stream_stop() == CAM_OK), because publish() pre-pins a sink and calls
 * consume() with the pipeline lock released, so this stays a separate step the
 * owner takes when it is ready.  Until it does, the sink reserves the camera:
 * no new stream can start.
 *
 * REFUSED (CAM_ERR_STATE) after a producer that never acknowledged a stop --
 * see the note on camera_stream_stop().  A refusal means the sink is still
 * linked and must stay owned.
 */
int camera_unsubscribe(struct frame_sink *sink);

/** Balance one push delivery.  Sinks call this exactly once per consume(). */
void camera_frame_put(struct frame_sink *sink, const struct frame_desc *f);

#ifdef __cplusplus
}
#endif

#endif /* CAMERA_H */
