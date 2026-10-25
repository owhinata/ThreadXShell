/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    camera_names.h
 * @brief   The words this board's camera is described in, shared by its two
 *          writers: the `camera` adapter (camera_svc_f746.c) and the board's
 *          own subcommands (cmds/cmd_camera_board.c) (issue #143).
 *
 * Until issue #143 both halves lived in one cmds/ file.  Splitting the shared
 * command off put `camera info` (which prints the settings and the format
 * names) in the port and `camera set` (which prints the same settings and
 * parses the same names) in cmds/, so the tables and the sentences they print
 * live here once rather than in two copies that would drift.
 *
 * The port must not reach up into cmds/, and cmds/ already depends on the port,
 * so this sits in the port.  It holds no shell instance: lines go through the
 * length-bearing writer of svc/camera/camera_svc.h (svc/camera/camera_svc_adapter.h).
 */
#ifndef CAMERA_NAMES_H
#define CAMERA_NAMES_H

#include <stddef.h>

#include "camera.h"
#include "camera_svc.h"
#include "camera_svc_adapter.h" /* struct camera_out, camera_out() */

#ifdef __cplusplus
extern "C" {
#endif

/** One symbolic value of an enumerated setting (name <-> port enum). */
struct camera_named { const char *name; int val; };

/* NULL-name terminated.  RGB888 is unsupported; JPEG is snapshot-only and
   gated to <= VGA (owhinata/stm32f746g-disco#45). */
extern const struct camera_named camera_awb_names[];
extern const struct camera_named camera_effect_names[];
extern const struct camera_named camera_flip_names[];
extern const struct camera_named camera_res_names[];
extern const struct camera_named camera_format_names[];

/** Name -> value; 0, or -1 for a name not in @p t. */
int camera_named_lookup(const struct camera_named *t, const char *s, int *out);

/** Value -> name, or "?" for a value not in @p t. */
const char *camera_named_name(const struct camera_named *t, int v);

/** A CAM_ERR_* code in the words the console has always printed for it. */
const char *camera_strerror(int rc);

/** The nine `camera set` values, one line each -- `camera info` and a bare
 *  `camera set` print exactly these. */
void camera_settings_write(struct camera_out *o, const struct camera_settings *s);

#ifdef __cplusplus
}
#endif

#endif /* CAMERA_NAMES_H */
