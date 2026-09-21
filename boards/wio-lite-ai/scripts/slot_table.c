/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    slot_table.c
 * @brief   Emit this board's blob slot table as JSON, for the asset build
 *          (issue #108 = #78 Step 3a).
 *
 * The same schema Grove's scripts/slot_table.c emits -- {"unit", "slots":
 * [{"index", "base", "payload_max"}]} -- so cmake/build_asset.py reads ONE shape
 * for both boards and neither restates its geometry there.  What differs is only
 * where the numbers come from: Grove carves an irregular table in blob_map.c;
 * this board has six equal slots declared in src/blob.h.
 *
 * [!] THIS INCLUDES blob.h AND NEVER SCRAPES IT.  BLOB_PAYLOAD_MAX reaches
 * NOR_SECTOR_SIZE through nor_flash.h, and a text scan of one header would stop
 * following the moment that chain changed.  Compiled, it follows by
 * construction; and the build tracks this tool's full header closure with a
 * compiler depfile, so a change the firmware recompiles against also regenerates
 * the table rather than leaving a stale one to answer for it.
 *
 * Why the build needs it at all: `blob write <slot>` ERASES THE WHOLE SLOT
 * before the YMODEM size header arrives, so an oversized container is found out
 * on the device only after the slot's previous contents are gone and an erase
 * has been spent.  The asset build knows the packed size and the declared slot,
 * so it can refuse first -- if it is told the slot's capacity.
 */
#include "blob.h"

#include <stdio.h>

int main(void)
{
	unsigned i;

	printf("{\n  \"unit\": %u,\n  \"slots\": [\n", (unsigned)NOR_SECTOR_SIZE);
	for (i = 0u; i < BLOB_SLOT_COUNT; i++)
		printf("    {\"index\": %u, \"base\": %u, \"payload_max\": %u}%s\n",
		       i, (unsigned)blob_slot_addr(i), (unsigned)BLOB_PAYLOAD_MAX,
		       i + 1u < BLOB_SLOT_COUNT ? "," : "");
	printf("  ]\n}\n");
	return 0;
}
