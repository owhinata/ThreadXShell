/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    plugin_libc.c
 * @brief   The freestanding remnant of libc a plugin needs (issue #101).
 *
 * [!] MEASURED, NOT ASSUMED.  The reviewed plan said a plugin would need only
 * libgcc.  Running `nm -u` over the firmware's own object for svc/blazeface.c
 * showed exactly one undefined symbol -- `memset` -- which GCC synthesises from
 * an initialisation loop and which libgcc does NOT provide: it is libc's.  A
 * `-nostdlib` link would therefore have failed to resolve it, and the host
 * gate's "zero undefined symbols" rule is what turned that into a build error
 * rather than a surprise much later.
 *
 * Providing it here rather than linking newlib keeps the rule that makes the
 * gate cheap to state: a plugin image resolves everything within itself, so
 * "no undefined symbols" is the whole check.  These are deliberately dull -- the
 * compiler emits calls to them from code it generated, so cleverness here buys
 * nothing and a bug would be very hard to attribute.
 */
#include <stddef.h>
#include <stdint.h>

void *memset(void *dst, int c, size_t n)
{
	uint8_t *p = (uint8_t *)dst;

	while (n-- != 0u)
		*p++ = (uint8_t)c;
	return dst;
}

void *memcpy(void *dst, const void *src, size_t n)
{
	uint8_t       *d = (uint8_t *)dst;
	const uint8_t *s = (const uint8_t *)src;

	while (n-- != 0u)
		*d++ = *s++;
	return dst;
}
