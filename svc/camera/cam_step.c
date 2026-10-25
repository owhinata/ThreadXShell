/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * A camera sink's step machine (issue #142).  See cam_step.h.  Pure: the
 * critical section every caller runs these in is the board's; this TU owns no
 * storage (svc/, audited per board).
 */
#include "cam_step.h"

int cam_step_enter(struct cam_step_state *s)
{
	if (!s->armed || s->busy == UINT16_MAX)
		return 0;
	s->busy++;
	return 1;
}

int cam_step_leave(struct cam_step_state *s)
{
	if (s->busy == 0u)
		return CAM_STEP_E_IDLE;
	s->busy--;
	return CAM_STEP_OK;
}

int cam_step_may_arm(const struct cam_step_state *s)
{
	return !s->armed && s->busy == 0u;
}

int cam_step_arm(struct cam_step_state *s)
{
	if (s->armed)
		return CAM_STEP_E_ARMED;
	if (s->busy != 0u)
		return CAM_STEP_E_BUSY;
	s->armed = 1u;
	return CAM_STEP_OK;
}

void cam_step_disarm(struct cam_step_state *s)
{
	s->armed = 0u;
}
