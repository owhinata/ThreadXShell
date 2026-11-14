/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Host stand-in for a board's svc/log.h, for the tests of the shared microSD
 * mount glue and FileX media driver (issue #233): the board's macros, over a
 * log_write() that shell/test/sd_fake.c records as "tag: text" lines, which is
 * how what the glue leaves in `dmesg` is pinned.
 */
#ifndef LOG_H
#define LOG_H

#define LOG_LEVEL_ERR 0u
#define LOG_LEVEL_WRN 1u
#define LOG_LEVEL_INF 2u
#define LOG_LEVEL_DBG 3u

#ifndef LOG_TAG
#define LOG_TAG "??"
#endif

void log_write(unsigned level, const char *tag, const char *fmt, ...)
	__attribute__((format(printf, 3, 4)));

#define LOG_ERR(...) log_write(LOG_LEVEL_ERR, LOG_TAG, __VA_ARGS__)
#define LOG_WRN(...) log_write(LOG_LEVEL_WRN, LOG_TAG, __VA_ARGS__)
#define LOG_INF(...) log_write(LOG_LEVEL_INF, LOG_TAG, __VA_ARGS__)
#define LOG_DBG(...) log_write(LOG_LEVEL_DBG, LOG_TAG, __VA_ARGS__)

#endif /* LOG_H */
