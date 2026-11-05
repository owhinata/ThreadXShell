/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Host stand-in for a board's svc/log.h, for test_blob_cmd.c (issue #194):
 * log_write() formats into a record the test reads back, which is how what a
 * `blob write` leaves in `dmesg` is pinned.
 */
#ifndef LOG_H
#define LOG_H

#define LOG_LEVEL_ERR 0u
#define LOG_LEVEL_WRN 1u
#define LOG_LEVEL_INF 2u
#define LOG_LEVEL_DBG 3u

void log_write(unsigned level, const char *tag, const char *fmt, ...);

/* Every line written since blob_test_log_clear(), newline-separated. */
const char *blob_test_log(void);
void blob_test_log_clear(void);

#endif /* LOG_H */
