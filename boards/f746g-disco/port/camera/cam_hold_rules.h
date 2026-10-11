/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    cam_hold_rules.h
 * @brief   The two decisions this board makes around the camera's holder table
 *          (issue #142): whether a teardown still goes ahead, and whether nn may
 *          join the stream.
 *
 * The table itself is svc/cam_holders.h.  What is here is this board's: its
 * stop is ASYNCHRONOUS (camera_stream_stop() asks, the producer tears down
 * later), and its base can run formats the nn step cannot read.  Both are pure
 * so test/test_cam_hold_rules.c can walk them; camera.c calls them under
 * cam_lock and acts on the answer in the same locked region.
 */
#ifndef CAM_HOLD_RULES_H
#define CAM_HOLD_RULES_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * The producer is about to tear the stream down, and has taken cam_lock.  Does
 * it?
 *
 * @param err       a stream error asked for it (overrun gave up, DMA TE, ...)
 * @param stop_req  the last holder's leaving asked for it
 * @param holders   holders registered NOW
 *
 * [!] AN ERROR IS NEVER CANCELLED: the producer has seen the hardware fail.
 * [!] A STOP REQUEST IS CANCELLED WHEN A HOLDER CAME BACK before the producer
 * got here (a `nn stream start` straight after the operator's stop), and when
 * nothing asks for it any more (a start already withdrew it).  Only the stop
 * request the leaving holder made is withdrawn -- never the teardown's other
 * reasons.
 *
 * @return non-zero to tear down, 0 to keep streaming
 */
int cam_teardown_proceeds(int err, int stop_req, unsigned holders);

/**
 * May the nn step join the camera now (issue #142 D3)?  It reads RGB565 only,
 * and the mode must be one that streams -- whether the base is running, about
 * to be started by this join, or between an overrun and its recovery (review
 * F3: accepting a capture-only RGB565 mode there would only lose the holders
 * at the re-arm).  A running raster mode streams by definition.
 *
 * @param rgb565      the current mode's format is RGB565
 * @param streamable  the current mode can be streamed
 */
int cam_join_format_ok(int rgb565, int streamable);

#ifdef __cplusplus
}
#endif

#endif /* CAM_HOLD_RULES_H */
