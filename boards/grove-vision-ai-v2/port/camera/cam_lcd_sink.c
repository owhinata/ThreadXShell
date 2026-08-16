/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Frame sink that puts camera frames on the ST7789 panel (issue #35).
 */
#include <stdint.h>

#include "cam_lcd_sink.h"
#include "camera.h"
#include "cam_dp.h"
#include "frame.h"
#include "frame_pipeline.h"
#include "lcd_st7789.h"

#define LOG_TAG "camlcd"
#include "log.h"

static struct cam_lcd_sink_stats sink_stats;
static int sink_attached;

/*
 * The optional overlay (issue #48).
 *
 * Set by attach() and cleared by detach(), so it cannot outlive the command
 * that asked for it.  Read on the producer thread inside consume(); written on
 * a shell thread only while no stream is running, which is what attach()'s
 * exclusivity guarantees.
 */
static struct cam_lcd_overlay sink_overlay;
static int sink_has_overlay;

/* Defined below; consume() has to name it when balancing its delivery. */
static struct frame_sink cam_lcd_sink;

static int cam_lcd_open(void *ctx, enum frame_format fmt, uint16_t w,
                        uint16_t h)
{
	(void)ctx;

	/*
	 * Reject rather than adapt.  A sink that quietly coped with an
	 * unexpected format would turn a producer change into a picture that is
	 * subtly wrong, which is the failure this whole path is worst at
	 * diagnosing.
	 */
	if (fmt != FRAME_FMT_RGB565) {
		LOG_ERR("sink refused format %d (wants RGB565)", (int)fmt);
		return -1;
	}
	if (w != (uint16_t)CAM_FRAME_WIDTH || h != (uint16_t)CAM_FRAME_HEIGHT) {
		LOG_ERR("sink refused %ux%u (wants %ux%u)", w, h,
		        (unsigned)CAM_FRAME_WIDTH, (unsigned)CAM_FRAME_HEIGHT);
		return -1;
	}
	if (w > lcd_width() || h > lcd_height()) {
		LOG_ERR("the panel is %ux%u; rotate it to landscape first",
		        lcd_width(), lcd_height());
		return -1;
	}
	return 0;
}

/*
 * Called on the PRODUCER thread, with the slot pre-pinned once.
 *
 * The contract is that exactly one frame_pipeline_put() balances that pin, on
 * every path including the error ones -- so there is exactly one return
 * statement's worth of cleanup here, reached from everywhere.  Getting that
 * wrong leaks a slot per failure and the ring stops handing any out.
 */
/* Trampoline: the LCD driver's callback signature is its own, and the sink's
 * overlay contract is the sink's.  Only reached with sink_has_overlay set. */
static void cam_lcd_draw(void *ctx, uint16_t *fb, uint16_t fb_w, uint16_t fb_h)
{
	(void)ctx;
	sink_overlay.draw(sink_overlay.ctx, fb, fb_w, fb_h);
}

static int cam_lcd_consume(void *ctx, const struct frame_desc *f)
{
	int rc = 0;
	int overlay_ok = 0;

	(void)ctx;

	/*
	 * [!] BEFORE THE GUARD, and that ordering is the design (issue #48).
	 *
	 * This is where a whole NPU inference happens under `nn preview`.  It
	 * runs with the panel released so that `lcd` commands on other threads
	 * keep working, and so that a stop landing here is not waiting on the
	 * panel as well as on the NPU.  A failure means no boxes on this frame,
	 * not a blank preview -- the picture is worth more than the annotation.
	 */
	if (sink_has_overlay && sink_overlay.process != NULL) {
		if (sink_overlay.process(sink_overlay.ctx, f->data,
		                         (uint16_t)CAM_FRAME_WIDTH,
		                         (uint16_t)CAM_FRAME_HEIGHT) == 0)
			overlay_ok = 1;
		else
			sink_stats.overlay_errors++;
	}

	/*
	 * TX_NO_WAIT, and a skipped frame if the panel is taken.  Blocking here
	 * would stall the producer -- and therefore the datapath's single WDMA3
	 * buffer -- behind whatever `lcd fill` a user happened to type.  A live
	 * preview that drops a frame is better than one that stutters the
	 * capture, and the count says it happened.
	 */
	if (lcd_acquire() != 0) {
		sink_stats.busy++;
		rc = -1;
	} else {
		/* lcd_blit_le_overlay: the slot is little-endian RGB565 and the
		 * panel wants wire order.  The driver owns that swap, and calls
		 * the draw hook between the swap and the DMA. */
		int blit = lcd_blit_le_overlay(
			0u, 0u, (uint16_t)CAM_FRAME_WIDTH,
			(uint16_t)CAM_FRAME_HEIGHT,
			(const uint16_t *)f->data,
			(overlay_ok && sink_overlay.draw != NULL)
				? cam_lcd_draw : NULL,
			NULL, NULL, NULL);
		lcd_release();

		if (blit == 0) {
			sink_stats.delivered++;
		} else {
			sink_stats.errors++;
			rc = -1;
		}
	}

	/*
	 * THE single put, on every path.  Nothing above may return early.
	 *
	 * [!] It must name THIS sink.  frame_pipeline_put() clears the sink's
	 * busy flag only when it is given the owning sink -- a NULL there
	 * releases the slot's reference and nothing else, so the sink stays
	 * marked busy for ever and its DROP policy discards every frame after
	 * the first.  A preview that shows exactly one frame and then a frozen
	 * panel, with the producer's own counters climbing normally, is what
	 * that looks like.  (NULL is for releasing a frame_pipeline_pin_latest()
	 * pin, which has no sink.)
	 */
	camera_frame_put(&cam_lcd_sink, f);
	return rc;
}

static void cam_lcd_close(void *ctx)
{
	(void)ctx;
	/* Non-blocking by contract: this runs from inside detach(). */
	sink_attached = 0;
}

static struct frame_sink cam_lcd_sink = {
	.name    = "lcd",
	.ctx     = NULL,
	.policy  = FRAME_POLICY_DROP,
	.open    = cam_lcd_open,
	.consume = cam_lcd_consume,
	.close   = cam_lcd_close,
};

int cam_lcd_sink_attach(const struct cam_lcd_overlay *ov)
{
	int rc;

	/*
	 * [!] BUSY, not "already done".  The shell runs commands as background
	 * jobs, so a second `camera preview` is a real second caller -- and if
	 * it were told the attach succeeded it would go on to fail at
	 * camera_stream_start() (the stream really is busy) and then detach on
	 * its way out, taking the FIRST preview's sink with it.  The first
	 * preview would keep capturing while the panel silently stopped
	 * updating, which is a hard failure to attribute to a command that
	 * appeared to do nothing.
	 */
	if (sink_attached)
		return CAM_ERR_BUSY;

	if (!lcd_ready()) {
		if (lcd_init() != 0) {
			LOG_ERR("panel bring-up failed");
			return CAM_ERR_HAL;
		}
	}

	/*
	 * Landscape.  The camera delivers 320x240 and the panel is natively
	 * 240x320; issue #31 established by measurement that MADCTL really
	 * rotates THIS panel over 4-wire SPI (unlike the Wio's RGB-parallel
	 * one), so the controller transposes for free and no CPU-side rotation
	 * -- svc/gfx_rot and its 150 KB round trip -- is involved.
	 */
	if (lcd_width() < (uint16_t)CAM_FRAME_WIDTH ||
	    lcd_height() < (uint16_t)CAM_FRAME_HEIGHT) {
		if (lcd_set_rotation(90u) != 0) {
			LOG_ERR("could not rotate the panel to landscape");
			return CAM_ERR_HAL;
		}
	}

	sink_stats.delivered      = 0u;
	sink_stats.dropped        = 0u;
	sink_stats.errors         = 0u;
	sink_stats.busy           = 0u;
	sink_stats.overlay_errors = 0u;

	/* Published before the subscribe, so the producer can never see a sink
	 * that is attached with a half-written overlay. */
	if (ov != NULL) {
		sink_overlay     = *ov;
		sink_has_overlay = 1;
	} else {
		sink_has_overlay = 0;
	}

	rc = camera_subscribe(&cam_lcd_sink);
	if (rc != CAM_OK) {
		sink_has_overlay = 0;
		return rc;
	}

	sink_attached = 1;
	return CAM_OK;
}

int cam_lcd_sink_detach(void)
{
	int rc;

	if (!sink_attached)
		return CAM_OK;
	rc = camera_unsubscribe(&cam_lcd_sink);
	/*
	 * [!] ONLY ON SUCCESS.  A refused unsubscribe -- which is what a lost
	 * producer gets (CAM_ST_LOST) -- means the sink is STILL ATTACHED and a
	 * consume() may still be running in it.  Clearing the overlay there
	 * would mutate state that a live producer reads, which is the exact race
	 * the refusal exists to prevent: the backstop would have become the
	 * hazard.  Leaving it set costs nothing, because the camera never
	 * delivers another frame without a reboot.
	 */
	if (rc == CAM_OK)
		sink_has_overlay = 0;
	return rc;
}

void cam_lcd_sink_stats(struct cam_lcd_sink_stats *out)
{
	if (out == NULL)
		return;
	*out = sink_stats;
	/* The pipeline counts drops (policy DROP, sink busy with a previous
	 * frame); the sink counts what it did with the ones it got.  Reading
	 * the drop count from the ring rather than keeping a second one here is
	 * what stops the two disagreeing. */
	out->dropped = cam_lcd_sink.dropped;
}
