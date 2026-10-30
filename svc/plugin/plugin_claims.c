/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    plugin_claims.c
 * @brief   The open model's container claims and their `nn info` lines.  See
 *          plugin_claims.h.
 *
 * No mutable storage: the store is the board's, and so is the critical section
 * around plugin_claims_set() / plugin_claims_get() (static inline, in the
 * header).
 */
#include "plugin_claims.h"

#include "nn_svc_adapter.h"   /* nn_info_line() */
#include "plugin_info.h"      /* plugin_info_stack() */

#include <stddef.h>

void plugin_claims_info(nn_svc_write_fn write, void *ctx,
                        enum plugin_claims_seen seen,
                        const struct plugin_claims *c, int running,
                        uint32_t res_base, uint32_t res_len,
                        uint32_t veneer_cost)
{
	const struct plugin_view *v;

	if (write == NULL)
		return;
	if (seen == PLUGIN_CLAIMS_LOADING) {
		(void)nn_info_line(write, ctx,
		                   "plugin  : (a model load is in progress -- ask "
		                   "again)\r\n");
		return;
	}
	if (seen != PLUGIN_CLAIMS_VALID || c == NULL) {
		/* Says so rather than leaving the line out: a missing line reads as a
		 * board with no plugin support at all, which is a different and wrong
		 * fact. */
		(void)nn_info_line(write, ctx,
		                   "plugin  : (none) -- reservation %lu B at 0x%08lx"
		                   "\r\n", (unsigned long)res_len,
		                   (unsigned long)res_base);
		return;
	}
	v = &c->view;
	/*
	 * [!] `running` IS THE LOADER'S OWN STATE, FROM THE SAME SNAPSHOT AS THE
	 * CLAIMS (issue #110): what the board's loader actually has branched into.
	 * "validated" is a container whose plugin was refused or never reached --
	 * and with no resident decoder that means nothing is reading its model.
	 */
	if (!v->has_plugin) {
		if (nn_info_line(write, ctx,
		                 "plugin  : (none in this container) -- reservation "
		                 "%lu B at 0x%08lx\r\n", (unsigned long)res_len,
		                 (unsigned long)res_base) < 0)
			return;
	} else if (nn_info_line(write, ctx,
	                        "plugin  : %s (build %s, crc %08lx), %s\r\n",
	                        v->name, v->build_id, (unsigned long)v->digest,
	                        running ? "running" : "validated, not loaded") < 0) {
		return;
	}
	/*
	 * Which bytes these claims came with -- for a model-only container too: on
	 * a board whose closing check is the CRC the load printed (f746g-disco),
	 * this line is where `nn info` repeats it.
	 */
	if (nn_info_line(write, ctx, "  from  : %s\r\n", c->from) < 0)
		return;
	if (!v->has_plugin)
		return;
	if (nn_info_line(write, ctx,
	                 "  image : %lu B file / %lu B mem (code %lu, data %lu, "
	                 "bss %lu), link 0x%08lx\r\n",
	                 (unsigned long)v->file_size, (unsigned long)v->mem_size,
	                 (unsigned long)v->code_len, (unsigned long)v->data_seg_len,
	                 (unsigned long)v->bss_len,
	                 (unsigned long)v->link_addr) < 0)
		return;
	/* What each slot needs at this firmware's c, and what the manifest
	 * declared -- compared with the call-site depths `nn stream stats`
	 * measures.  See plugin_info.h. */
	(void)plugin_info_stack(write, ctx, v, veneer_cost);
}
