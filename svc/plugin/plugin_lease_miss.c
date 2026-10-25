/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    plugin_lease_miss.c
 * @brief   The lease's miss accounting.  See plugin_lease_miss.h.
 *
 * A pure function over a state the caller owns: this TU holds no storage of
 * its own (svc/, audited per board by cmake/shared_storage_gate.cmake).
 */
#include "plugin_lease_miss.h"

#include <stddef.h>

void plugin_lease_miss_note(struct plugin_lease_miss *m,
                            enum plugin_lease_who who, int held)
{
	if (!held) {
		m->total++;
		m->run++;
		if (m->run > m->worst)
			m->worst = m->run;
		return;
	}
	/* [!] Only an annotated frame ends a run -- see the header. */
	if (who == PLUGIN_LEASE_PANEL)
		m->run = 0u;
}

void plugin_lease_miss_read(const struct plugin_lease_miss *m,
                            uint32_t *total, uint32_t *worst_run)
{
	if (total != NULL)
		*total = m->total;
	if (worst_run != NULL)
		*worst_run = m->worst;
}

void plugin_lease_miss_reset(struct plugin_lease_miss *m)
{
	m->total = 0u;
	m->run   = 0u;
	m->worst = 0u;
}
