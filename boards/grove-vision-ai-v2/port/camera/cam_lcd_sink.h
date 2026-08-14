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
 * @brief  Start / stop showing published frames on the panel.
 *
 * attach() brings the panel up if needed, rotates it to landscape (the camera
 * delivers 320x240 and the panel is natively 240x320 -- MADCTL transposes for
 * free on this SPI panel, so no CPU-side rotation is involved), and subscribes
 * to the pipeline.  detach() unsubscribes.
 *
 * The caller must stop the stream before detaching if it needs delivery to have
 * ceased: consume() runs on the producer thread.  camera_stream_stop() is
 * synchronous precisely so that this ordering is available.
 *
 * @return CAM_OK, or a CAM_* error.
 */
int cam_lcd_sink_attach(void);
int cam_lcd_sink_detach(void);

/** Delivery counters for `camera stats`. */
struct cam_lcd_sink_stats {
	uint32_t delivered;
	uint32_t dropped;
	uint32_t errors;
	uint32_t busy;      /**< frames skipped because the panel was taken */
};
void cam_lcd_sink_stats(struct cam_lcd_sink_stats *out);

#ifdef __cplusplus
}
#endif

#endif /* CAM_LCD_SINK_H */
