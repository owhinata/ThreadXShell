/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    plugin_info.c
 * @brief   The plugin stack lines of `nn info`.  See plugin_info.h.
 */
#include "plugin_info.h"

#include <stdarg.h>

#include "fmt.h"

/*
 * One line through the bounded formatter.  128, like the boards' own
 * nn_info_line(): a line cut short there loses its CRLF and the next one runs
 * on (issue #103).  Worst case here, with 4-digit bounds and a 10-digit c and
 * sink, is 119 B.
 */
static int info_line(plugin_info_write_fn write, void *ctx, const char *f, ...)
{
	char line[128];
	va_list ap;
	int n;

	va_start(ap, f);
	n = fmt_vsnformat(line, sizeof line, f, ap);
	va_end(ap);
	if (n < 0)
		return -1;
	if ((size_t)n >= sizeof line)
		n = (int)sizeof line - 1;
	return write(ctx, line, (size_t)n);
}

/*
 * One slot's frames at a crossing, as text: the number when the slot reaches a
 * veneer, "-" when it does not -- A1 is 0 there by the canonical form, and
 * printing that 0 would read as a crossing with no frames of its own.
 * @p out holds >= 21 chars (fmt_utoa's 20 + NUL).
 */
static const char *cross_text(char *out, const struct plugin_view *v,
                              unsigned slot)
{
	int n;

	if (v->slot[slot] == PLUGIN_SLOT_ABSENT ||
	    ((v->stack_crossing >> slot) & 1u) == 0u)
		return "-";
	n = fmt_utoa(v->stack_cross[slot], 10u, 0, out);
	out[n] = '\0';
	return out;
}

int plugin_info_stack(plugin_info_write_fn write, void *ctx,
                      const struct plugin_view *v, uint32_t cost)
{
	char x[PLUGIN_SLOT_COUNT][21];

	if (write == NULL || v == NULL)
		return -1;
	if (info_line(write, ctx,
	              "  stack : entry %lu  shapes %lu  decode %lu  draw %lu  "
	              "report %lu  param %lu/%lu B needed at c %lu B\r\n",
	              (unsigned long)v->stack[PLUGIN_SLOT_ENTRY],
	              (unsigned long)v->stack[PLUGIN_SLOT_SHAPES_OK],
	              (unsigned long)v->stack[PLUGIN_SLOT_DECODE],
	              (unsigned long)v->stack[PLUGIN_SLOT_DRAW],
	              (unsigned long)v->stack[PLUGIN_SLOT_REPORT],
	              (unsigned long)v->stack[PLUGIN_SLOT_PARAM_SET],
	              (unsigned long)v->stack[PLUGIN_SLOT_PARAM_GET],
	              (unsigned long)cost) < 0)
		return -1;
	if (info_line(write, ctx,
	              "  decl  : own %lu/%lu/%lu/%lu/%lu/%lu/%lu  "
	              "at a crossing %s/%s/%s/%s/%s/%s/%s  sink %lu B\r\n",
	              (unsigned long)v->stack_own[PLUGIN_SLOT_ENTRY],
	              (unsigned long)v->stack_own[PLUGIN_SLOT_SHAPES_OK],
	              (unsigned long)v->stack_own[PLUGIN_SLOT_DECODE],
	              (unsigned long)v->stack_own[PLUGIN_SLOT_DRAW],
	              (unsigned long)v->stack_own[PLUGIN_SLOT_REPORT],
	              (unsigned long)v->stack_own[PLUGIN_SLOT_PARAM_SET],
	              (unsigned long)v->stack_own[PLUGIN_SLOT_PARAM_GET],
	              cross_text(x[0], v, PLUGIN_SLOT_ENTRY),
	              cross_text(x[1], v, PLUGIN_SLOT_SHAPES_OK),
	              cross_text(x[2], v, PLUGIN_SLOT_DECODE),
	              cross_text(x[3], v, PLUGIN_SLOT_DRAW),
	              cross_text(x[4], v, PLUGIN_SLOT_REPORT),
	              cross_text(x[5], v, PLUGIN_SLOT_PARAM_SET),
	              cross_text(x[6], v, PLUGIN_SLOT_PARAM_GET),
	              (unsigned long)v->stack_sink) < 0)
		return -1;
	return 0;
}
