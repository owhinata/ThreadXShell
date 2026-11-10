/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Host stand-in for svc/log.h, for test_kv_qspi.c (issue #214): the levels,
 * the build-time floor, the macros, the three writers and the line assembler
 * port/flashdb/fal_qspi.c uses.  The test defines the functions and keeps the
 * lines.
 */
#ifndef LOG_H
#define LOG_H

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#define LOG_LEVEL_ERR 0u
#define LOG_LEVEL_WRN 1u
#define LOG_LEVEL_INF 2u
#define LOG_LEVEL_DBG 3u

#ifndef LOG_COMPILE_LEVEL
#define LOG_COMPILE_LEVEL LOG_LEVEL_DBG
#endif

#ifndef LOG_TAG
#define LOG_TAG "??"
#endif

#define LOG_MSG_MAX 104

#define LOG_AT(lvl, ...) \
	do { \
		if ((lvl) <= LOG_COMPILE_LEVEL) \
			log_write((lvl), LOG_TAG, __VA_ARGS__); \
	} while (0)
#define LOG_ERR(...) LOG_AT(LOG_LEVEL_ERR, __VA_ARGS__)
#define LOG_WRN(...) LOG_AT(LOG_LEVEL_WRN, __VA_ARGS__)
#define LOG_INF(...) LOG_AT(LOG_LEVEL_INF, __VA_ARGS__)
#define LOG_DBG(...) LOG_AT(LOG_LEVEL_DBG, __VA_ARGS__)

void log_write(unsigned level, const char *tag, const char *fmt, ...)
	__attribute__((format(printf, 3, 4)));
void log_vwrite(unsigned level, const char *tag, const char *fmt, va_list ap);
void log_write_bytes(unsigned level, const char *tag, const char *prefix,
                     const char *s, size_t len, const char *more);

#define LOG_LINE_ASM_MAX (LOG_MSG_MAX + 1)

struct log_line {
	char     buf[LOG_LINE_ASM_MAX];
	uint16_t used;
	uint8_t  esc;
};

void log_line_init(struct log_line *l);
void log_line_feed(struct log_line *l, const char *tag, const void *data,
                   size_t len);
void log_line_flush(struct log_line *l, const char *tag);

#endif /* LOG_H */
