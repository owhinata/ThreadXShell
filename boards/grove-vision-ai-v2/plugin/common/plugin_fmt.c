/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    plugin_fmt.c
 * @brief   The little a plugin needs to write a line.  See plugin_fmt.h.
 *
 * Deliberately dull.  There is no width beyond right-alignment, no format
 * string and no float: a plugin that could ask for %f would pull a float
 * formatter into an image measured in kilobytes, and every board's firmware
 * already refuses to carry one.
 */
#include "plugin_fmt.h"

#include "plugin_base.h"

int pl_fmt_str(const struct plugin_printer *out, const char *s, size_t len)
{
	int rc;

	if (out == NULL || out->write == NULL)
		return -1;
	if (len == 0u)
		return 0;
	/* Through the veneer, never the pointer: see plugin_base.c. */
	rc = pl_print_write(out, s, len);
	return rc < 0 ? rc : 0;
}

int pl_fmt_cstr(const struct plugin_printer *out, const char *s)
{
	size_t n = 0u;

	if (s == NULL)
		return -1;
	while (s[n] != '\0')
		n++;
	return pl_fmt_str(out, s, n);
}

/*
 * Format backwards into @p buf and return the offset of the first digit.
 *
 * Backwards because the low digit is the one arithmetic produces first;
 * 11 bytes hold every uint32_t, and the do/while writes a '0' for zero.
 */
static unsigned pl_utoa(char *buf, unsigned len, uint32_t v)
{
	unsigned i = len;

	do {
		buf[--i] = (char)('0' + (v % 10u));
		v /= 10u;
	} while (v != 0u && i != 0u);
	return i;
}

int pl_fmt_u32(const struct plugin_printer *out, uint32_t v)
{
	char b[12];
	unsigned at = pl_utoa(b, (unsigned)sizeof b, v);

	return pl_fmt_str(out, b + at, sizeof b - at);
}

/*
 * The magnitude of a possibly-negative value, without overflowing on INT32_MIN.
 *
 * [!] `-(int32_t)v` IS UNDEFINED FOR INT32_MIN, and a dequantised score is a
 * float cast to an int, so the extreme is not hypothetical -- a NaN or a huge
 * product casts to exactly this value.  Negating in the unsigned domain is
 * defined for every input.
 */
static uint32_t pl_abs32(int32_t v)
{
	return v < 0 ? (uint32_t)0 - (uint32_t)v : (uint32_t)v;
}

int pl_fmt_i32(const struct plugin_printer *out, int32_t v)
{
	char b[12];
	unsigned at = pl_utoa(b, (unsigned)sizeof b, pl_abs32(v));

	if (v < 0)
		b[--at] = '-';
	return pl_fmt_str(out, b + at, sizeof b - at);
}

int pl_fmt_i32_pad(const struct plugin_printer *out, int32_t v, unsigned width)
{
	char b[12];
	unsigned at = pl_utoa(b, (unsigned)sizeof b, pl_abs32(v));
	unsigned n;
	int rc = 0;

	if (v < 0)
		b[--at] = '-';
	n = (unsigned)sizeof b - at;
	while (rc == 0 && n < width) {
		rc = pl_fmt_str(out, " ", 1u);
		n++;
	}
	if (rc == 0)
		rc = pl_fmt_str(out, b + at, (size_t)((unsigned)sizeof b - at));
	return rc;
}

int pl_fmt_cstr_pad(const struct plugin_printer *out, const char *s,
                    unsigned width)
{
	size_t n = 0u;
	int rc;

	if (s == NULL)
		return -1;
	while (s[n] != '\0')
		n++;
	rc = pl_fmt_str(out, s, n);
	while (rc == 0 && n < (size_t)width) {
		rc = pl_fmt_str(out, " ", 1u);
		n++;
	}
	return rc;
}
