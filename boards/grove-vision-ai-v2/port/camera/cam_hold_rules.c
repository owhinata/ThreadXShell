/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    cam_hold_rules.c
 * @brief   The holder-table decisions (issue #142).  See the header.
 */
#include "cam_hold_rules.h"

int cam_hold_may_detach(int cam_rc)
{
	/* Exactly one value, enumerated rather than excluded: LOCKED is "the
	 * producer was never asked", and unlinking a sink it may be inside is
	 * what the camera's lost-producer state exists to prevent. */
	return cam_rc == CAM_HOLD_CAM_OK;
}

enum cam_hold_end cam_hold_end_of(int cam_rc, int detach_attempted,
                                  int detach_rc)
{
	if (cam_rc != CAM_HOLD_CAM_OK) {
		/* Nothing was asked, nothing was touched: the same stop can ask
		 * again.  Everything else is a producer that may still be out
		 * there, or a code that is no evidence it is not. */
		return (cam_rc == CAM_HOLD_CAM_LOCKED) ? CAM_HOLD_END_RETRY
		                                       : CAM_HOLD_END_TERMINAL;
	}
	if (!detach_attempted)
		return CAM_HOLD_END_TERMINAL;   /* a caller that skipped the unlink */
	switch (detach_rc) {
	case CAM_HOLD_CAM_OK:
		return CAM_HOLD_END_DONE;
	case CAM_HOLD_CAM_BUSY:
		/* The unlink put the sink back and asks to be called again (issue
		 * #79): the camera is stopped, so a later stop finishes it. */
		return CAM_HOLD_END_RETRY;
	default:
		return CAM_HOLD_END_TERMINAL;
	}
}

enum cam_hold_settle cam_hold_settle_of(int linked, int streaming,
                                        int stopping)
{
	if (stopping)
		return CAM_HOLD_SETTLE_STOP;
	if (linked && !streaming)
		return CAM_HOLD_SETTLE_ERROR;
	return CAM_HOLD_SETTLE_NONE;
}
