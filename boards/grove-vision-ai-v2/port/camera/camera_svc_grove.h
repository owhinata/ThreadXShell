/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    camera_svc_grove.h
 * @brief   What this board's own `camera` subcommands (cmds/cmd_camera_board.c)
 *          share with its adapter (camera_svc_grove.c) (issue #143).
 *
 * The producer's stage profile is printed by `camera stream stats` (the
 * adapter's report), by the board's `camera stats` and by `camera bench`, so
 * its lines live once, in the adapter, behind the length-bearing writer.
 */
#ifndef CAMERA_SVC_GROVE_H
#define CAMERA_SVC_GROVE_H

#include "camera.h"
#include "camera_svc_adapter.h"   /* struct camera_out */

/** The `profile  :` line and its seven stage rows, from one stats sample. */
void camera_grove_profile_write(struct camera_out *o,
                                const struct camera_stats *st);

#endif /* CAMERA_SVC_GROVE_H */
