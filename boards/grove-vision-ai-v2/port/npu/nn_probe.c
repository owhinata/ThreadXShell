/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    nn_probe.c
 * @brief   The pure half of the plugin stack probe.  See nn_probe.h.
 *
 * No ThreadX, no register, no storage: what a sample means, how it is folded
 * into a record, and how a record is printed.  The half that reads the thread
 * and holds the records is nn_probe_rtos.c, and is kept as small as it can be,
 * because nothing but the board can run it.
 */
#include "nn_probe.h"

#include <stdarg.h>

#include "fmt.h"

enum nn_probe_ctx nn_probe_classify(
	const struct nn_probe_thread_class tab[NN_PROBE_CTX_COUNT],
	uint32_t prio, uint32_t stack)
{
	unsigned c;

	if (tab == NULL)
		return NN_PROBE_UNKNOWN;
	for (c = 0u; c < (unsigned)NN_PROBE_CTX_COUNT; c++)
		if (tab[c].prio == prio && tab[c].stack == stack)
			return (enum nn_probe_ctx)c;
	return NN_PROBE_UNKNOWN;
}

int nn_probe_measure(uintptr_t sp, uintptr_t lo, uint32_t size, uint32_t extra,
                     uint32_t *depth, uint32_t *left)
{
	uintptr_t hi;
	uint32_t d;

	if (depth == NULL || left == NULL)
		return NN_PROBE_BAD_STACK;
	if (size == 0u || lo > UINTPTR_MAX - (uintptr_t)size)
		return NN_PROBE_BAD_STACK;
	hi = lo + (uintptr_t)size;
	/* sp == hi is an empty stack, which a function body never has -- but it is
	 * inside, and the arithmetic below is right for it. */
	if (sp < lo || sp > hi)
		return NN_PROBE_OUT_OF_RANGE;

	d = (uint32_t)(hi - sp);
	d = (extra > UINT32_MAX - d) ? UINT32_MAX : d + extra;
	*depth = d;
	*left  = (d < size) ? size - d : 0u;
	return NN_PROBE_OK;
}

void nn_probe_record(struct nn_probe_row *r, unsigned ctx, uint32_t depth,
                     uint32_t left, uint32_t stack)
{
	struct nn_probe_cell *c;

	if (r == NULL)
		return;
	if (ctx >= (unsigned)NN_PROBE_CTX_COUNT) {
		nn_probe_reject(r);
		return;
	}
	c = &r->cell[ctx];
	/* The first observation sets both, whatever they are: a cell that has not
	 * been hit holds no value to compare against, and a 0 there must not win
	 * the minimum. */
	if (c->hits == 0u || depth > c->depth_hw)
		c->depth_hw = depth;
	if (c->hits == 0u || left < c->left_min) {
		c->left_min = left;
		c->stack    = stack;
	}
	if (c->hits != UINT32_MAX)
		c->hits++;
}

void nn_probe_reject(struct nn_probe_row *r)
{
	if (r != NULL && r->invalid != UINT32_MAX)
		r->invalid++;
}

/* ---- a sample that waits for its load to succeed ------------------------ */

void nn_probe_pending_arm(struct nn_probe_pending *p, const void *who)
{
	if (p == NULL)
		return;
	p->sp    = 0u;
	p->who   = who;
	p->takes = 0u;
	p->armed = 1u;
}

void nn_probe_pending_take(struct nn_probe_pending *p, const void *who,
                           uintptr_t sp)
{
	/* Not the loading thread's: not the branch.  A call when no load is in
	 * flight needs no test of its own -- arm() starts every load over and
	 * settle() answers nothing unarmed, so what it leaves is never read. */
	if (p == NULL || who != p->who)
		return;
	p->sp = sp;
	if (p->takes != UINT32_MAX)
		p->takes++;
}

enum nn_probe_settle nn_probe_pending_settle(struct nn_probe_pending *p,
                                             int load_ok, uintptr_t *sp)
{
	int armed;

	if (p == NULL)
		return NN_PROBE_SETTLE_NONE;
	armed    = p->armed != 0u;
	p->armed = 0u;
	if (!armed || !load_ok)
		return NN_PROBE_SETTLE_NONE;
	/*
	 * [!] A SUCCESSFUL LOAD WITHOUT EXACTLY ONE SAMPLE IS REPORTED, NOT
	 * SKIPPED.  Zero means the loader stopped calling the hook before entry();
	 * two mean it calls it somewhere else as well, and which of them stood
	 * beside the branch cannot be told.  Either is the probe no longer
	 * measuring what it says, and a count of invalid samples is how the report
	 * says so -- silence would read as "entry was not exercised".
	 */
	if (p->takes != 1u)
		return NN_PROBE_SETTLE_REJECT;
	if (sp != NULL)
		*sp = p->sp;
	return NN_PROBE_SETTLE_RECORD;
}

/* ---- the report line ----------------------------------------------------- */

static const char *const nn_probe_names[NN_PROBE_CTX_COUNT] = {
	[NN_PROBE_PRODUCER] = "prod",
	[NN_PROBE_PANEL]    = "panel",
	[NN_PROBE_CONSOLE]  = "con",
	[NN_PROBE_BG]       = "bg",
};

/* Append to buf at *at, never past cap - 1, always terminated. */
static void nn_probe_put(char *buf, size_t cap, size_t *at, const char *fmt,
                         ...)
{
	va_list ap;

	if (*at + 1u >= cap)
		return;
	va_start(ap, fmt);
	*at += (size_t)fmt_vsnformat(buf + *at, cap - *at, fmt, ap);
	va_end(ap);
}

int nn_probe_line(char *buf, size_t cap, const char *label,
                  const struct nn_probe_row *r, unsigned runs,
                  const char *note)
{
	size_t at = 0u;
	uint32_t least = 0u;
	int seen = 0;
	unsigned c;

	if (buf == NULL || cap == 0u)
		return 0;
	buf[0] = '\0';
	nn_probe_put(buf, cap, &at, "%-9s:", label != NULL ? label : "?");
	if (r == NULL) {
		nn_probe_put(buf, cap, &at, " not measured");
		return (int)at;
	}
	for (c = 0u; c < (unsigned)NN_PROBE_CTX_COUNT; c++) {
		const struct nn_probe_cell *k = &r->cell[c];
		int expected = (runs & (1u << c)) != 0u;

		if (k->hits == 0u) {
			if (expected)
				nn_probe_put(buf, cap, &at, " %s --", nn_probe_names[c]);
			continue;
		}
		/* [!] SEEN WHERE IT IS NOT SUPPOSED TO RUN.  Shown, flagged, and
		 * folded into the minimum like any other: the table is what is
		 * wrong, and the depth is still a depth. */
		nn_probe_put(buf, cap, &at, " %s%s %lu/%lu", expected ? "" : "!",
		             nn_probe_names[c], (unsigned long)k->depth_hw,
		             (unsigned long)k->stack);
		if (!seen || k->left_min < least)
			least = k->left_min;
		seen = 1;
	}
	if (seen)
		nn_probe_put(buf, cap, &at, "; left %lu", (unsigned long)least);
	else
		nn_probe_put(buf, cap, &at, "; not measured");
	/* Last, because it is the one field with no bound on its width: a count
	 * past five digits pushes the end of the line off the caller's 96 B, and
	 * what is lost then is only the tail of the count. */
	if (note != NULL)
		nn_probe_put(buf, cap, &at, " %s", note);
	if (r->invalid != 0u)
		nn_probe_put(buf, cap, &at, " inv %lu", (unsigned long)r->invalid);
	return (int)at;
}
