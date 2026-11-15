/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Host stand-in for the one Himax SDK type port/camera/cam_sensor.h names
 * (issue #237, test_camera_svc_grove.c).  Not the SDK's layout: nothing on
 * the host reads a sensor table.
 */
#ifndef HX_DRV_CIS_COMMON_H
#define HX_DRV_CIS_COMMON_H

#include <stdint.h>

typedef struct {
	uint16_t addr;
	uint8_t  val;
} HX_CIS_SensorSetting_t;

#endif /* HX_DRV_CIS_COMMON_H */
