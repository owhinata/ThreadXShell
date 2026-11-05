/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    slot_table.c
 * @brief   Emit this board's blob slot table as JSON, for the asset build
 *          (issue #108 = #78 Step 3a; from the shared table lookups since
 *          issue #193).
 *
 * The same schema grove-vision-ai-v2's scripts/slot_table.c emits -- {"unit",
 * "slots": [{"index", "base", "payload_max"}]} -- so cmake/build_asset.py reads
 * ONE shape for both boards and neither restates its geometry there.  Since
 * issue #193 the numbers come from the same place on both: the board's table
 * (src/blob_slots.c) through svc/blob/blob_map.c's lookups, linked into this
 * tool exactly as they are into the firmware.
 *
 * [!] THIS INCLUDES blob.h AND NEVER SCRAPES ANYTHING.  The interval and the
 * erase unit are src/blob.h's (the unit reaches NOR_SECTOR_SIZE through
 * nor_flash.h), and a text scan of a header or of the table would stop
 * following the moment either changed.  Compiled, it follows by construction;
 * and the build tracks this tool's header closure with a compiler depfile, so a
 * change the firmware recompiles against also regenerates the table.
 *
 * [!] AND IT REFUSES A TABLE THE STORE WOULD REFUSE.  The firmware checks the
 * table against the interval at every entry point (blob_check_map()) and does
 * nothing with one that fails; a JSON describing such a table would let an
 * asset be built for slots no `blob write` can reach.  So the same check runs
 * here and a failure stops the build.
 *
 * Why the build needs it at all: `blob write <name> <slot>` ERASES THE WHOLE
 * SLOT before the YMODEM size header arrives, so an oversized container is
 * found out on the device only after the slot's previous contents are gone and
 * an erase has been spent.  The asset build knows the packed size and the
 * declared slot, so it can refuse first -- if it is told the slot's capacity.
 */
#include "blob.h"
#include "blob_map.h"

#include <stdio.h>

int main(void)
{
	unsigned n = blob_map_count(), i, bad = 0u;
	enum blob_map_verdict v;

	v = blob_map_check(BLOB_REGION_BASE, BLOB_REGION_END, BLOB_ERASE_UNIT,
	                   blob_map_table(), n, &bad);
	if (v != BLOB_MAP_OK) {
		if (blob_map_verdict_names_slot(v))
			fprintf(stderr, "slot_table: the table is not usable: %s "
			                "(slot %u)\n", blob_map_verdict_name(v), bad);
		else
			fprintf(stderr, "slot_table: the table is not usable: %s\n",
			        blob_map_verdict_name(v));
		return 1;
	}

	printf("{\n  \"unit\": %u,\n  \"slots\": [\n", (unsigned)BLOB_ERASE_UNIT);
	for (i = 0u; i < n; i++) {
		const struct blob_slot *s = blob_map_slot(i);

		printf("    {\"index\": %u, \"base\": %u, \"payload_max\": %u}%s\n",
		       i, (unsigned)s->base,
		       (unsigned)blob_map_payload_max(s, BLOB_ERASE_UNIT),
		       i + 1u < n ? "," : "");
	}
	printf("  ]\n}\n");
	return 0;
}
