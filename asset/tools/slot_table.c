/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    slot_table.c
 * @brief   Emit a board's blob slot table as JSON, for the asset build -- one
 *          tool for every board (issue #195; until then each board had its own
 *          scripts/slot_table.c, from issues #101, #108 and #194).
 *
 *   slot_table <out.json>
 *
 * The schema is {"unit", "slots": [{"index", "base", "payload_max"}]}, so
 * cmake/build_asset.py reads ONE shape for every board and none restates its
 * geometry there.  The numbers are the board's table (src/blob_slots.c)
 * through svc/blob/blob_map.c's lookups, linked into this tool exactly as they
 * are into the firmware.
 *
 * [!] THE INTERVAL AND THE ERASE UNIT ARE COMPILED IN, FROM THE FIRMWARE'S OWN
 * DECLARATIONS -- never an argument, never scraped.  The board's build rule
 * force-includes the header the firmware's store reads them from and names its
 * three macros:
 *
 *   -include <header>  -DSLOT_TABLE_LO=<macro> -DSLOT_TABLE_HI=<macro>
 *   -DSLOT_TABLE_UNIT=<macro>
 *
 * (wio-lite-ai and f746g-disco: src/blob.h's BLOB_REGION_BASE / _END /
 * BLOB_ERASE_UNIT; grove-vision-ai-v2: port/sdk_seam/nor_seam.h's
 * NOR_WRITABLE_LO / _HI / NOR_ERASE_UNIT, over the same board.cmake values the
 * firmware is compiled with.)  The rule tracks this tool's header closure with
 * a compiler depfile, so a change the firmware recompiles against also
 * regenerates the table.  Until #195 Grove's tool took the erase unit as a
 * run-time argument and checked nothing.
 *
 * [!] AND IT REFUSES A TABLE THE STORE WOULD REFUSE.  The firmware checks the
 * table against the interval at every entry point (blob_check_map()) and does
 * nothing with one that fails; a JSON describing such a table would let an
 * asset be built for slots no `blob write` can reach.  So the same check runs
 * here, BEFORE anything is written, and a refusal removes any JSON an earlier
 * run left at that path and exits non-zero, which stops the build.  The file
 * is written beside its final path and renamed into place, so an interrupted
 * or failed write leaves no partial JSON either.
 *
 * Why the build needs it at all: `blob write <name> <slot>` ERASES THE WHOLE
 * SLOT before the YMODEM size header arrives, so an oversized container is
 * found out on the device only after the slot's previous contents are gone and
 * an erase has been spent.  The asset build knows the packed size and the
 * declared slot, so it can refuse first -- if it is told the slot's capacity.
 */
#include "blob_map.h"

#include <stdio.h>
#include <string.h>

#if !defined(SLOT_TABLE_LO) || !defined(SLOT_TABLE_HI) || \
    !defined(SLOT_TABLE_UNIT)
#error "the board's build rule names the interval and the erase unit: -include <header> -DSLOT_TABLE_LO= -DSLOT_TABLE_HI= -DSLOT_TABLE_UNIT="
#endif

static int write_table(FILE *f, unsigned n)
{
	unsigned i;

	if (fprintf(f, "{\n  \"unit\": %u,\n  \"slots\": [\n",
	            (unsigned)(SLOT_TABLE_UNIT)) < 0)
		return -1;
	for (i = 0u; i < n; i++) {
		const struct blob_slot *s = blob_map_slot(i);

		if (fprintf(f, "    {\"index\": %u, \"base\": %u, "
		               "\"payload_max\": %u}%s\n",
		            i, (unsigned)s->base,
		            (unsigned)blob_map_payload_max(s, SLOT_TABLE_UNIT),
		            i + 1u < n ? "," : "") < 0)
			return -1;
	}
	return fprintf(f, "  ]\n}\n") < 0 ? -1 : 0;
}

int main(int argc, char **argv)
{
	unsigned n = blob_map_count(), bad = 0u;
	enum blob_map_verdict v;
	char tmp[4096];
	FILE *f;
	int rc;

	if (argc != 2 || strlen(argv[1]) + 5u > sizeof tmp) {
		fprintf(stderr, "usage: slot_table <out.json>\n");
		return 2;
	}

	v = blob_map_check(SLOT_TABLE_LO, SLOT_TABLE_HI, SLOT_TABLE_UNIT,
	                   blob_map_table(), n, &bad);
	if (v != BLOB_MAP_OK) {
		if (blob_map_verdict_names_slot(v))
			fprintf(stderr, "slot_table: the table is not usable: %s "
			                "(slot %u); no JSON written\n",
			        blob_map_verdict_name(v), bad);
		else
			fprintf(stderr, "slot_table: the table is not usable: %s; "
			                "no JSON written\n", blob_map_verdict_name(v));
		(void)remove(argv[1]);
		return 1;
	}

	(void)snprintf(tmp, sizeof tmp, "%s.tmp", argv[1]);
	f = fopen(tmp, "w");
	if (f == NULL) {
		perror(tmp);
		return 1;
	}
	rc = write_table(f, n);
	if (fclose(f) != 0)
		rc = -1;
	if (rc != 0) {
		fprintf(stderr, "slot_table: could not write %s\n", tmp);
		(void)remove(tmp);
		return 1;
	}
	if (rename(tmp, argv[1]) != 0) {
		perror(argv[1]);
		(void)remove(tmp);
		return 1;
	}
	return 0;
}
