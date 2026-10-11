/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * The camera holder rules of this board (issue #142).  See cam_hold_rules.h.
 */
#include "cam_hold_rules.h"

int cam_teardown_proceeds(int err, int stop_req, unsigned holders)
{
	if (err)
		return 1;
	if (!stop_req)
		return 0;
	return holders == 0u;
}

int cam_join_format_ok(int rgb565, int streamable)
{
	return rgb565 && streamable;
}
