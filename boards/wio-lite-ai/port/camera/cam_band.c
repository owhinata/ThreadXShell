/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Wio Lite AI ThreadX Shell Project
 */
/**
 * @file    cam_band.c
 * @brief   The camera's one band sink and its two steps (owhinata/wio-lite-ai#9
 * phase 3; issue #142).  See cam_band.h for why this exists and what it promises.
 */
#include "cam_band.h"

#include "cam_step.h"    /* the step machine, shared with f746g-disco (#142) */
#include "stm32h7xx.h"   /* __get_PRIMASK / __disable_irq / __set_PRIMASK */
#include "tx_api.h"

#include <stddef.h>

static struct cam_step_state band_step[CAM_BAND_CLIENTS];
static cam_band_client_fn    band_fn[CAM_BAND_CLIENTS];

/*
 * The one real camera_band_fn.  Runs on the camera's producer thread.
 *
 * [!] THE ARMED TEST AND THE BUSY INCREMENT ARE ONE ATOMIC STEP (cam_step.h).
 * Disarming a step and then waiting for a "busy" flag would miss the producer
 * that has already passed the armed test and not yet entered the callback: it
 * would run afterwards, writing a buffer whose owner believed the drain was
 * complete.  Once a step reads disarmed inside the critical section, a
 * producer pass has either already counted itself in -- and the drain waits
 * for it -- or will skip.  There is no third case.
 */
void cam_band_sink(void *ctx, unsigned band, const uint16_t *px, unsigned rows)
{
	unsigned c;

	(void)ctx;
	for (c = 0; c < CAM_BAND_CLIENTS; c++) {
		cam_band_client_fn fn = NULL;
		uint32_t primask;

		primask = __get_PRIMASK();
		__disable_irq();
		if (cam_step_enter(&band_step[c]))
			fn = band_fn[c];
		__set_PRIMASK(primask);

		if (fn == NULL)
			continue;

		fn(band, px, rows);

		/* [!] Re-read PRIMASK here rather than reusing the value from entry: the
		 * callback may have changed the interrupt state, and restoring a stale
		 * value would leave interrupts in whatever state they were two sections
		 * ago.  The same reason nn_session_release() saves its own. */
		primask = __get_PRIMASK();
		__disable_irq();
		(void)cam_step_leave(&band_step[c]);
		__set_PRIMASK(primask);
	}
}

int cam_band_step_enable(enum cam_band_client c, cam_band_client_fn fn)
{
	uint32_t primask;
	int rc;

	if (c >= CAM_BAND_CLIENTS || fn == NULL)
		return CAM_BAND_ERR_ARG;
	primask = __get_PRIMASK();
	__disable_irq();
	if (!cam_step_may_arm(&band_step[c])) {
		rc = CAM_BAND_ERR_BUSY;
	} else {
		/* Disarmed and nobody inside: the callback can be swapped safely. */
		band_fn[c] = fn;
		rc = (cam_step_arm(&band_step[c]) == CAM_STEP_OK) ? CAM_BAND_OK
		                                                   : CAM_BAND_ERR_BUSY;
	}
	__set_PRIMASK(primask);
	return rc;
}

void cam_band_step_disable(enum cam_band_client c)
{
	uint32_t primask;

	if (c >= CAM_BAND_CLIENTS)
		return;
	primask = __get_PRIMASK();
	__disable_irq();
	cam_step_disarm(&band_step[c]);
	__set_PRIMASK(primask);
}

int cam_band_step_armed(enum cam_band_client c)
{
	return (c < CAM_BAND_CLIENTS) ? band_step[c].armed != 0u : 0;
}

int cam_band_step_idle(enum cam_band_client c)
{
	uint32_t primask;
	int idle;

	if (c >= CAM_BAND_CLIENTS)
		return 0;
	primask = __get_PRIMASK();
	__disable_irq();
	idle = cam_step_may_arm(&band_step[c]);
	__set_PRIMASK(primask);
	return idle;
}

/*
 * Bounded drain, wall clock (issue #65): the producer runs at priority 10 and
 * every caller of this is below it, so a callback in flight preempts us and
 * finishes promptly; the sleep is what lets it.
 */
int cam_band_step_drain(enum cam_band_client c, uint32_t ticks)
{
	ULONG start = tx_time_get();

	if (c >= CAM_BAND_CLIENTS)
		return CAM_BAND_ERR_ARG;
	for (;;) {
		uint32_t primask = __get_PRIMASK();
		unsigned busy;

		__disable_irq();
		busy = band_step[c].busy;
		__set_PRIMASK(primask);
		if (busy == 0u)
			return CAM_BAND_OK;
		if ((tx_time_get() - start) >= (ULONG)ticks)
			return CAM_BAND_ERR_BUSY;
		tx_thread_sleep(1);
	}
}
