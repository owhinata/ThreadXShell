/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * The panel-less frame sink `nn run` streams through (issue #129).  See
 * cam_capture_sink.h for the contract.
 */
#include <stddef.h>
#include <stdint.h>

#include "tx_api.h"

#include "cam_capture_sink.h"
#include "camera.h"
#include "cam_state.h"
#include "frame.h"
#include "frame_pipeline.h"

#define LOG_TAG "camcap"
#include "log.h"

enum cap_state {
	CAP_DETACHED = 0,
	CAP_ATTACHING,
	CAP_ATTACHED,
	CAP_DETACHING,
	CAP_LOST,          /* refused for good: still linked, never released */
};

static uint8_t cap_state_v;

/* The hook, published before the subscribe (so the producer never sees a half-
 * written one) and cleared only by a detach that proved the sink idle. */
static struct cam_lcd_overlay cap_ov;
static uint8_t cap_has_ov;

/* Frames consume() accepted and handed back.  Both on the producer, but read
 * together by the detach, so in critical sections. */
static uint32_t cap_accepted;
static uint32_t cap_puts;

static int cap_claim(uint8_t from, uint8_t to)
{
	TX_INTERRUPT_SAVE_AREA
	int ok = 0;

	TX_DISABLE
	if (cap_state_v == from) {
		cap_state_v = to;
		ok = 1;
	}
	TX_RESTORE
	return ok;
}

static void cap_set(uint8_t to)
{
	TX_INTERRUPT_SAVE_AREA

	TX_DISABLE
	cap_state_v = to;
	TX_RESTORE
}

static uint8_t cap_get(void)
{
	TX_INTERRUPT_SAVE_AREA
	uint8_t s;

	TX_DISABLE
	s = cap_state_v;
	TX_RESTORE
	return s;
}

static int cap_open(void *ctx, enum frame_format fmt, uint16_t w, uint16_t h)
{
	(void)ctx;
	/* The model reads camera_raw_frame(), not this slot -- but a producer that
	 * changed what it publishes is a change to look at, not to adapt to. */
	if (fmt != FRAME_FMT_RGB565 || w != (uint16_t)CAM_FRAME_WIDTH ||
	    h != (uint16_t)CAM_FRAME_HEIGHT) {
		LOG_ERR("capture sink refused format %d %ux%u", (int)fmt, w, h);
		return -1;
	}
	return 0;
}

static struct frame_sink cap_sink;

/* On the PRODUCER, with the slot pre-pinned once.  Synchronous: the hook, then
 * the one put, then return -- nothing of the frame is touched after the put. */
static int cap_consume(void *ctx, const struct frame_desc *f)
{
	TX_INTERRUPT_SAVE_AREA

	(void)ctx;
	TX_DISABLE
	cap_accepted++;
	TX_RESTORE
	if (cap_has_ov && cap_ov.process != NULL)
		(void)cap_ov.process(cap_ov.ctx, f->data,
		                     (uint16_t)CAM_FRAME_WIDTH,
		                     (uint16_t)CAM_FRAME_HEIGHT);
	/* Counted before the put: nothing of the delivery is touched after it. */
	TX_DISABLE
	cap_puts++;
	TX_RESTORE
	camera_frame_put(&cap_sink, f);
	return 0;
}

/* Deliberately empty, as cam_lcd_sink.c's: the teardown that knows whether it
 * succeeded is cam_capture_sink_detach(). */
static void cap_close(void *ctx)
{
	(void)ctx;
}

static struct frame_sink cap_sink = {
	.name    = "capture",
	.ctx     = NULL,
	.policy  = FRAME_POLICY_DROP,
	.open    = cap_open,
	.consume = cap_consume,
	.close   = cap_close,
};

int cam_capture_sink_attach_and_stream(const struct cam_lcd_overlay *ov)
{
	TX_INTERRUPT_SAVE_AREA
	int rc;

	if (!cap_claim(CAP_DETACHED, CAP_ATTACHING))
		return (cap_get() == CAP_LOST) ? CAM_ERR_STATE : CAM_ERR_BUSY;

	/* Nothing is linked yet, so nothing else writes these. */
	TX_DISABLE
	cap_accepted = 0u;
	cap_puts     = 0u;
	TX_RESTORE
	if (ov != NULL) {
		cap_ov     = *ov;
		cap_has_ov = 1u;
	} else {
		cap_has_ov = 0u;
	}

	/* [!] THE ONE WAY IN: the subscribe and the stream start are one
	 * operation under the camera's API mutex (issue #63).  A failure here
	 * linked nothing. */
	rc = camera_stream_start(&cap_sink);
	if (rc != CAM_OK) {
		cap_has_ov = 0u;
		cap_set(CAP_DETACHED);
		return rc;
	}
	cap_set(CAP_ATTACHED);
	return CAM_OK;
}

int cam_capture_sink_detach(void)
{
	TX_INTERRUPT_SAVE_AREA
	uint32_t acc, puts;
	int rc;

	switch (cap_get()) {
	case CAP_DETACHED:
		return CAM_OK;
	case CAP_LOST:
		return CAM_ERR_STATE;
	default:
		break;
	}
	if (!cap_claim(CAP_ATTACHED, CAP_DETACHING))
		return CAM_ERR_BUSY;

	/* Unlink first: nothing can select the sink after this. */
	rc = camera_unsubscribe(&cap_sink);
	if (rc == CAM_ERR_BUSY) {
		/* Retryable (issue #79): put it back so the next stop asks again. */
		cap_set(CAP_ATTACHED);
		return CAM_ERR_BUSY;
	}
	if (rc != CAM_OK) {
		LOG_ERR("the camera refused to unsubscribe the capture sink (%d)",
		        rc);
		cap_set(CAP_LOST);
		return CAM_ERR_STATE;
	}

	/* consume() is synchronous, so there is no thread to drain: unlinked
	 * after a confirmed stop, every frame it accepted has been put.  Asked
	 * anyway, in both bookkeepings, because a sink that holds a slot detaches
	 * cleanly and only shows up as the next stream finding none. */
	TX_DISABLE
	acc  = cap_accepted;
	puts = cap_puts;
	TX_RESTORE
	if (puts != acc ||
	    cam_drain_decide(CAM_OK, camera_sink_pins(&cap_sink)) !=
	    CAM_DRAIN_DONE) {
		LOG_ERR("the capture sink still holds a pipeline slot (%lu/%lu)",
		        (unsigned long)puts, (unsigned long)acc);
		cap_set(CAP_LOST);
		return CAM_ERR_STATE;
	}

	cap_has_ov = 0u;
	cap_set(CAP_DETACHED);
	return CAM_OK;
}
