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
 * length-bearing writer of svc/camera_svc.h.
 */
#ifndef CAMERA_NAMES_H
#define CAMERA_NAMES_H

#include <stdarg.h>
#include <stddef.h>

#include "camera.h"
#include "camera_svc.h"
#include "fmt.h"

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

/**
 * A writer that stops at the first refusal: once @ref failed is set, every
 * later line is dropped rather than retried, so one refused line does not
 * turn into a run of them (svc/camera_svc.h, camera_svc_write_fn).
 */
struct camera_out {
	camera_svc_write_fn write;
	void *ctx;
	int failed;
};

/*
 * Format one piece of a line and hand it to the writer.  fmt_vsnformat() is the
 * formatter cli_print uses underneath, so what lands on the console is what the
 * old cli_print calls produced.  128 is the same bound as nn_info_line() and
 * for the same reason: a truncated line loses its CRLF first.  On the caller's
 * stack, never static (two consoles may write at once).
 */
static inline void camera_out(struct camera_out *o, const char *f, ...)
	__attribute__((format(printf, 2, 3)));
static inline void camera_out(struct camera_out *o, const char *f, ...)
{
	char line[128];
	va_list ap;
	int n;

	if (o->failed)
		return;
	va_start(ap, f);
	n = fmt_vsnformat(line, sizeof line, f, ap);
	va_end(ap);
	if (n < 0 || o->write(o->ctx, line, (size_t)n) < 0)
		o->failed = 1;
}

/** The nine `camera set` values, one line each -- `camera info` and a bare
 *  `camera set` print exactly these. */
void camera_settings_write(struct camera_out *o, const struct camera_settings *s);

#ifdef __cplusplus
}
#endif

#endif /* CAMERA_NAMES_H */
