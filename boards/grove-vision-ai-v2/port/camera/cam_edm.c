/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/*
 * The EDM observer's bookkeeping (issue #68).  See cam_edm.h for what EDM is
 * and why this branch of its interrupt reaches nobody.
 *
 * Pure: no register, no log, no kernel call.  Everything this needs has already
 * been read out by the caller, which is what lets test/test_cam_edm.c compile
 * the real thing rather than a copy of it.
 */
#include <stddef.h>

#include "cam_edm.h"

int cam_edm_should_log(uint32_t count)
{
	/* The first, then every power of two.  Zero is not an occurrence, and
	 * UINT32_MAX -- where the count saturates -- has more than one bit set,
	 * so a saturated counter selects nothing.  That is the property that
	 * bounds the trail. */
	return count != 0u && (count & (count - 1u)) == 0u;
}

int cam_edm_note(struct cam_edm_state *st, const struct cam_edm_event *ev)
{
	if (st == NULL || ev == NULL)
		return 0;

	/* First event: latch what only the first one can say.  The test for it
	 * is the counter being zero, not a separate flag, so there is one piece
	 * of state to get wrong instead of two. */
	if (st->events == 0u) {
		st->first_status = ev->status;
		st->first_tick   = ev->tick;
		st->first_gen    = ev->generation;
	}

	st->last_status = ev->status;

	/* Saturate.  cam_edm.h explains why this may not wrap. */
	if (st->events != 0xFFFFFFFFu)
		st->events++;

	return cam_edm_should_log(st->events);
}

void cam_edm_snapshot(const struct cam_edm_state *st, struct cam_edm_state *out)
{
	if (st == NULL || out == NULL)
		return;
	*out = *st;
}
