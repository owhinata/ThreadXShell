/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    camera_names.c
 * @brief   This board's camera vocabulary: the setting / mode names, the
 *          CAM_ERR_* sentences and the `camera set` lines (issue #143).
 *
 * Moved verbatim from cmds/cmd_camera.c when the shared `camera` command split
 * the file in two; see camera_names.h for why it lives in the port.
 *
 * Clean-room design; no third-party code reused.
 */
#include "camera_names.h"

#include <string.h>

const char *camera_strerror(int rc)
{
	switch (rc) {
	case CAM_ERR_PARAM:     return "bad argument";
	case CAM_ERR_HAL:       return "I2C/sensor I/O failed";
	case CAM_ERR_TIMEOUT:   return "operation timed out";
	case CAM_ERR_STATE:     return "driver not initialized / SDRAM down";
	case CAM_ERR_NO_SENSOR: return "OV5640 not detected (module connected?)";
	case CAM_ERR_NO_FRAME:  return "no captured frame";
	case CAM_ERR_BUSY:      return "busy (stream, preview or sdram test active)";
	case CAM_ERR_IN_USE:    return "still in use";
	case CAM_ERR_FORMAT:    return "not an RGB565 stream";
	case CAM_ERR_TEST:      return "the stream is running with a different test setting";
	default:                return "unknown error";
	}
}

/* ---- `camera set` quality controls (owhinata/stm32f746g-disco#44) ------------- */

const struct camera_named camera_awb_names[] = {
	{ "auto", CAM_AWB_AUTO }, { "sunny", CAM_AWB_SUNNY },
	{ "office", CAM_AWB_OFFICE }, { "home", CAM_AWB_HOME },
	{ "cloudy", CAM_AWB_CLOUDY }, { NULL, 0 }
};
const struct camera_named camera_effect_names[] = {
	{ "none", CAM_FX_NONE }, { "bw", CAM_FX_BW }, { "sepia", CAM_FX_SEPIA },
	{ "negative", CAM_FX_NEGATIVE }, { "blue", CAM_FX_BLUE },
	{ "red", CAM_FX_RED }, { "green", CAM_FX_GREEN }, { NULL, 0 }
};
const struct camera_named camera_flip_names[] = {
	{ "none", CAM_FLIP_NONE }, { "mirror", CAM_FLIP_MIRROR },
	{ "flip", CAM_FLIP_FLIP }, { "both", CAM_FLIP_BOTH }, { NULL, 0 }
};

/* Resolution / pixel-format names for `camera res` / `camera format`
   (owhinata/stm32f746g-disco#45). */
const struct camera_named camera_res_names[] = {
	{ "qqvga", CAM_RES_QQVGA }, { "qvga", CAM_RES_QVGA },
	{ "vga", CAM_RES_VGA }, { "wvga", CAM_RES_WVGA }, { NULL, 0 }
};
const struct camera_named camera_format_names[] = {
	{ "rgb565", CAM_FMT_RGB565 }, { "yuv422", CAM_FMT_YUV422 },
	{ "y8", CAM_FMT_Y8 }, { "jpeg", CAM_FMT_JPEG }, { NULL, 0 }
};

int camera_named_lookup(const struct camera_named *t, const char *s, int *out)
{
	for (; t->name != NULL; t++) {
		if (strcmp(t->name, s) == 0) {
			*out = t->val;
			return 0;
		}
	}
	return -1;
}

const char *camera_named_name(const struct camera_named *t, int v)
{
	for (; t->name != NULL; t++)
		if (t->val == v)
			return t->name;
	return "?";
}

void camera_settings_write(struct camera_out *o, const struct camera_settings *s)
{
	camera_out(o, "brightness : %d\r\n", s->brightness);
	camera_out(o, "contrast : %d\r\n", s->contrast);
	camera_out(o, "saturation : %d\r\n", s->saturation);
	camera_out(o, "hue      : %d deg\r\n", s->hue * 30);
	camera_out(o, "awb      : %s\r\n", camera_named_name(camera_awb_names, s->awb));
	camera_out(o, "effect   : %s\r\n",
	           camera_named_name(camera_effect_names, s->effect));
	camera_out(o, "flip     : %s\r\n",
	           camera_named_name(camera_flip_names, s->flip));
	camera_out(o, "zoom     : x%u\r\n", (unsigned)s->zoom);
	camera_out(o, "night    : %s\r\n", s->night ? "on" : "off");
}
