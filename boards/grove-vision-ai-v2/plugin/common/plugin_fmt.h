/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    plugin_fmt.h
 * @brief   The little a plugin needs to write a line (issue #103).
 *
 * A plugin formats its own text: @ref plugin_printer is length-bearing and not
 * varargs, so no formatter crosses the ABI.  Reaching for one would pull a float
 * formatter into three firmwares at once, which is the trap svc/fmt.c documents.
 *
 * [!] EVERY FUNCTION HERE PROPAGATES A REFUSAL, AND THAT IS WHY THEY ARE SHARED.
 * A sink that has said no has said no; carrying on turns one refused line into a
 * run of them and the caller sees only the last.  Two plugins spelling that rule
 * separately is two chances to spell it wrong, and the second plugin is where a
 * transcription would have started.
 *
 * The return convention is 0 or negative, NOT a byte count: what a caller does
 * with it is `if (rc == 0) rc = next(...)`, and a positive length would make
 * that read backwards.
 */
#ifndef PLUGIN_FMT_H
#define PLUGIN_FMT_H

#include <stddef.h>
#include <stdint.h>

#include "plugin_abi.h"

/** Write @p len bytes.  @return 0, or the writer's negative refusal. */
int pl_fmt_str(const struct plugin_printer *out, const char *s, size_t len);

/** Write a NUL-terminated string.  @return 0, or negative. */
int pl_fmt_cstr(const struct plugin_printer *out, const char *s);

/** Write @p v in decimal.  @return 0, or negative. */
int pl_fmt_u32(const struct plugin_printer *out, uint32_t v);

/** Write @p v in decimal, with a leading '-' when negative. */
int pl_fmt_i32(const struct plugin_printer *out, int32_t v);

/**
 * Write @p v in decimal, right-aligned in at least @p width columns.
 *
 * Columns, because a report of several rows is read down a column and ragged
 * numbers are what make that hard.  Padding is spaces and never truncates: a
 * number wider than @p width is written in full.
 */
int pl_fmt_i32_pad(const struct plugin_printer *out, int32_t v, unsigned width);

/** Write @p s, then spaces up to @p width columns.  Never truncates. */
int pl_fmt_cstr_pad(const struct plugin_printer *out, const char *s,
                    unsigned width);

#endif /* PLUGIN_FMT_H */
