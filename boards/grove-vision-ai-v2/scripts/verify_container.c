/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    verify_container.c
 * @brief   Run the DEVICE's container validator over a packed container, on the
 *          host (issue #101).
 *
 * [!] IT LINKS svc/plugin_load.c ITSELF.  That is the whole design, and it is
 * the same one verify_vela_model uses when it links the firmware's own
 * npu_payload.c: a host checker that reimplemented the walk could agree with
 * itself and disagree with the board, and the disagreement would surface as a
 * container that packs cleanly and is refused on the hardware -- after the
 * erase and the transfer have already been spent.
 *
 * Issue #93 is the reason this is not hypothetical.  Its host gate ran the
 * FlatBuffer verifier with default limits while the firmware ran it with its
 * own, so a model passed on the host and failed on the board.  One piece of
 * code, called from both sides, is what stops that shape of bug.
 *
 * The board's policy arrives on the command line rather than being baked in:
 * svc/ owns no board address, and neither does this.
 */
#include "plugin_load.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(void)
{
	fprintf(stderr,
	        "usage: verify_container <container> --target <id> --link <addr>\n"
	        "                        --capacity <bytes> [--stack <slot=bytes>]\n");
	exit(2);
}

int main(int argc, char **argv)
{
	struct plugin_policy pol;
	struct plugin_view view;
	const char *path = NULL;
	unsigned char *buf;
	long len;
	FILE *fh;
	enum plugin_result rc;
	int i;

	memset(&pol, 0, sizeof pol);
	pol.image_align = PLUGIN_IMAGE_ALIGN;
	pol.caps_supported = PLUGIN_CAP_KNOWN_MASK;
	for (i = 0; i < PLUGIN_SLOT_COUNT; i++)
		pol.stack_limit[i] = 0;

	for (i = 1; i < argc; i++) {
		if (argv[i][0] != '-' && path == NULL) {
			path = argv[i];
		} else if (!strcmp(argv[i], "--target") && i + 1 < argc) {
			pol.target_id = (uint32_t)strtoul(argv[++i], NULL, 0);
		} else if (!strcmp(argv[i], "--link") && i + 1 < argc) {
			pol.link_addr = (uint32_t)strtoul(argv[++i], NULL, 0);
		} else if (!strcmp(argv[i], "--capacity") && i + 1 < argc) {
			pol.capacity = (uint32_t)strtoul(argv[++i], NULL, 0);
		} else if (!strcmp(argv[i], "--stack") && i + 1 < argc) {
			/* slot index = bytes; the caller knows the enum. */
			char *eq = strchr(argv[++i], '=');
			if (eq == NULL)
				usage();
			*eq = '\0';
			int slot = atoi(argv[i]);
			if (slot < 0 || slot >= PLUGIN_SLOT_COUNT)
				usage();
			pol.stack_limit[slot] = (uint32_t)strtoul(eq + 1, NULL, 0);
		} else {
			usage();
		}
	}
	if (path == NULL || pol.capacity == 0u)
		usage();

	fh = fopen(path, "rb");
	if (fh == NULL) {
		fprintf(stderr, "verify_container: cannot open %s\n", path);
		return 1;
	}
	fseek(fh, 0, SEEK_END);
	len = ftell(fh);
	fseek(fh, 0, SEEK_SET);
	buf = malloc((size_t)len);
	if (buf == NULL || fread(buf, 1, (size_t)len, fh) != (size_t)len) {
		fprintf(stderr, "verify_container: cannot read %s\n", path);
		return 1;
	}
	fclose(fh);

	if (plugin_probe(buf, (size_t)len) != PLUGIN_KIND_CONTAINER) {
		fprintf(stderr, "verify_container: %s is not a container\n", path);
		return 1;
	}

	rc = plugin_parse(buf, (size_t)len, &pol, &view);
	if (rc != PLUGIN_OK) {
		fprintf(stderr, "verify_container: REFUSED -- %s\n",
		        plugin_result_name(rc));
		return 1;
	}

	fprintf(stderr,
	        "verify_container: OK  name=%s build=%s\n"
	        "  model  %u B at +%u\n"
	        "  plugin image %u B file / %u B mem at +%u (link 0x%08x)\n"
	        "  code %u B  data %u B  bss %u B  scratch %u B\n",
	        view.name, view.build_id,
	        view.model_len, view.model_off,
	        view.file_size, view.mem_size, view.image_off, view.link_addr,
	        view.code_len, view.data_seg_len, view.bss_len, view.scratch_len);
	for (i = 0; i < PLUGIN_SLOT_COUNT; i++)
		if (view.slot[i] != PLUGIN_SLOT_ABSENT)
			fprintf(stderr, "  slot %d at +0x%x, stack %u B\n",
			        i, view.slot[i] & ~1u, view.stack[i]);
	free(buf);
	return 0;
}
