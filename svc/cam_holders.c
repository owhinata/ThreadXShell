/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    cam_holders.c
 * @brief   The camera's holder table (issue #142).  See cam_holders.h.
 *
 * A pure function over a table the caller owns: this TU holds no storage of
 * its own (svc/, audited per board by cmake/shared_storage_gate.cmake).
 */
#include "cam_holders.h"

#include <stddef.h>

/* add_or_start() registers two holders on an empty table, and the counters
 * are unsigned char. */
_Static_assert(CAM_HOLDERS_MAX >= 2, "add_or_start needs two slots");
_Static_assert(CAM_HOLDERS_MAX <= 255, "counters are unsigned char");

static int well_formed(const struct cam_holders *t)
{
	return t != NULL && t->n <= CAM_HOLDERS_MAX &&
	       t->claim_base <= t->n && t->claimed <= 1u;
}

static int holder_ok(const struct cam_holder *h, unsigned attr)
{
	return h != NULL && h->name != NULL &&
	       (attr & ~CAM_HOLDER_ATTRS_KNOWN) == 0u;
}

static int find(const struct cam_holders *t, const struct cam_holder *h)
{
	unsigned i;

	for (i = 0; i < t->n; i++)
		if (t->who[i] == h)
			return (int)i;
	return -1;
}

static void append(struct cam_holders *t, const struct cam_holder *h,
                   unsigned attr)
{
	t->who[t->n] = h;
	t->attr[t->n] = (unsigned char)attr;
	t->n++;
}

/* The first registration on an empty table starts the camera, and the table
 * stays claimed until the board says how that went. */
static int start_claim(struct cam_holders *t, unsigned base)
{
	t->claimed = 1u;
	t->claim_base = (unsigned char)base;
	return CAM_HOLDERS_T_START;
}

void cam_holders_init(struct cam_holders *t)
{
	unsigned i;

	for (i = 0; i < CAM_HOLDERS_MAX; i++) {
		t->who[i] = NULL;
		t->attr[i] = 0u;
	}
	t->n = 0u;
	t->claimed = 0u;
	t->claim_base = 0u;
}

int cam_holders_add(struct cam_holders *t, const struct cam_holder *h,
                    unsigned attr)
{
	int at;

	if (!well_formed(t) || !holder_ok(h, attr))
		return CAM_HOLDERS_E_INVAL;
	if (t->claimed)
		return CAM_HOLDERS_E_CLAIMED;
	at = find(t, h);
	if (at >= 0)
		return t->attr[at] == attr ? CAM_HOLDERS_T_NONE
		                           : CAM_HOLDERS_E_INVAL;
	if (t->n >= CAM_HOLDERS_MAX)
		return CAM_HOLDERS_E_FULL;
	if (t->n == 0u) {
		append(t, h, attr);
		return start_claim(t, 0u);
	}
	append(t, h, attr);
	return CAM_HOLDERS_T_NONE;
}

int cam_holders_add_or_start(struct cam_holders *t, const struct cam_holder *h,
                             unsigned attr, const struct cam_holder *starter,
                             unsigned starter_attr, unsigned *added)
{
	int at;

	if (added == NULL)
		return CAM_HOLDERS_E_INVAL;
	*added = 0u;
	if (!well_formed(t) || !holder_ok(h, attr) ||
	    !holder_ok(starter, starter_attr) || h == starter)
		return CAM_HOLDERS_E_INVAL;
	if (t->claimed)
		return CAM_HOLDERS_E_CLAIMED;

	if (t->n == 0u) {
		/* Stopped: the starter first, as if started by hand.  Both slots
		 * exist (static assert), so this cannot stop half-way. */
		append(t, starter, starter_attr);
		append(t, h, attr);
		*added = CAM_HOLDERS_ADDED_STARTER | CAM_HOLDERS_ADDED_SELF;
		return start_claim(t, 0u);
	}

	/* Streaming: join it.  The starter is whoever already holds it. */
	at = find(t, h);
	if (at >= 0)
		return t->attr[at] == attr ? CAM_HOLDERS_T_NONE
		                           : CAM_HOLDERS_E_INVAL;
	if (t->n >= CAM_HOLDERS_MAX)
		return CAM_HOLDERS_E_FULL;
	append(t, h, attr);
	*added = CAM_HOLDERS_ADDED_SELF;
	return CAM_HOLDERS_T_NONE;
}

int cam_holders_remove(struct cam_holders *t, const struct cam_holder *h)
{
	unsigned i;
	int at;

	if (!well_formed(t) || h == NULL)
		return CAM_HOLDERS_E_INVAL;
	if (t->claimed)
		return CAM_HOLDERS_E_CLAIMED;
	at = find(t, h);
	if (at < 0)
		return CAM_HOLDERS_T_NONE;

	/* [!] D8: a holder that keeps the stream does not leave while one that
	 * needs it is still there -- and it is refused, not deferred. */
	if (t->attr[at] & CAM_HOLDER_KEEPS_STREAM)
		for (i = 0; i < t->n; i++)
			if (!(t->attr[i] & CAM_HOLDER_KEEPS_STREAM))
				return CAM_HOLDERS_E_IN_USE;

	/* Close the gap so the list stays in registration order. */
	for (i = (unsigned)at; i + 1u < t->n; i++) {
		t->who[i] = t->who[i + 1u];
		t->attr[i] = t->attr[i + 1u];
	}
	t->n--;
	t->who[t->n] = NULL;
	t->attr[t->n] = 0u;
	return t->n == 0u ? CAM_HOLDERS_T_STOP : CAM_HOLDERS_T_NONE;
}

int cam_holders_commit(struct cam_holders *t)
{
	if (!well_formed(t))
		return CAM_HOLDERS_E_INVAL;
	if (!t->claimed)
		return CAM_HOLDERS_E_NOT_CLAIMED;
	t->claimed = 0u;
	t->claim_base = 0u;
	return CAM_HOLDERS_T_NONE;
}

int cam_holders_abort(struct cam_holders *t)
{
	unsigned removed;

	if (!well_formed(t))
		return CAM_HOLDERS_E_INVAL;
	if (!t->claimed)
		return CAM_HOLDERS_E_NOT_CLAIMED;
	/* Nothing else could change the table while it was claimed, so the
	 * claiming registration's entries are exactly the ones past the base. */
	removed = t->n - t->claim_base;
	while (t->n > t->claim_base) {
		t->n--;
		t->who[t->n] = NULL;
		t->attr[t->n] = 0u;
	}
	t->claimed = 0u;
	t->claim_base = 0u;
	return (int)removed;
}

unsigned cam_holders_count(const struct cam_holders *t)
{
	return well_formed(t) ? t->n : 0u;
}

int cam_holders_claimed(const struct cam_holders *t)
{
	return well_formed(t) && t->claimed;
}

int cam_holders_has(const struct cam_holders *t, const struct cam_holder *h)
{
	return well_formed(t) && h != NULL && find(t, h) >= 0;
}

unsigned cam_holders_list(const struct cam_holders *t,
                          const struct cam_holder **out, unsigned char *attr,
                          unsigned max)
{
	unsigned i;

	if (!well_formed(t) || out == NULL)
		return 0u;
	for (i = 0; i < t->n && i < max; i++) {
		out[i] = t->who[i];
		if (attr != NULL)
			attr[i] = t->attr[i];
	}
	return i;
}

int cam_holders_disp_of(int rc)
{
	if (rc >= 0)
		return CAM_HOLDERS_DISP_DONE;
	switch (rc) {
	case CAM_HOLDERS_E_CLAIMED:
		return CAM_HOLDERS_DISP_RETRYABLE;
	case CAM_HOLDERS_E_INVAL:
	case CAM_HOLDERS_E_FULL:
	case CAM_HOLDERS_E_IN_USE:
	case CAM_HOLDERS_E_NOT_CLAIMED:
	default:
		return CAM_HOLDERS_DISP_TERMINAL;
	}
}
