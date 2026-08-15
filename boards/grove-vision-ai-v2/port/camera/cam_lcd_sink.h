/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    cam_lcd_sink.h
 * @brief   Frame sink that puts camera frames on the ST7789 panel (issue #35).
 *
 * The glue between two drivers, so it lives with neither: camera.c stays free
 * of the panel and lcd_st7789.c stays free of the camera.
 */
#ifndef CAM_LCD_SINK_H
#define CAM_LCD_SINK_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief  Optional per-frame overlay (issue #48).
 *
 * Two callbacks because the work splits at the panel guard, and that split is
 * the point:
 *
 *   - process() runs FIRST, with NO guard held, and may take as long as it
 *     needs -- `nn preview` runs a whole NPU inference here.  Holding the panel
 *     across that would block every other `lcd` command for the duration and
 *     entangle the stop path with the transfer.
 *   - draw() runs INSIDE the guard, on the staged frame, immediately before the
 *     DMA.  It is handed the framebuffer and its geometry and is bound by the
 *     contract in lcd_st7789.h -- no blocking, no re-entry, only
 *     lcd_rect_wire().
 *
 * Both run ON THE PRODUCER THREAD.  A non-zero return from process() means "no
 * overlay for this frame": draw() is skipped, the frame is still shown, and the
 * preview keeps running.  A failed inference must not blank the picture.
 */
struct cam_lcd_overlay {
	void *ctx;
	int  (*process)(void *ctx, const void *pixels, uint16_t w, uint16_t h);
	void (*draw)(void *ctx, uint16_t *fb, uint16_t fb_w, uint16_t fb_h);
};

/**
 * @brief  Start / stop showing published frames on the panel.
 *
 * attach() brings the panel up if needed, rotates it to landscape (the camera
 * delivers 320x240 and the panel is natively 240x320 -- MADCTL transposes for
 * free on this SPI panel, so no CPU-side rotation is involved), and subscribes
 * to the pipeline.  detach() unsubscribes.
 *
 * @p ov may be NULL for a plain preview.  It is an ARGUMENT rather than a
 * separate setter so that an overlay cannot outlive the command that wanted it:
 * there is no state a crashed or forgotten teardown could leave behind for the
 * next `camera preview` to inherit.  The pointer must stay valid until
 * detach() returns.
 *
 * [!] The caller must have a CONFIRMED stop before detaching -- consume() runs
 * on the producer thread, and camera_stream_stop() only proves the producer is
 * idle when it returns CAM_OK (issue #48).  A timeout proves nothing, and
 * detaching after one is what this sink cannot survive.
 *
 * @return CAM_OK, or a CAM_* error.
 */
int cam_lcd_sink_attach(const struct cam_lcd_overlay *ov);
int cam_lcd_sink_detach(void);

/** Delivery counters for `camera stats`. */
struct cam_lcd_sink_stats {
	uint32_t delivered;
	uint32_t dropped;
	uint32_t errors;
	uint32_t busy;           /**< frames skipped because the panel was taken */
	uint32_t overlay_errors; /**< process() refused: frame shown, no boxes   */
};
void cam_lcd_sink_stats(struct cam_lcd_sink_stats *out);

#ifdef __cplusplus
}
#endif

#endif /* CAM_LCD_SINK_H */
