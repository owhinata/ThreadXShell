/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/*
 * The board's bindings table and the record of which word is which veneer
 * (svc/plugin/plugin_bindings.c, issue #165), against the plugin side.
 *
 * cmake/check_veneer_base_cost.py derives the stack root behind each veneer
 * from the word plugin_bind_probe names for it.  The offsets in that record are
 * taken from the struct, so they are right whatever veneer name is written
 * beside them -- a record that put "pl_base_to_frame" against the log member
 * would compile and would charge the log function's stack to the wrong veneer.
 * So the pairs are held against a SECOND SOURCE here: the plugin's own veneers
 * (asset/common/plugin_base.c), each called through the vtables the firmware
 * really hands a plugin (the head of the table at entry, and the painter and
 * printer plugin_painter_init() / plugin_printer_init() fill), with a distinct
 * marker function in every member.  The marker a veneer reaches must be the
 * word at the offset the record gives for that veneer's name, and each veneer
 * must reach exactly one marker.  The veneer names below are this test's own,
 * not read from the record.
 */
#include <stdio.h>
#include <string.h>

#include "plugin_bindings.h"
#include "plugin_base.h"

static int failures;
static int hit[6];

static void ok(const char *what, int cond)
{
	printf("  %s %s\n", cond ? "ok  " : "FAIL", what);
	if (!cond)
		failures++;
}

static void m_log(void *c, const char *s, size_t n)
{ (void)c; (void)s; (void)n; hit[0]++; }
static int m_frame(void *c, float x, float y, float w, float h,
                   struct plugin_rect *o)
{ (void)c; (void)x; (void)y; (void)w; (void)h; (void)o; hit[1]++; return 0; }
static void m_rect(void *c, const struct plugin_rect *r, uint16_t a,
                   uint16_t b)
{ (void)c; (void)r; (void)a; (void)b; hit[2]++; }
static void m_fill(void *c, const struct plugin_rect *r, uint16_t a)
{ (void)c; (void)r; (void)a; hit[3]++; }
static void m_blit(void *c, const struct plugin_rect *r, const uint16_t *s,
                   uint32_t st, int32_t k)
{ (void)c; (void)r; (void)s; (void)st; (void)k; hit[4]++; }
static int m_write(void *c, const char *s, size_t n)
{ (void)c; (void)s; (void)n; hit[5]++; return 0; }

const struct plugin_bindings plugin_board_bindings = {
	.base = { PLUGIN_ABI_VERSION, (uint32_t)sizeof(struct plugin_base_api),
	          NULL, m_log, m_frame },
	.rect = m_rect, .fill_rect = m_fill, .blit = m_blit, .write = m_write,
};

/* The markers, by index, as code pointers to compare with a table word. */
static void (*const marker[6])(void) = {
	(void (*)(void))m_log, (void (*)(void))m_frame, (void (*)(void))m_rect,
	(void (*)(void))m_fill, (void (*)(void))m_blit, (void (*)(void))m_write,
};

static struct plugin_painter painter;
static struct plugin_printer printer;
static int ctx_paint, ctx_print;

static void v_log(void)   { pl_base_log(&plugin_board_bindings.base, "x", 1u); }
static void v_frame(void)
{
	struct plugin_rect r;
	(void)pl_base_to_frame(&plugin_board_bindings.base, 0, 0, 1, 1, &r);
}
static void v_rect(void)
{
	struct plugin_rect r = { 0, 0, 1, 1 };
	pl_paint_rect(&painter, &r, 0u, 1u);
}
static void v_fill(void)
{
	struct plugin_rect r = { 0, 0, 1, 1 };
	pl_paint_fill_rect(&painter, &r, 0u);
}
static void v_blit(void)
{
	struct plugin_rect r = { 0, 0, 1, 1 };
	uint16_t px = 0u;
	pl_paint_blit(&painter, &r, &px, 1u, -1);
}
static void v_write(void) { (void)pl_print_write(&printer, "x", 1u); }

/* This test's own list of the plugin's veneers, by name. */
static const struct { const char *name; void (*call)(void); } veneers[] = {
	{ "pl_base_log", v_log },           { "pl_base_to_frame", v_frame },
	{ "pl_paint_rect", v_rect },        { "pl_paint_fill_rect", v_fill },
	{ "pl_paint_blit", v_blit },        { "pl_print_write", v_write },
};
#define NV (sizeof veneers / sizeof veneers[0])

int main(void)
{
	unsigned i, k;
	char what[160];

	printf("test_plugin_bindings (svc/plugin/plugin_bindings.c):\n");

	memset(&painter, 0xA5, sizeof painter);
	plugin_painter_init(&painter, &ctx_paint);
	ok("[!] the painter's version and size are filled -- the veneer refuses "
	   "a painter without them",
	   painter.version == PLUGIN_ABI_VERSION &&
	   painter.size == sizeof painter);
	ok("its context is the caller's and its three primitives are the "
	   "table's",
	   painter.ctx == &ctx_paint && painter.rect == m_rect &&
	   painter.fill_rect == m_fill && painter.blit == m_blit);
	memset(&printer, 0xA5, sizeof printer);
	plugin_printer_init(&printer, &ctx_print);
	ok("the printer likewise: version, size, the caller's context, the "
	   "table's write",
	   printer.version == PLUGIN_ABI_VERSION &&
	   printer.size == sizeof printer && printer.ctx == &ctx_print &&
	   printer.write == m_write);
	plugin_painter_init(NULL, &ctx_paint);
	plugin_printer_init(NULL, &ctx_print);
	ok("a NULL vtable is ignored", 1);

	ok("the record: magic, the table's size, one field per veneer",
	   plugin_bind_probe.magic == PLUGIN_BIND_PROBE_MAGIC &&
	   plugin_bind_probe.size == sizeof(struct plugin_bindings) &&
	   plugin_bind_probe.count == NV);

	for (i = 0; i < NV; i++) {
		const struct plugin_bind_field *f = NULL;
		unsigned which = 6u, n = 0u;
		void (*word)(void) = NULL;

		for (k = 0; k < plugin_bind_probe.count; k++)
			if (strcmp(plugin_bind_probe.field[k].veneer,
			           veneers[i].name) == 0)
				f = &plugin_bind_probe.field[k];
		memset(hit, 0, sizeof hit);
		veneers[i].call();
		for (k = 0; k < 6u; k++)
			if (hit[k]) {
				n++;
				which = k;
			}
		if (f != NULL && f->offset + sizeof word <=
		                 sizeof(struct plugin_bindings))
			memcpy(&word, (const char *)&plugin_board_bindings +
			              f->offset, sizeof word);
		snprintf(what, sizeof what, "%s: the record has it, the veneer "
		         "reaches exactly one member, and that member is the "
		         "word at the record's offset", veneers[i].name);
		ok(what, f != NULL && n == 1u && which < 6u &&
		         word == marker[which]);
	}

	if (failures) {
		printf("test_plugin_bindings: %d FAILED\n", failures);
		return 1;
	}
	printf("test_plugin_bindings: all passed\n");
	return 0;
}
