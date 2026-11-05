/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Host-test shim for svc/log.h, as src/blob_writer.c uses it (issue #194): the
 * levels and log_write(), which test_blob_writer.c defines and counts.
 */
#ifndef LOG_H
#define LOG_H

#define LOG_LEVEL_ERR 0u
#define LOG_LEVEL_WRN 1u
#define LOG_LEVEL_INF 2u
#define LOG_LEVEL_DBG 3u

void log_write(unsigned level, const char *tag, const char *fmt, ...);

#endif /* LOG_H */
