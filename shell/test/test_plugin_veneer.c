/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Host unit test for the plugin's painter and printer veneers
 * (asset/common/plugin_base.c) -- issue #111.
 *
 * ABI 2 put a version and a size at the head of struct plugin_painter and struct
 * plugin_printer, and the veneers call through a member only when the version is
 * this ABI's and the size reaches the end of that member.  Three answers per
 * member, each from a vtable that differs from a good one in ONE field:
 *
 *   the version is another ABI's            -> not called
 *   the size stops one byte short of it     -> not called
 *   the size ends exactly at it             -> called
 *
 * The third is what makes the second mean something: a check written as
 * `size == sizeof` would pass the first two and refuse a base that has since
 * appended a member -- the very growth the size field exists to allow.
 *
 * [!] asset/common IS BOARD-INDEPENDENT (every board's plugins link it), so this
 * runs with the other board-independent tests rather than under one board.
 */
#include "plugin_abi.h"
#include "plugin_base.h"

#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

static unsigned calls_rect, calls_fill, calls_blit, calls_write;

static void f_rect(void *ctx, const struct plugin_rect *r, uint16_t c,
                   uint16_t s)
{
	(void)ctx; (void)r; (void)c; (void)s;
	calls_rect++;
}

static void f_fill(void *ctx, const struct plugin_rect *r, uint16_t c)
{
	(void)ctx; (void)r; (void)c;
	calls_fill++;
}

static void f_blit(void *ctx, const struct plugin_rect *r, const uint16_t *src,
                   uint32_t stride, int32_t key)
{
	(void)ctx; (void)r; (void)src; (void)stride; (void)key;
	calls_blit++;
}

static int f_write(void *ctx, const char *s, size_t len)
{
	(void)ctx; (void)s;
	calls_write++;
	return (int)len;
}

/* Where a member ends, which is the least a size may say for it to be called. */
#define END_OF(type, m) \
	((uint32_t)(offsetof(type, m) + sizeof(((type *)0)->m)))

static struct plugin_painter painter(uint32_t version, uint32_t size)
{
	struct plugin_painter p;

	p.version   = version;
	p.size      = size;
	p.ctx       = NULL;
	p.rect      = f_rect;
	p.fill_rect = f_fill;
	p.blit      = f_blit;
	return p;
}

static void reset(void)
{
	calls_rect = calls_fill = calls_blit = calls_write = 0u;
}

static void check(const char *what, unsigned got, unsigned want)
{
	if (got != want) {
		printf("  FAIL: %s -- called %u time(s), wanted %u\n", what, got,
		       want);
		assert(0);
	}
	printf("  %-52s -> %s\n", what, want ? "called" : "not called");
}

static void test_painter(void)
{
	struct plugin_rect r = { 0, 0, 1, 1 };
	uint16_t px = 0u;
	struct plugin_painter p;

	printf(" case: the painter veneers\n");

	p = painter(PLUGIN_ABI_VERSION, (uint32_t)sizeof(struct plugin_painter));
	reset();
	pl_paint_rect(&p, &r, 1u, 1u);
	pl_paint_fill_rect(&p, &r, 1u);
	pl_paint_blit(&p, &r, &px, 1u, -1);
	check("a whole painter of this ABI: rect", calls_rect, 1u);
	check("... fill_rect", calls_fill, 1u);
	check("... blit", calls_blit, 1u);

	p = painter(PLUGIN_ABI_VERSION + 1u,
	            (uint32_t)sizeof(struct plugin_painter));
	reset();
	pl_paint_rect(&p, &r, 1u, 1u);
	pl_paint_fill_rect(&p, &r, 1u);
	pl_paint_blit(&p, &r, &px, 1u, -1);
	check("another ABI's painter: rect", calls_rect, 0u);
	check("... fill_rect", calls_fill, 0u);
	check("... blit", calls_blit, 0u);

	p = painter(PLUGIN_ABI_VERSION, END_OF(struct plugin_painter, rect) - 1u);
	reset();
	pl_paint_rect(&p, &r, 1u, 1u);
	check("size one byte short of rect", calls_rect, 0u);
	p.size = END_OF(struct plugin_painter, rect);
	pl_paint_rect(&p, &r, 1u, 1u);
	pl_paint_fill_rect(&p, &r, 1u);
	check("size ending at rect: rect", calls_rect, 1u);
	check("... but not fill_rect, which lies past it", calls_fill, 0u);

	p = painter(PLUGIN_ABI_VERSION,
	            END_OF(struct plugin_painter, fill_rect) - 1u);
	reset();
	pl_paint_fill_rect(&p, &r, 1u);
	check("size one byte short of fill_rect", calls_fill, 0u);
	p.size = END_OF(struct plugin_painter, fill_rect);
	pl_paint_fill_rect(&p, &r, 1u);
	check("size ending at fill_rect", calls_fill, 1u);

	p = painter(PLUGIN_ABI_VERSION, END_OF(struct plugin_painter, blit) - 1u);
	reset();
	pl_paint_blit(&p, &r, &px, 1u, -1);
	check("size one byte short of blit", calls_blit, 0u);
	p.size = END_OF(struct plugin_painter, blit);
	pl_paint_blit(&p, &r, &px, 1u, -1);
	check("size ending at blit", calls_blit, 1u);

	/* A base that has appended a member since: larger than this plugin knows,
	 * and every member it does know is still called. */
	p = painter(PLUGIN_ABI_VERSION,
	            (uint32_t)sizeof(struct plugin_painter) + 16u);
	reset();
	pl_paint_blit(&p, &r, &px, 1u, -1);
	check("a larger painter from a newer base", calls_blit, 1u);
}

static void test_printer(void)
{
	struct plugin_printer o;
	int rc;

	printf(" case: the printer veneer\n");

	o.version = PLUGIN_ABI_VERSION;
	o.size    = (uint32_t)sizeof(struct plugin_printer);
	o.ctx     = NULL;
	o.write   = f_write;
	reset();
	rc = pl_print_write(&o, "ab", 2u);
	check("a whole printer of this ABI", calls_write, 1u);
	assert(rc == 2);

	/* [!] A REFUSED WRITE IS A FAILURE THE PLUGIN PROPAGATES, not a silent
	 * success: the report must stop, as it does for a sink that said no. */
	o.version = PLUGIN_ABI_VERSION + 1u;
	reset();
	rc = pl_print_write(&o, "ab", 2u);
	check("another ABI's printer", calls_write, 0u);
	assert(rc < 0);

	o.version = PLUGIN_ABI_VERSION;
	o.size    = END_OF(struct plugin_printer, write) - 1u;
	reset();
	rc = pl_print_write(&o, "ab", 2u);
	check("size one byte short of write", calls_write, 0u);
	assert(rc < 0);

	o.size = END_OF(struct plugin_printer, write);
	rc = pl_print_write(&o, "ab", 2u);
	check("size ending at write", calls_write, 1u);
	assert(rc == 2);
}

int main(void)
{
	printf("test_plugin_veneer (asset/common/plugin_base.c):\n");
	test_painter();
	test_printer();
	printf("test_plugin_veneer: all passed\n");
	return 0;
}
