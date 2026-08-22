/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    nx_mjpeg.h
 * @brief   MJPEG-over-HTTP camera streaming server (owhinata/stm32f746g-disco#49 P5).
 *
 * Streams the OV5640 hardware-JPEG frames to a browser as
 * `multipart/x-mixed-replace` over a NetX TCP server socket (port 80) -- the
 * "eth_sink" consumer of the camera frame pipeline (owhinata/stm32f746g-disco#46/#47)
 * and the headline network sink for the camera (the 30 fps JPEG path's real consumer).
 *
 * Since Epic owhinata/stm32f746g-disco#99 Phase 2 (owhinata/stm32f746g-disco#101)
 * MJPEG is a pure JPEG-class *subscriber*: it does NOT own or start the base capture.
 * Run `camera format jpeg` + `camera stream start` first, then `net mjpeg start`
 * attaches to the running JPEG base and listens on :80; point a browser at
 * http://<board-ip>/ for the live stream.  `net mjpeg stop` detaches the sink (the
 * base keeps running). A `camera stream stop` (cascade) auto-stops the server; a
 * transient DCMI overrun pauses and resumes it.  One client at a time (N=1).
 */
#ifndef NX_MJPEG_H
#define NX_MJPEG_H

#include <stdbool.h>
#include <stdint.h>

#include "camera.h"          /* enum camera_res */

#ifdef __cplusplus
extern "C" {
#endif

/* nx_mjpeg_start() format-arbitration codes (owhinata/stm32f746g-disco#101).  Distinct
   from the -1..-3
   transport codes and the CAM_ERR_* range (-1..-7). */
#define NX_MJPEG_NO_CAPTURE  (-10)  /* base not streaming: run `camera stream start` */
#define NX_MJPEG_FMT_CLASH   (-11)  /* base is raster (RGB565): mjpeg needs JPEG     */
/* Teardown codes (issue #72), in the same range for the same reason. */
#define NX_MJPEG_BUSY        (-12)  /* a start or another stop owns the lifecycle   */
#define NX_MJPEG_PINS        (-13)  /* the sink did not hand its frame back         */

/** Snapshot for `net mjpeg stats`. */
struct nx_mjpeg_stats {
	bool     running;        /**< nx_mjpeg_start() active                          */
	bool     client;         /**< a browser is currently connected                 */
	uint8_t  res;            /**< enum camera_res of the stream                     */
	uint32_t conns;          /**< clients accepted since start                      */
	uint32_t sent_frames;    /**< JPEG frames delivered                            */
	uint32_t sent_bytes;     /**< JPEG payload bytes delivered (wraps at 4 GB)      */
	uint32_t drop_busy;      /**< dropped: HTTP still sending the previous frame    */
	uint32_t drop_oversized; /**< dropped: frame larger than the copy buffer        */
	uint32_t send_err;       /**< TCP send failures (window/disconnect)             */
	uint32_t pool_fail;      /**< packet pool allocation failures                   */
};

/**
 * Attach the MJPEG sink to the running JPEG base capture and listen on :80
 * (owhinata/stm32f746g-disco#101).  The base must already be streaming JPEG (`camera
 * format jpeg` + `camera stream start`); the stream resolution follows the base.
 * Returns 0, or a negative:  NX_MJPEG_NO_CAPTURE (base off), NX_MJPEG_FMT_CLASH (base
 * is raster, not JPEG), -1 (net down), -2 (already running / a teardown owns the
 * sink), -3 (listen / thread failed), NX_MJPEG_PINS (the start failed AND its
 * unwind could not get the sink back -- retry `net mjpeg stop`, issue #72), or a
 * CAM_ERR_* from camera_subscribe_oneshot (e.g. CAM_ERR_STATE registry full).
 */
int  nx_mjpeg_start(void);

/** Detach the MJPEG sink (the base keeps running), wait for the producer to hand
 *  the sink's frame back, then for the server thread to park.  Returns:
 *    0                 stopped;
 *   -1                 not running;
 *   -2                 the server thread has not parked yet (the sink IS
 *                      released, so this is not a refusal -- it parks by itself);
 *   NX_MJPEG_PINS      the sink did not hand its frame back (issue #72): a
 *                      producer callback may still be copying into mjpeg_buf, so
 *                      `net mjpeg start` is refused until a later stop re-polls
 *                      and finds it clear.  Retryable by design;
 *   NX_MJPEG_BUSY      a start or another stop owns the lifecycle -- nothing done. */
int  nx_mjpeg_stop(void);

/** Fill @p out (may be NULL) and return true while the server is running. */
bool nx_mjpeg_stats_get(struct nx_mjpeg_stats *out);

#ifdef __cplusplus
}
#endif

#endif /* NX_MJPEG_H */
