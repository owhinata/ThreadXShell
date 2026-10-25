/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    plugin_info.h
 * @brief   The plugin stack lines of `nn info`, for every board that loads
 *          plugins (issue #111).
 *
 * [!] ONE COPY.  Grove and wio print the same two lines about a validated
 * container's stack; written in each board they would drift apart the way the
 * `nn` output did before issue #50.  A board calls this and adds nothing.
 *
 * [!] KEPT OUT OF plugin_load.c.  The host container verifier links
 * plugin_load.c and crc32.c only, and formatting would pull svc/fmt.c into it.
 *
 * Owns no mutable storage (the line buffer is on the caller's stack).
 */
#ifndef PLUGIN_INFO_H
#define PLUGIN_INFO_H

#include <stddef.h>
#include <stdint.h>

#include "plugin_load.h"

#ifdef __cplusplus
extern "C" {
#endif

/** The same shape as nn_svc_write_fn, so a board passes its writer as is. */
typedef int (*plugin_info_write_fn)(void *ctx, const char *s, size_t len);

/**
 * @brief  Write the `stack` and `decl` lines for @p v.
 *
 *   stack : what each slot NEEDS on this firmware -- max(own, crossing +
 *           max(c, sink)) for a slot that reaches a veneer, own otherwise --
 *           and the c it was computed with, so the number is never shown
 *           without its divisor.
 *   decl  : what the container carries, none of which contains c: each slot's
 *           own deepest frames, its own frames at a crossing ("-" = it reaches
 *           no veneer), and the plugin's sink bound S.
 *
 * Slot order is the ABI's: entry / shapes_ok / decode / draw / report /
 * param_set / param_get.  An absent slot shows 0 (and "-"), as it declares.
 *
 * @param cost  the c the loader used: the board policy's veneer_cost
 * @return 0, or negative once @p write has refused (the second line is then
 *         not attempted)
 */
int plugin_info_stack(plugin_info_write_fn write, void *ctx,
                      const struct plugin_view *v, uint32_t cost);

#ifdef __cplusplus
}
#endif

#endif /* PLUGIN_INFO_H */
