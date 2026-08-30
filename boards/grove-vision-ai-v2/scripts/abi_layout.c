/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    abi_layout.c
 * @brief   Emit svc/plugin_abi.h's wire layout as JSON, for the packer.
 *
 * [!] THE PACKER DOES NOT MIRROR THE FORMAT, IT IS TOLD IT.  A Python struct
 * format string transcribed from the header is a second declaration of the ABI,
 * and the day someone inserts a field the two disagree -- silently, because both
 * sides still produce and consume something.  The device would refuse every
 * container with a size mismatch if it were lucky, and misread one if it were
 * not.
 *
 * So the offsets come from the compiler, out of the same header the firmware
 * includes.  This is the same reasoning that makes verify_vela_model link the
 * firmware's own npu_payload.c rather than reimplement the payload walk, and
 * that makes the blob slot capacity table generated rather than scraped.
 *
 * Built and run on the HOST.  It reads no board headers and depends on nothing
 * but plugin_abi.h, which is board-independent by construction.
 */
#include "plugin_abi.h"

#include <stdio.h>

#define F(st, field) \
	printf("    \"%s\": %u%s\n", #field, \
	       (unsigned)offsetof(struct st, field), ",")

int main(void)
{
	printf("{\n");
	printf("  \"abi_version\": %u,\n", (unsigned)PLUGIN_ABI_VERSION);
	printf("  \"magic\": \"%c%c%c%c\",\n",
	       PLUGIN_CONTAINER_MAGIC0, PLUGIN_CONTAINER_MAGIC1,
	       PLUGIN_CONTAINER_MAGIC2, PLUGIN_CONTAINER_MAGIC3);
	printf("  \"format\": \"%c%c%c%c\",\n",
	       PLUGIN_CONTAINER_FORMAT0, PLUGIN_CONTAINER_FORMAT1,
	       PLUGIN_CONTAINER_FORMAT2, PLUGIN_CONTAINER_FORMAT3);
	printf("  \"manifest_magic\": \"%c%c%c%c\",\n",
	       PLUGIN_MANIFEST_MAGIC0, PLUGIN_MANIFEST_MAGIC1,
	       PLUGIN_MANIFEST_MAGIC2, PLUGIN_MANIFEST_MAGIC3);
	printf("  \"hdr_size\": %u,\n",
	       (unsigned)sizeof(struct plugin_container_hdr));
	printf("  \"section_size\": %u,\n", (unsigned)sizeof(struct plugin_section));
	printf("  \"section_max\": %u,\n", (unsigned)PLUGIN_SECTION_MAX);
	printf("  \"manifest_size\": %u,\n", (unsigned)sizeof(struct plugin_manifest));
	printf("  \"slot_count\": %u,\n", (unsigned)PLUGIN_SLOT_COUNT);
	printf("  \"name_max\": %u,\n", (unsigned)PLUGIN_NAME_MAX);
	printf("  \"build_id_max\": %u,\n", (unsigned)PLUGIN_BUILD_ID_MAX);
	printf("  \"model_align\": %u,\n", (unsigned)PLUGIN_MODEL_ALIGN);
	printf("  \"image_align\": %u,\n", (unsigned)PLUGIN_IMAGE_ALIGN);
	printf("  \"slot_absent\": %u,\n", (unsigned)PLUGIN_SLOT_ABSENT);

	printf("  \"section_type\": {\"model\": %u, \"plugin\": %u, \"data\": %u},\n",
	       (unsigned)PLUGIN_SECTION_MODEL, (unsigned)PLUGIN_SECTION_PLUGIN,
	       (unsigned)PLUGIN_SECTION_DATA);
	printf("  \"slot\": {\"entry\": %u, \"shapes_ok\": %u, \"decode\": %u, "
	       "\"draw\": %u, \"report\": %u, \"param_set\": %u, "
	       "\"param_get\": %u},\n",
	       (unsigned)PLUGIN_SLOT_ENTRY, (unsigned)PLUGIN_SLOT_SHAPES_OK,
	       (unsigned)PLUGIN_SLOT_DECODE, (unsigned)PLUGIN_SLOT_DRAW,
	       (unsigned)PLUGIN_SLOT_REPORT, (unsigned)PLUGIN_SLOT_PARAM_SET,
	       (unsigned)PLUGIN_SLOT_PARAM_GET);
	printf("  \"cap\": {\"draw\": %u, \"report\": %u, \"params\": %u},\n",
	       (unsigned)PLUGIN_CAP_DRAW, (unsigned)PLUGIN_CAP_REPORT,
	       (unsigned)PLUGIN_CAP_PARAMS);

	printf("  \"hdr\": {\n");
	F(plugin_container_hdr, magic);
	F(plugin_container_hdr, format);
	F(plugin_container_hdr, hdr_size);
	F(plugin_container_hdr, abi_version);
	F(plugin_container_hdr, total_size);
	F(plugin_container_hdr, section_count);
	F(plugin_container_hdr, plugin_digest);
	F(plugin_container_hdr, reserved);
	printf("    \"sections\": %u\n",
	       (unsigned)offsetof(struct plugin_container_hdr, sections));
	printf("  },\n");

	printf("  \"section\": {\n");
	F(plugin_section, type);
	F(plugin_section, offset);
	F(plugin_section, length);
	printf("    \"reserved\": %u\n",
	       (unsigned)offsetof(struct plugin_section, reserved));
	printf("  },\n");

	printf("  \"manifest\": {\n");
	F(plugin_manifest, magic);
	F(plugin_manifest, struct_size);
	F(plugin_manifest, abi_version);
	F(plugin_manifest, target_id);
	F(plugin_manifest, link_addr);
	F(plugin_manifest, capability);
	F(plugin_manifest, image_off);
	F(plugin_manifest, file_size);
	F(plugin_manifest, mem_size);
	F(plugin_manifest, code_off);
	F(plugin_manifest, code_len);
	F(plugin_manifest, data_off);
	F(plugin_manifest, data_len);
	F(plugin_manifest, bss_off);
	F(plugin_manifest, bss_len);
	F(plugin_manifest, scratch_off);
	F(plugin_manifest, scratch_len);
	F(plugin_manifest, slot);
	F(plugin_manifest, stack);
	F(plugin_manifest, name);
	printf("    \"build_id\": %u\n",
	       (unsigned)offsetof(struct plugin_manifest, build_id));
	printf("  }\n");
	printf("}\n");
	return 0;
}
