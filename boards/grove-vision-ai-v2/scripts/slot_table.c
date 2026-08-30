/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    slot_table.c
 * @brief   Emit the blob slot table as JSON, for the sender (issue #101).
 *
 * [!] THE SENDER MUST NOT SCRAPE blob_map.c WITH A REGULAR EXPRESSION.  It needs
 * one number -- how many payload bytes the slot the operator named can hold --
 * and the only correct source is the map the firmware uses, through the same
 * accessors, with the same payload arithmetic (the header unit is subtracted,
 * which a text scan of the array would silently miss).
 *
 * Why the sender needs it at all: the DEVICE chooses nothing here.  `blob write`
 * picks a target and ERASES THE WHOLE SLOT before the YMODEM size header
 * arrives, so an oversized transfer is discovered after ~40 s of erase with the
 * old contents already gone.  The only place a container can be refused for
 * being too large is the host, before the transfer starts -- and the host can
 * only do that if it is told which slot.
 */
#include "blob_map.h"

#include <stdio.h>
#include <stdlib.h>

/*
 * The erase unit is passed in rather than included from anywhere.  It is a
 * MEASURED property of the part (cmake/flash_geometry.cmake owns it as a
 * non-cache variable, and a disagreeing -D is a configure-time FATAL_ERROR), so
 * restating it here would create a second declaration of a number whose whole
 * point is that it has exactly one.
 */
int main(int argc, char **argv)
{
	unsigned n = blob_map_count(), i;
	uint32_t unit;

	if (argc != 2) {
		fprintf(stderr, "usage: slot_table <erase-unit>\n");
		return 2;
	}
	unit = (uint32_t)strtoul(argv[1], NULL, 0);

	printf("{\n  \"unit\": %u,\n  \"slots\": [\n", (unsigned)unit);
	for (i = 0u; i < n; i++) {
		const struct blob_slot *s = blob_map_slot(i);

		printf("    {\"index\": %u, \"base\": %u, \"payload_max\": %u}%s\n",
		       i, (unsigned)s->base,
		       (unsigned)blob_map_payload_max(s, unit),
		       i + 1u < n ? "," : "");
	}
	printf("  ]\n}\n");
	return 0;
}
