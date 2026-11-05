/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    slot_table.c
 * @brief   Emit this board's blob slot table as JSON, for the asset build
 *          (issue #194; the same tool as wio-lite-ai's scripts/slot_table.c).
 *
 * The schema the other two boards emit -- {"unit", "slots": [{"index", "base",
 * "payload_max"}]} -- so cmake/build_asset.py reads ONE shape for every board
 * and none restates its geometry there.  The numbers come from the board's
 * table (src/blob_slots.c) through svc/blob/blob_map.c's lookups, linked into
 * this tool exactly as they are into the firmware.
 *
 * [!] THIS INCLUDES blob.h AND NEVER SCRAPES ANYTHING.  The interval and the
 * erase unit are src/blob.h's (the unit reaches QSPI_FLASH_SUBSECTOR_SIZE
 * through port/qspi/qspi_flash.h); compiled, it follows them by construction,
 * and the build tracks this tool's header closure with a compiler depfile.
 *
 * [!] AND IT REFUSES A TABLE THE STORE WOULD REFUSE: the same check the
 * firmware makes at every entry point (blob_check_map()), and a failure stops
 * the build.
 *
 * Why the build needs it: `blob write <name> <slot>` ERASES THE WHOLE SLOT
 * before the YMODEM size header arrives, so an oversized container is found
 * out on the device only after the slot's previous contents are gone.  The
 * asset build knows the packed size and the declared slot, so it can refuse
 * first -- if it is told the slot's capacity.  (Nothing on this board reads the
 * JSON until the asset is sent to a slot, issue #194 step 5.)
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
