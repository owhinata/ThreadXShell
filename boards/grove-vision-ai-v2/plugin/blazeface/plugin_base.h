/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/** @file plugin_base.h  @brief Named veneers for the base vtable.  See the .c. */
#ifndef PLUGIN_BASE_H
#define PLUGIN_BASE_H

#include <stddef.h>

#include "plugin_abi.h"

void pl_base_log(const struct plugin_base_api *base, const char *s, size_t len);

int pl_base_to_frame(const struct plugin_base_api *base, float x, float y,
                     float w, float h, struct plugin_rect *out);

/* The painter and the printer are handed in per call, but they are vtables just
 * the same, so they get veneers for the same reason. */
void pl_paint_rect(const struct plugin_painter *p, const struct plugin_rect *r,
                   uint16_t rgb565, uint16_t stroke);
void pl_paint_fill_rect(const struct plugin_painter *p,
                        const struct plugin_rect *r, uint16_t rgb565);
void pl_paint_blit(const struct plugin_painter *p, const struct plugin_rect *r,
                   const uint16_t *src, uint32_t src_stride, int32_t key);
int  pl_print_write(const struct plugin_printer *o, const char *s, size_t len);

#endif /* PLUGIN_BASE_H */
