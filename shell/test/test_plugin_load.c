/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Host unit test for svc/plugin_load.c -- container and manifest validation
 * (issue #101 = #78 Step 1a).
 *
 * WHY THIS FILE CARRIES SO MANY CASES.  plugin_load.h names thirty-one distinct
 * refusals on purpose: "the container is malformed" is true of every failure and
 * tells whoever is holding the board nothing, and several of these are ordinary
 * operator mistakes (a container built for another board, a stale ABI) that must
 * not read like corruption.  A reason that no input can reach is not a reason,
 * so every one of them is provoked here from a container that is valid up to the
 * single field the case mutates.
 *
 * [!] THE MUTATION IS ALWAYS ONE FIELD OF AN OTHERWISE VALID CONTAINER.  A test
 * that hand-rolled a broken buffer could be refused for a reason it did not
 * intend -- an earlier check firing first -- and would then pass while proving
 * nothing about the check it was named after.  build() produces a container that
 * parses cleanly; each case changes one thing and asserts the exact code.
 *
 * [!] AND THE ORDER OF CHECKS IS PART OF THE CONTRACT.  The digest covers the
 * whole plugin section, so any mutation inside that section also breaks the CRC.
 * Every case that mutates the manifest re-stamps the digest afterwards, which is
 * what makes the assertion "this field was rejected" rather than "something in
 * there was wrong".  The two digest cases do the opposite deliberately.
 */
#include "plugin_abi.h"
#include "plugin_load.h"
#include "crc32.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

/* ---- a container builder ------------------------------------------------- */

#define IMAGE_CODE      256u
#define IMAGE_DATA      64u
#define IMAGE_BSS       128u
#define IMAGE_SCRATCH   1024u
#define IMAGE_FILE      (IMAGE_CODE + IMAGE_DATA)
/* mem_size must be a multiple of the image alignment -- the far end of the
 * reservation matters as much as the near one, because cache maintenance rounds
 * outward.  1472 = 46 * 32. */
#define IMAGE_MEM       (IMAGE_CODE + IMAGE_DATA + IMAGE_BSS + IMAGE_SCRATCH)

#define MODEL_LEN       128u
#define DATA_LEN        48u

#define HDR_SZ          ((uint32_t)sizeof(struct plugin_container_hdr))
#define MAN_SZ          ((uint32_t)sizeof(struct plugin_manifest))

#define PLUGIN_SECT_OFF HDR_SZ
#define PLUGIN_SECT_LEN (MAN_SZ + IMAGE_FILE)
#define MODEL_OFF       (PLUGIN_SECT_OFF + PLUGIN_SECT_LEN)
/*
 * [!] A GAP BETWEEN THE MODEL AND THE DATA SECTION, ON PURPOSE.  Sections need
 * not be contiguous -- the validator checks range and non-overlap, nothing else
 * -- and packing them tight made two cases below unreachable: nudging the model
 * by one byte to test the alignment check pushed it into the data section, so
 * SECTION_OVERLAP fired and the case passed without ever reaching the check it
 * was named after.
 */
#define SECTION_GAP     16u
#define DATA_OFF        (MODEL_OFF + MODEL_LEN + SECTION_GAP)
#define TOTAL           (DATA_OFF + DATA_LEN)

static uint8_t buf[TOTAL];

/* The policy a board would supply.  Deliberately not Grove's real numbers: this
 * file tests the validator, and pinning the board's address here would be the
 * very leak svc/ is not allowed to have. */
static const uint32_t TEST_LINK_ADDR = 0x20010000u;
static struct plugin_policy pol;

static void wr32(uint32_t at, uint32_t v)
{
	buf[at + 0u] = (uint8_t)(v & 0xFFu);
	buf[at + 1u] = (uint8_t)((v >> 8) & 0xFFu);
	buf[at + 2u] = (uint8_t)((v >> 16) & 0xFFu);
	buf[at + 3u] = (uint8_t)((v >> 24) & 0xFFu);
}

static uint32_t rd(uint32_t at)
{
	return (uint32_t)buf[at] | ((uint32_t)buf[at + 1u] << 8) |
	       ((uint32_t)buf[at + 2u] << 16) | ((uint32_t)buf[at + 3u] << 24);
}

/* Field addresses, so a case names a field rather than a number. */
#define HF(f)  ((uint32_t)offsetof(struct plugin_container_hdr, f))
#define SF(i, f) (HF(sections) + (uint32_t)(i) * \
                  (uint32_t)sizeof(struct plugin_section) + \
                  (uint32_t)offsetof(struct plugin_section, f))
#define MF(f)  (PLUGIN_SECT_OFF + (uint32_t)offsetof(struct plugin_manifest, f))

/* Re-stamp the digest over the plugin section.  Every manifest mutation calls
 * this, or the CRC would be what got rejected. */
static void restamp(void)
{
	wr32(HF(plugin_digest),
	     crc32_update(0u, buf + PLUGIN_SECT_OFF, PLUGIN_SECT_LEN));
}

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
	wr32(HF(section_count), 3u);

	wr32(SF(0, type), PLUGIN_SECTION_PLUGIN);
	wr32(SF(0, offset), PLUGIN_SECT_OFF);
	wr32(SF(0, length), PLUGIN_SECT_LEN);
	wr32(SF(1, type), PLUGIN_SECTION_MODEL);
	wr32(SF(1, offset), MODEL_OFF);
	wr32(SF(1, length), MODEL_LEN);
	wr32(SF(2, type), PLUGIN_SECTION_DATA);
	wr32(SF(2, offset), DATA_OFF);
	wr32(SF(2, length), DATA_LEN);

	buf[MF(magic) + 0u] = PLUGIN_MANIFEST_MAGIC0;
	buf[MF(magic) + 1u] = PLUGIN_MANIFEST_MAGIC1;
	buf[MF(magic) + 2u] = PLUGIN_MANIFEST_MAGIC2;
	buf[MF(magic) + 3u] = PLUGIN_MANIFEST_MAGIC3;
	wr32(MF(struct_size), MAN_SZ);
	wr32(MF(abi_version), PLUGIN_ABI_VERSION);
	wr32(MF(target_id), pol.target_id);
	wr32(MF(link_addr), TEST_LINK_ADDR);
	wr32(MF(capability), PLUGIN_CAP_DRAW | PLUGIN_CAP_REPORT);

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

	/* Mandatory three plus the two optional ones the capability bits claim.
	 * Odd values: bit 0 is the Thumb bit and the validator insists on it. */
	wr32(MF(slot) + 4u * PLUGIN_SLOT_ENTRY,     0x01u);
	wr32(MF(slot) + 4u * PLUGIN_SLOT_SHAPES_OK, 0x21u);
	wr32(MF(slot) + 4u * PLUGIN_SLOT_DECODE,    0x41u);
	wr32(MF(slot) + 4u * PLUGIN_SLOT_DRAW,      0x61u);
	wr32(MF(slot) + 4u * PLUGIN_SLOT_REPORT,    0x81u);
	for (i = 0u; i < PLUGIN_SLOT_COUNT; i++)
		if (rd(MF(slot) + 4u * i) != PLUGIN_SLOT_ABSENT)
			wr32(MF(stack) + 4u * i, 256u);

	memcpy(buf + MF(name), "unittest", 8);
	memcpy(buf + MF(build_id), "deadbeef", 8);

	restamp();
}

/* ---- helpers ------------------------------------------------------------- */

static struct plugin_view view;

static enum plugin_result run(void)
{
	return plugin_parse(buf, sizeof buf, &pol, &view);
}

static void expect(const char *what, enum plugin_result want)
{
	enum plugin_result got = run();

	if (got != want) {
		printf("  FAIL: %s -- wanted %s, got %s\n", what,
		       plugin_result_name(want), plugin_result_name(got));
		assert(0);
	}
	printf("  %-46s -> %s\n", what, plugin_result_name(got));
}

/* Mutate one 32-bit field, assert the code, restore. */
static void one(const char *what, uint32_t at, uint32_t v,
                enum plugin_result want, int stamp)
{
	uint32_t save = rd(at);

	wr32(at, v);
	if (stamp)
		restamp();
	expect(what, want);
	wr32(at, save);
	if (stamp)
		restamp();
}

/* ---- cases --------------------------------------------------------------- */

static void test_probe(void)
{
	uint8_t tfl[16];

	printf(" case: plugin_probe discriminates without validating\n");
	build();
	assert(plugin_probe(buf, sizeof buf) == PLUGIN_KIND_CONTAINER);

	/* A bare model: a small root offset at 0..3 and "TFL3" at 4..7. */
	memset(tfl, 0, sizeof tfl);
	tfl[0] = 0x18;
	tfl[4] = PLUGIN_TFLITE_IDENT0; tfl[5] = PLUGIN_TFLITE_IDENT1;
	tfl[6] = PLUGIN_TFLITE_IDENT2; tfl[7] = PLUGIN_TFLITE_IDENT3;
	assert(plugin_probe(tfl, sizeof tfl) == PLUGIN_KIND_TFLITE);

	/*
	 * [!] THE DISCRIMINATORS ARE INDEPENDENT, AND THIS IS THE CASE THAT SAYS
	 * SO.  A payload carrying OUR magic at 0..3 but "TFL3" at 4..7 is reported
	 * as a bare model, not a container: the identifier wins, so a legacy model
	 * whose first word happened to equal the magic still takes the old path
	 * instead of being parsed as a container and refused.
	 */
	memcpy(tfl, buf, 4);
	assert(plugin_probe(tfl, sizeof tfl) == PLUGIN_KIND_TFLITE);

	assert(plugin_probe(buf, 7u) == PLUGIN_KIND_UNKNOWN);
	assert(plugin_probe(NULL, 64u) == PLUGIN_KIND_UNKNOWN);
	printf("  container / tflite / short / NULL all classified\n");
}

static void test_valid(void)
{
	printf(" case: a well-formed container parses, and the view is rebased\n");
	build();
	expect("the container as built", PLUGIN_OK);

	assert(view.has_plugin == 1u);
	assert(view.model_off == MODEL_OFF && view.model_len == MODEL_LEN);
	assert(view.data_off == DATA_OFF && view.data_len == DATA_LEN);
	/* Offsets come back relative to CONTAINER byte 0, not to the image: the
	 * point of rebasing here is that no consumer adds two numbers that were
	 * validated separately. */
	assert(view.image_off == PLUGIN_SECT_OFF + MAN_SZ);
	assert(view.code_off == view.image_off + 0u);
	assert(view.code_len == IMAGE_CODE);
	assert(view.scratch_off == view.image_off + IMAGE_FILE + IMAGE_BSS);
	assert(view.link_addr == TEST_LINK_ADDR);
	assert(view.slot[PLUGIN_SLOT_DECODE] == 0x41u);   /* Thumb bit preserved */
	assert(view.slot[PLUGIN_SLOT_PARAM_SET] == PLUGIN_SLOT_ABSENT);
	assert(strcmp(view.name, "unittest") == 0);
	assert(strcmp(view.build_id, "deadbeef") == 0);
	printf("  view is POD, rebased, and keeps the Thumb bit\n");
}

static void test_header(void)
{
	printf(" case: header fields\n");
	build();
	one("magic byte flipped", 0u, 0x00434E4Eu, PLUGIN_ERR_MAGIC, 0);
	/* The format word is checked after the magic, so corrupt only it. */
	buf[4] = 'X';
	expect("format word flipped", PLUGIN_ERR_FORMAT);
	buf[4] = PLUGIN_CONTAINER_FORMAT0;
	one("hdr_size disagrees", HF(hdr_size), HDR_SZ + 4u,
	    PLUGIN_ERR_HDR_SIZE, 0);
	one("abi_version is not ours", HF(abi_version), PLUGIN_ABI_VERSION + 1u,
	    PLUGIN_ERR_ABI, 0);
	one("header reserved is set", HF(reserved), 1u, PLUGIN_ERR_RESERVED, 0);
	one("total_size larger than the payload", HF(total_size), TOTAL + 1u,
	    PLUGIN_ERR_TOTAL_SIZE, 0);
	one("total_size smaller than the payload", HF(total_size), TOTAL - 1u,
	    PLUGIN_ERR_TOTAL_SIZE, 0);
	one("section_count is zero", HF(section_count), 0u,
	    PLUGIN_ERR_SECTION_COUNT, 0);
	one("section_count exceeds the table", HF(section_count),
	    PLUGIN_SECTION_MAX + 1u, PLUGIN_ERR_SECTION_COUNT, 0);

	assert(run() == PLUGIN_OK);   /* everything restored */
}

static void test_sections(void)
{
	printf(" case: the section table\n");
	build();
	one("a zeroed section type", SF(1, type), PLUGIN_SECTION_NONE,
	    PLUGIN_ERR_SECTION_TYPE, 0);
	one("an unknown section type", SF(1, type), 99u,
	    PLUGIN_ERR_SECTION_TYPE, 0);
	one("two sections of the same kind", SF(2, type), PLUGIN_SECTION_MODEL,
	    PLUGIN_ERR_SECTION_TYPE, 0);
	one("section reserved is set", SF(1, reserved), 1u,
	    PLUGIN_ERR_RESERVED, 0);
	one("a zero-length section", SF(1, length), 0u,
	    PLUGIN_ERR_SECTION_RANGE, 0);
	one("a section past the container", SF(1, offset), TOTAL,
	    PLUGIN_ERR_SECTION_RANGE, 0);
	/*
	 * [!] THE ADDITION THAT WOULD WRAP.  offset near 2^32 with a length that
	 * carries it past the top is the input that makes `off + len <= total`
	 * pass while the range is nonsense; the validator is written as
	 * `len <= total - off` so that it cannot.
	 */
	one("offset+length wraps 32 bits", SF(1, offset), 0xFFFFFFF0u,
	    PLUGIN_ERR_SECTION_RANGE, 0);
	one("a section reaching into the header", SF(1, offset), 4u,
	    PLUGIN_ERR_SECTION_RANGE, 0);
	/*
	 * [!] THE OVERLAP HAS TO STAY IN RANGE TO REACH THE OVERLAP CHECK.  Moving
	 * the MODEL section forward instead pushed its far end past the container,
	 * so SECTION_RANGE fired first and the case passed while proving nothing --
	 * the exact failure this file's header warns about.  Pull the DATA section
	 * BACK into the model instead: both stay inside the container, and the only
	 * thing wrong is that they intersect.
	 */
	one("two sections overlap", SF(2, offset), MODEL_OFF + 4u,
	    PLUGIN_ERR_SECTION_OVERLAP, 0);
	one("the model is not 4-byte aligned", SF(1, offset), MODEL_OFF + 1u,
	    PLUGIN_ERR_MODEL_ALIGN, 0);

	/* No model at all: one DATA section and nothing else. */
	wr32(SF(0, type), PLUGIN_SECTION_DATA);
	wr32(SF(0, offset), DATA_OFF);
	wr32(SF(0, length), DATA_LEN);
	wr32(HF(section_count), 1u);
	wr32(HF(plugin_digest), 0u);
	expect("no model section", PLUGIN_ERR_NO_MODEL);
	build();

	assert(run() == PLUGIN_OK);
}

static void test_no_plugin_is_valid(void)
{
	printf(" case: a container with no plugin is valid\n");
	build();
	/* Turn the plugin section into a second data section... which is a
	 * duplicate.  Instead drop the section count to the model alone. */
	wr32(SF(0, type), PLUGIN_SECTION_MODEL);
	wr32(SF(0, offset), MODEL_OFF);
	wr32(SF(0, length), MODEL_LEN);
	wr32(HF(section_count), 1u);
	wr32(HF(plugin_digest), 0u);
	expect("model only, no plugin, no digest", PLUGIN_OK);
	assert(view.has_plugin == 0u);
	assert(view.model_len == MODEL_LEN);
	assert(view.slot[PLUGIN_SLOT_DECODE] == 0u);

	/* [!] A digest with nothing to cover is a refusal, not a spare field:
	 * absence has one spelling. */
	wr32(HF(plugin_digest), 1u);
	expect("a digest with no plugin section", PLUGIN_ERR_DIGEST);
	build();
}

static void test_manifest(void)
{
	printf(" case: the manifest\n");
	build();
	one("manifest magic flipped", MF(magic), 0u,
	    PLUGIN_ERR_MANIFEST_MAGIC, 1);
	one("struct_size disagrees", MF(struct_size), MAN_SZ - 4u,
	    PLUGIN_ERR_MANIFEST_SIZE, 1);
	one("manifest abi_version is not ours", MF(abi_version),
	    PLUGIN_ABI_VERSION + 1u, PLUGIN_ERR_ABI, 1);
	one("manifest reserved is set", MF(reserved), 1u,
	    PLUGIN_ERR_RESERVED, 1);
	one("a reserved target bit is set", MF(target_id),
	    pol.target_id | 0x00010000u, PLUGIN_ERR_RESERVED, 1);
	one("built for another target", MF(target_id),
	    plugin_target_id(PLUGIN_CPU_CORTEX_M7, PLUGIN_FPU_FPV5_D16,
	                     PLUGIN_FLOAT_ABI_HARD, 0, 0),
	    PLUGIN_ERR_TARGET, 1);
	one("prelinked somewhere else", MF(link_addr), TEST_LINK_ADDR + 32u,
	    PLUGIN_ERR_LINK_ADDR, 1);
	one("an unknown capability bit", MF(capability), 0x80000000u,
	    PLUGIN_ERR_CAPABILITY, 1);

	/* The plugin section is only MAN_SZ + IMAGE_FILE long, so a manifest that
	 * is too short for its own struct cannot be built by shrinking the field;
	 * shrink the SECTION instead. */
	wr32(SF(0, length), MAN_SZ - 1u);
	restamp();
	expect("plugin section shorter than a manifest",
	       PLUGIN_ERR_MANIFEST_SHORT);
	build();

	assert(run() == PLUGIN_OK);
}

static void test_image_and_segments(void)
{
	printf(" case: the load image and its segments\n");
	build();
	one("image starts inside the manifest", MF(image_off), MAN_SZ - 4u,
	    PLUGIN_ERR_IMAGE_RANGE, 1);
	one("image runs past its section", MF(file_size), PLUGIN_SECT_LEN,
	    PLUGIN_ERR_IMAGE_RANGE, 1);
	one("file_size exceeds mem_size", MF(mem_size), IMAGE_FILE - 32u,
	    PLUGIN_ERR_SIZES, 1);
	one("mem_size exceeds the reservation", MF(mem_size), pol.capacity + 32u,
	    PLUGIN_ERR_SIZES, 1);
	/*
	 * [!] THE FAR END OF THE RESERVATION.  mem_size must be a whole number of
	 * cache lines, because maintenance rounds outward and a ragged end would
	 * let it reach past what the plugin owns.
	 */
	one("mem_size is not a whole cache line", MF(mem_size), IMAGE_MEM + 16u,
	    PLUGIN_ERR_IMAGE_ALIGN, 1);
	one("code escapes the file part", MF(code_len), IMAGE_FILE + 4u,
	    PLUGIN_ERR_SEG_RANGE, 1);
	one("no code at all", MF(code_len), 0u, PLUGIN_ERR_SEG_RANGE, 1);
	one("bss escapes the memory image", MF(bss_len), IMAGE_MEM,
	    PLUGIN_ERR_SEG_RANGE, 1);
	one("code and data overlap", MF(data_off), IMAGE_CODE - 4u,
	    PLUGIN_ERR_SEG_OVERLAP, 1);
	one("bss and scratch overlap", MF(scratch_off), IMAGE_FILE,
	    PLUGIN_ERR_SEG_OVERLAP, 1);

	assert(run() == PLUGIN_OK);
}

static void test_slots(void)
{
	printf(" case: entry points\n");
	build();
	one("a mandatory slot is absent", MF(slot) + 4u * PLUGIN_SLOT_DECODE,
	    PLUGIN_SLOT_ABSENT, PLUGIN_ERR_SLOT_MISSING, 1);
	one("a slot has the Thumb bit clear", MF(slot) + 4u * PLUGIN_SLOT_DECODE,
	    0x40u, PLUGIN_ERR_SLOT_THUMB, 1);
	one("a slot points past the code", MF(slot) + 4u * PLUGIN_SLOT_DECODE,
	    (IMAGE_CODE + 16u) | 1u, PLUGIN_ERR_SLOT_RANGE, 1);
	one("a slot lands in data, not code",
	    MF(slot) + 4u * PLUGIN_SLOT_DECODE, IMAGE_CODE | 1u,
	    PLUGIN_ERR_SLOT_RANGE, 1);
	one("a slot asks for more stack than the thread has",
	    MF(stack) + 4u * PLUGIN_SLOT_DRAW,
	    pol.stack_limit[PLUGIN_SLOT_DRAW] + 1u, PLUGIN_ERR_STACK, 1);
	one("a present slot declares no stack",
	    MF(stack) + 4u * PLUGIN_SLOT_DRAW, 0u, PLUGIN_ERR_STACK, 1);

	/* An absent slot must declare no stack either: one spelling of absence. */
	wr32(MF(stack) + 4u * PLUGIN_SLOT_PARAM_SET, 64u);
	restamp();
	expect("an absent slot carries a stale stack", PLUGIN_ERR_STACK);
	build();

	/* Capability bit and slot are two spellings of the same fact. */
	one("a capability bit with no slot behind it", MF(capability),
	    PLUGIN_CAP_DRAW | PLUGIN_CAP_REPORT | PLUGIN_CAP_PARAMS,
	    PLUGIN_ERR_CAPABILITY, 1);
	one("a slot with no capability bit", MF(capability), PLUGIN_CAP_DRAW,
	    PLUGIN_ERR_CAPABILITY, 1);

	/* PARAM_SET and PARAM_GET share a bit, so neither may stand alone. */
	wr32(MF(capability), PLUGIN_CAP_DRAW | PLUGIN_CAP_REPORT |
	                     PLUGIN_CAP_PARAMS);
	wr32(MF(slot) + 4u * PLUGIN_SLOT_PARAM_SET, 0xA1u);
	wr32(MF(stack) + 4u * PLUGIN_SLOT_PARAM_SET, 128u);
	restamp();
	expect("param_set without param_get", PLUGIN_ERR_CAPABILITY);
	build();

	assert(run() == PLUGIN_OK);
}

static void test_policy_and_digest(void)
{
	struct plugin_policy narrow;

	printf(" case: policy and digest\n");
	build();

	/* The base refuses a capability it does not implement, even though the
	 * plugin declared it consistently. */
	narrow = pol;
	narrow.caps_supported = PLUGIN_CAP_DRAW;
	assert(plugin_parse(buf, sizeof buf, &narrow, &view) ==
	       PLUGIN_ERR_CAPABILITY);
	printf("  %-46s -> %s\n", "base does not implement a declared capability",
	       plugin_result_name(PLUGIN_ERR_CAPABILITY));

	/* [!] A stack limit of zero refuses that slot outright: a thread with
	 * nothing to spare is not a thread that will squeeze one call in. */
	narrow = pol;
	narrow.stack_limit[PLUGIN_SLOT_DRAW] = 0u;
	assert(plugin_parse(buf, sizeof buf, &narrow, &view) == PLUGIN_ERR_STACK);
	printf("  %-46s -> %s\n", "a thread that can spare nothing",
	       plugin_result_name(PLUGIN_ERR_STACK));

	/* The digest is the LAST check, so a body byte flipped without restamping
	 * is what reaches it. */
	buf[PLUGIN_SECT_OFF + MAN_SZ + 8u] ^= 0xFFu;
	expect("a byte of the image changed", PLUGIN_ERR_DIGEST);
	buf[PLUGIN_SECT_OFF + MAN_SZ + 8u] ^= 0xFFu;

	one("the stored digest is wrong", HF(plugin_digest),
	    rd(HF(plugin_digest)) ^ 1u, PLUGIN_ERR_DIGEST, 0);

	assert(run() == PLUGIN_OK);
}

static void test_arguments(void)
{
	printf(" case: arguments and short buffers\n");
	build();
	assert(plugin_parse(NULL, sizeof buf, &pol, &view) == PLUGIN_ERR_ARG);
	assert(plugin_parse(buf, sizeof buf, NULL, &view) == PLUGIN_ERR_ARG);
	assert(plugin_parse(buf, sizeof buf, &pol, NULL) == PLUGIN_ERR_ARG);
	assert(plugin_parse(buf, HDR_SZ - 1u, &pol, &view) == PLUGIN_ERR_SHORT);
	printf("  NULLs and a buffer shorter than the header are refused\n");
}

static void test_view_is_cleared_on_refusal(void)
{
	struct plugin_view zero;

	printf(" case: the view is cleared on every refusal\n");
	memset(&zero, 0, sizeof zero);
	build();

	/*
	 * [!] THIS IS THE ONE THAT CAUGHT A REAL BUG WHILE IT WAS BEING WRITTEN.
	 * check_manifest() fills the view as it validates, so a failure part-way
	 * through -- here, at the very last slot check -- used to leave the caller
	 * a view that was partly populated and wholly untrustworthy, while the
	 * header promised it was filled only on PLUGIN_OK.  A single exit that
	 * re-zeroes keeps the promise true instead of relying on every caller to
	 * check the code first.
	 */
	wr32(MF(stack) + 4u * PLUGIN_SLOT_REPORT,
	     pol.stack_limit[PLUGIN_SLOT_REPORT] + 1u);
	restamp();
	assert(run() == PLUGIN_ERR_STACK);
	assert(memcmp(&view, &zero, sizeof view) == 0);
	printf("  a late refusal leaves nothing behind\n");
	build();
}

int main(void)
{
	unsigned i;

	printf("test_plugin_load (svc/plugin_load.c):\n");

	pol.target_id = plugin_target_id(PLUGIN_CPU_CORTEX_M55,
	                                 PLUGIN_FPU_FP_ARMV8,
	                                 PLUGIN_FLOAT_ABI_HARD, 0, 1);
	pol.link_addr      = TEST_LINK_ADDR;
	pol.capacity       = 64u * 1024u;
	pol.image_align    = PLUGIN_IMAGE_ALIGN;
	pol.caps_supported = PLUGIN_CAP_KNOWN_MASK;
	for (i = 0u; i < PLUGIN_SLOT_COUNT; i++)
		pol.stack_limit[i] = 1024u;

	test_probe();
	test_valid();
	test_header();
	test_sections();
	test_no_plugin_is_valid();
	test_manifest();
	test_image_and_segments();
	test_slots();
	test_policy_and_digest();
	test_arguments();
	test_view_is_cleared_on_refusal();

	printf("test_plugin_load: all passed\n");
	return 0;
}
