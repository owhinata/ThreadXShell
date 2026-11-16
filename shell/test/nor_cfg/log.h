/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Host stand-in for a board's svc/log.h, for test_nor_cmd.c (issue #250):
 * log_write() keeps the last line, which is how what an erase, write or test
 * leaves in `dmesg` is pinned.
 */
#ifndef LOG_H
#define LOG_H

#define LOG_LEVEL_ERR 0u
#define LOG_LEVEL_WRN 1u
#define LOG_LEVEL_INF 2u
#define LOG_LEVEL_DBG 3u

void log_write(unsigned level, const char *tag, const char *fmt, ...);

#endif /* LOG_H */
