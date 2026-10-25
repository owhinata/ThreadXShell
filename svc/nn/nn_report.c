/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    nn_report.c
 * @brief   The bounded capture sink.  See nn_report.h.
 *
 * [!] NO MUTABLE STORAGE: the capture is the caller's, one per command frame,
 * which is the whole point of the design.
 */
#include "nn_report.h"

#include <string.h>

/* Latched in the status while the capture is still being filled, so that
 * nn_report_end() can tell an overflow from a decoder's own refusal. */
#define CAP_OVERFLOWED  NN_REPORT_TRUNCATED

void nn_report_begin(struct nn_report_capture *r)
{
	if (r == NULL)
		return;
	r->len    = 0u;
	r->status = (uint8_t)NN_REPORT_NONE;
}

void nn_report_set(struct nn_report_capture *r, enum nn_report_status st)
{
	if (r == NULL)
		return;
	r->status = (uint8_t)st;
}

int nn_report_write(void *ctx, const char *s, size_t len)
{
	struct nn_report_capture *r = (struct nn_report_capture *)ctx;
	uint32_t room;

	if (r == NULL || s == NULL)
		return -1;
	if (r->status == (uint8_t)CAP_OVERFLOWED)
		return -1;                 /* latched: it has already said no */
	if (len == 0u)
		return 0;
	if (r->buf == NULL || r->cap == 0u) {
		r->status = (uint8_t)CAP_OVERFLOWED;
		return -1;
	}

	room = r->cap - r->len;
	if ((uint32_t)len > room) {
		/* Keep the prefix that fits and refuse from here on. */
		if (room != 0u) {
			memcpy(r->buf + r->len, s, room);
			r->len += room;
		}
		r->status = (uint8_t)CAP_OVERFLOWED;
		return -1;
	}
	memcpy(r->buf + r->len, s, len);
	r->len += (uint32_t)len;
	return (int)len;
}

void nn_report_end(struct nn_report_capture *r, int rc)
{
	if (r == NULL)
		return;
	if (r->status == (uint8_t)CAP_OVERFLOWED)
		return;                    /* truncation outranks refusal */
	r->status = (uint8_t)(rc < 0 ? NN_REPORT_REFUSED : NN_REPORT_OK);
}
