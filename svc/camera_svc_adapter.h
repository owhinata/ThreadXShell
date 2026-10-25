/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    camera_svc_adapter.h
 * @brief   How a board's `camera` adapter fills a result and writes a report
 *          line (issue #143).
 *
 * The same few helpers in each boards/<board>/port/camera/camera_svc_<b>.c,
 * kept here once as static inline -- the arrangement of svc/nn_svc_adapter.h
 * for the `nn` adapters.  Each adapter TU gets its own private copy.
 *
 * [!] NO STORAGE.  Nothing here holds state; the line buffer is on the
 * caller's stack and a result's words go straight into the caller's result
 * (two consoles may report at once).
 *
 * [!] svc/ DOES NOT INCLUDE A BOARD HEADER OR tx_api.h.
 */
#ifndef CAMERA_SVC_ADAPTER_H
#define CAMERA_SVC_ADAPTER_H

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#include "camera_svc.h"
#include "fmt.h"

/* ---- results ------------------------------------------------------------- */

static inline void camera_res_ok(struct camera_op_result *res)
{
	res->status = CAMERA_SVC_OK;
	res->detail[0] = '\0';
	res->nusers = 0u;
}

static inline void camera_detail_to(struct camera_op_result *res,
                                    const char *f, ...)
	__attribute__((format(printf, 2, 3)));
static inline void camera_detail_to(struct camera_op_result *res,
                                    const char *f, ...)
{
	va_list ap;

	va_start(ap, f);
	(void)fmt_vsnformat(res->detail, sizeof res->detail, f, ap);
	va_end(ap);
}

/* Fill a failure in one place, its sentence checked against
   CAMERA_SVC_DETAIL_MAX at build time (the copy truncates silently). */
#define camera_res_fail(res, st, ...)                                       \
	((void)CAMERA_SVC_DETAIL_CHECK_FMT(__VA_ARGS__),                    \
	 (res)->status = (st), (res)->nusers = 0u,                          \
	 camera_detail_to((res), __VA_ARGS__))

/* IN_USE with the names of the holders that keep the operator out (#142 D8).
   The names are the holders' static strings. */
static inline void camera_res_in_use(struct camera_op_result *res,
                                     const char **users, int n)
{
	res->status = CAMERA_SVC_ERR_IN_USE;
	res->detail[0] = '\0';
	if (n < 0)
		n = 0;
	if (n > (int)CAMERA_SVC_USERS_MAX)
		n = (int)CAMERA_SVC_USERS_MAX;
	for (int i = 0; i < n; i++)
		res->users[i] = users[i];
	res->nusers = (unsigned)n;
}

/* ---- report lines -------------------------------------------------------- */

/**
 * A writer that stops at the first refusal: once @ref failed is set, every
 * later line is dropped rather than retried, so one refused line does not
 * turn into a run of them (camera_svc_write_fn).
 */
struct camera_out {
	camera_svc_write_fn write;
	void *ctx;
	int failed;
};

/*
 * Format ONE WHOLE LINE and hand it to the writer in one call: a line split
 * over two writes can have a background job's output land in the middle.
 * fmt_vsnformat() is the formatter cli_print uses underneath, so the bytes are
 * what the board's old cli_print calls produced.  128 for the reason
 * nn_info_line() gives: a truncated line loses its CRLF first.
 */
static inline void camera_out_v(struct camera_out *o, char *line, size_t cap,
                                const char *f, va_list ap)
{
	int n;

	if (o->failed)
		return;
	n = fmt_vsnformat(line, cap, f, ap);
	if (n < 0 || o->write(o->ctx, line, (size_t)n) < 0)
		o->failed = 1;
}

static inline void camera_out(struct camera_out *o, const char *f, ...)
	__attribute__((format(printf, 2, 3)));
static inline void camera_out(struct camera_out *o, const char *f, ...)
{
	char line[128];
	va_list ap;

	va_start(ap, f);
	camera_out_v(o, line, sizeof line, f, ap);
	va_end(ap);
}

/* The same for the rare line longer than 128 bytes: the caller gives the
   buffer (on its stack), so only that caller pays for it. */
static inline void camera_out_n(struct camera_out *o, char *line, size_t cap,
                                const char *f, ...)
	__attribute__((format(printf, 4, 5)));
static inline void camera_out_n(struct camera_out *o, char *line, size_t cap,
                                const char *f, ...)
{
	va_list ap;

	va_start(ap, f);
	camera_out_v(o, line, cap, f, ap);
	va_end(ap);
}

#endif /* CAMERA_SVC_ADAPTER_H */
