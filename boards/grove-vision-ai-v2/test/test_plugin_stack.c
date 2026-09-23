/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * The runtime half of the plugin stack policy test (issue #119).  Built and run
 * by test_plugin_stack.py, several times, each with a different set of -D; do
 * not run it on its own.
 *
 * It does two things with the table port/npu/nn_plugin_stack.h hands the
 * firmware -- the same GROVE_PLUGIN_STACK_LIMITS initialiser nn_svc_grove.c
 * builds nn_plugin_policy from, not a copy of it:
 *
 *   - prints it, one `limit <slot> <bytes>` line per slot, so the driver can
 *     check WHICH allowance each slot got (and `runs <slot> <mask>`, the
 *     threads the header says it runs on).  Compiled with the two allowances set
 *     to different sentinel values, a slot wired to the wrong one shows up as
 *     the wrong number; with the real values (both 1,024) it could not.
 *   - runs svc/plugin_load.c -- the device's validator -- over a container that
 *     declares, slot by slot, exactly the limit (accepted) and one byte more
 *     (PLUGIN_ERR_STACK), and then the whole stack of each thread a slot runs
 *     on: the declaration issue #119 found ACCEPTED for decode.
 *
 * Only the stack table is the firmware's.  The target, prelink base, capacity
 * and veneer cost below are whatever lets the parse reach the stack check; they
 * are not under test here.  Every slot declares its own frames only (no
 * crossing), so what the loader requires of it is exactly what it declares.
 */
#include "nn_plugin_stack.h"
#include "plugin_load.h"
#include "crc32.h"

#include <stdio.h>
#include <string.h>

static const struct plugin_policy pol = {
	.target_id      = 0x9302u,
	.link_addr      = 0x341E0000u,
	.capacity       = 131072u,
	.image_align    = PLUGIN_IMAGE_ALIGN,
	.caps_supported = PLUGIN_CAP_KNOWN_MASK,
	.stack_limit    = GROVE_PLUGIN_STACK_LIMITS,
	.veneer_cost    = 256u,
	.stack_accounting = PLUGIN_STACK_ACCOUNTING,
};

/* ---- a container with all seven slots ------------------------------------ */

#define IMAGE_CODE     256u
#define IMAGE_DATA     64u
#define IMAGE_BSS      128u
#define IMAGE_SCRATCH  1024u
#define IMAGE_FILE     (IMAGE_CODE + IMAGE_DATA)
#define IMAGE_MEM      (IMAGE_FILE + IMAGE_BSS + IMAGE_SCRATCH)   /* 46 x 32 */

#define HDR_SZ         ((uint32_t)sizeof(struct plugin_container_hdr))
#define MAN_SZ         ((uint32_t)sizeof(struct plugin_manifest))
#define SECT_OFF       HDR_SZ
#define SECT_LEN       (MAN_SZ + IMAGE_FILE)
#define ALIGN_UP(v, a) (((v) + (a) - 1u) & ~((a) - 1u))
#define MODEL_OFF      ALIGN_UP(SECT_OFF + SECT_LEN, PLUGIN_MODEL_ALIGN)
#define MODEL_LEN      128u
#define TOTAL          (MODEL_OFF + MODEL_LEN)

static uint8_t buf[TOTAL];

#define HF(f)     ((uint32_t)offsetof(struct plugin_container_hdr, f))
#define SF(i, f)  (HF(sections) + (uint32_t)(i) * \
                   (uint32_t)sizeof(struct plugin_section) + \
                   (uint32_t)offsetof(struct plugin_section, f))
#define MF(f)     (SECT_OFF + (uint32_t)offsetof(struct plugin_manifest, f))

static void wr32(uint32_t at, uint32_t v)
{
	buf[at + 0u] = (uint8_t)(v & 0xFFu);
	buf[at + 1u] = (uint8_t)((v >> 8) & 0xFFu);
	buf[at + 2u] = (uint8_t)((v >> 16) & 0xFFu);
	buf[at + 3u] = (uint8_t)((v >> 24) & 0xFFu);
}

/* Every slot present and declaring 8 B, except the one a case sets. */
static void build(void)
{
	unsigned i;

	memset(buf, 0, sizeof buf);
	buf[0] = PLUGIN_CONTAINER_MAGIC0; buf[1] = PLUGIN_CONTAINER_MAGIC1;
	buf[2] = PLUGIN_CONTAINER_MAGIC2; buf[3] = PLUGIN_CONTAINER_MAGIC3;
	buf[4] = PLUGIN_CONTAINER_FORMAT0; buf[5] = PLUGIN_CONTAINER_FORMAT1;
	buf[6] = PLUGIN_CONTAINER_FORMAT2; buf[7] = PLUGIN_CONTAINER_FORMAT3;
	wr32(HF(hdr_size), HDR_SZ);
	wr32(HF(abi_version), PLUGIN_ABI_VERSION);
	wr32(HF(total_size), TOTAL);
	wr32(HF(section_count), 2u);
	wr32(SF(0, type), PLUGIN_SECTION_PLUGIN);
	wr32(SF(0, offset), SECT_OFF);
	wr32(SF(0, length), SECT_LEN);
	wr32(SF(1, type), PLUGIN_SECTION_MODEL);
	wr32(SF(1, offset), MODEL_OFF);
	wr32(SF(1, length), MODEL_LEN);

	buf[MF(magic) + 0u] = PLUGIN_MANIFEST_MAGIC0;
	buf[MF(magic) + 1u] = PLUGIN_MANIFEST_MAGIC1;
	buf[MF(magic) + 2u] = PLUGIN_MANIFEST_MAGIC2;
	buf[MF(magic) + 3u] = PLUGIN_MANIFEST_MAGIC3;
	wr32(MF(struct_size), MAN_SZ);
	wr32(MF(abi_version), PLUGIN_ABI_VERSION);
	wr32(MF(target_id), pol.target_id);
	wr32(MF(link_addr), pol.link_addr);
	wr32(MF(capability),
	     PLUGIN_CAP_DRAW | PLUGIN_CAP_REPORT | PLUGIN_CAP_PARAMS);
	wr32(MF(image_off), MAN_SZ);
	wr32(MF(file_size), IMAGE_FILE);
	wr32(MF(mem_size), IMAGE_MEM);
	wr32(MF(code_off), 0u);
	wr32(MF(code_len), IMAGE_CODE);
	wr32(MF(data_off), IMAGE_CODE);
	wr32(MF(data_len), IMAGE_DATA);
	wr32(MF(bss_off), IMAGE_FILE);
	wr32(MF(bss_len), IMAGE_BSS);
	wr32(MF(scratch_off), IMAGE_FILE + IMAGE_BSS);
	wr32(MF(scratch_len), IMAGE_SCRATCH);
	for (i = 0u; i < PLUGIN_SLOT_COUNT; i++) {
		wr32(MF(slot) + 4u * i, 0x20u * i + 1u);   /* Thumb bit set */
		wr32(MF(stack_own) + 4u * i, 8u);
	}
	wr32(MF(stack_accounting), PLUGIN_STACK_ACCOUNTING);
}

static enum plugin_result parse_with(unsigned slot, uint32_t declared)
{
	struct plugin_view view;

	build();
	wr32(MF(stack_own) + 4u * slot, declared);
	wr32(HF(plugin_digest), crc32_update(0u, buf + SECT_OFF, SECT_LEN));
	return plugin_parse(buf, sizeof buf, &pol, &view);
}

static int failures;

static void expect(unsigned slot, const char *what, uint32_t declared,
                   enum plugin_result want)
{
	enum plugin_result got = parse_with(slot, declared);

	if (got != want) {
		printf("  FAIL slot %u %-26s %5lu B -> %s, want %s\n", slot, what,
		       (unsigned long)declared, plugin_result_name(got),
		       plugin_result_name(want));
		failures++;
	} else {
		printf("  ok   slot %u %-26s %5lu B -> %s\n", slot, what,
		       (unsigned long)declared, plugin_result_name(got));
	}
}

int main(void)
{
	/* The stack of every thread each slot can run on (nn_plugin_stack.h). */
	static const uint32_t shell[] = { CLI_INSTANCE_STACK_SIZE,
	                                  CLI_BG_JOB_STACK_SIZE };
	unsigned i, k;

	for (i = 0u; i < PLUGIN_SLOT_COUNT; i++)
		printf("limit %u %lu\n", i, (unsigned long)pol.stack_limit[i]);
	/* And which threads the header says each slot runs on -- the masks the
	 * asserts are gated by and the stack report marks coverage with. */
	{
		static const unsigned runs[PLUGIN_SLOT_COUNT] = GROVE_PLUGIN_STACK_RUNS;

		for (i = 0u; i < PLUGIN_SLOT_COUNT; i++)
			printf("runs %u %u\n", i, runs[i]);
	}

	for (i = 0u; i < PLUGIN_SLOT_COUNT; i++) {
		uint32_t lim = pol.stack_limit[i];

		expect(i, "declares the limit", lim, PLUGIN_OK);
		expect(i, "declares one byte more", lim + 1u, PLUGIN_ERR_STACK);
		if (i == PLUGIN_SLOT_DRAW) {
			expect(i, "declares the panel stack",
			       CAM_PANEL_STACK_BYTES, PLUGIN_ERR_STACK);
			continue;
		}
		for (k = 0u; k < 2u; k++)
			expect(i, k == 0u ? "declares a console stack"
			                  : "declares a job stack",
			       shell[k], PLUGIN_ERR_STACK);
		if (i == PLUGIN_SLOT_DECODE)
			expect(i, "declares the producer stack",
			       CAM_PRODUCER_STACK_BYTES, PLUGIN_ERR_STACK);
	}

	if (failures) {
		printf("test_plugin_stack: %d FAILED\n", failures);
		return 1;
	}
	printf("test_plugin_stack: the policy refuses one byte past every limit\n");
	return 0;
}
