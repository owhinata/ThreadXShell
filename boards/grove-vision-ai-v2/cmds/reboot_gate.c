/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    reboot_gate.c
 * @brief   The reboot's stop table (issue #68).  See the header.
 */
#include "reboot_gate.h"

#include <stddef.h>

enum reboot_verdict reboot_verdict_of(int stop_rc)
{
	switch (stop_rc) {
	case REBOOT_CAM_OK:
		/* Joined, or nothing was streaming: either way the producer
		 * has been through the quiesce and the datapath is down. */
		return REBOOT_RESET;
	case REBOOT_CAM_LOCKED:
		/* Nothing was asked of the producer: the stream may still be
		 * running, which is exactly the hardware state #68 is about. */
		return REBOOT_REFUSE_LOCKED;
	case REBOOT_CAM_TIMEOUT:
		return REBOOT_REFUSE_LOST;
	case REBOOT_CAM_STATE:
		return REBOOT_REFUSE_STATE;
	default:
		/* [!] Unknown codes are terminal: no evidence of a stop. */
		return REBOOT_REFUSE_UNKNOWN;
	}
}

const char *reboot_refusal_text(enum reboot_verdict v)
{
	switch (v) {
	case REBOOT_RESET:
		return NULL;
	case REBOOT_REFUSE_LOCKED:
		return "the camera API stayed locked, so the stop was never "
		       "requested";
	case REBOOT_REFUSE_LOST:
		return "the camera producer did not acknowledge the stop";
	case REBOOT_REFUSE_STATE:
		return "the camera is unusable (lost, or never created), so "
		       "the stop cannot be confirmed";
	case REBOOT_REFUSE_UNKNOWN:
		break;
	}
	return "the camera stop returned an undocumented result";
}
