/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Host unit test for svc/plugin_load.c -- container and manifest validation
 * (issue #101 = #78 Step 1a).
 *
 * WHY THIS FILE CARRIES SO MANY CASES.  plugin_load.h names thirty-two distinct
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
/*
 * [!] ROUNDED UP, AND THE TEST USED NOT TO BE.  This was
 * PLUGIN_SECT_OFF + PLUGIN_SECT_LEN, which came to 604 -- 4-aligned and twelve
 * bytes short of 16 -- so the fixture had EXACTLY the defect the packer had, and
 * a suite that agreed with the bug could not catch it.  The hardware did:
 * "Command stream addr 0x3aea78fc not aligned to 16 bytes".
 */
#define ALIGN_UP(v, a)  (((v) + (a) - 1u) & ~((a) - 1u))
#define MODEL_OFF       ALIGN_UP(PLUGIN_SECT_OFF + PLUGIN_SECT_LEN, \
                                 PLUGIN_MODEL_ALIGN)
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

/* The stack declaration build() writes (issue #111), and the policy's c.  Chosen
 * so that every term of max(A0, A1 + max(c, S)) is distinguishable. */
#define TEST_COST     256u
#define SINK          16u
#define DRAW_OWN      96u
#define DRAW_CROSS    64u
#define REPORT_OWN    128u
#define REPORT_CROSS  100u

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
			wr32(MF(stack_own) + 4u * i, 256u);
	/*
	 * Two slots reach a veneer (issue #111), with their own frames at the
	 * crossing SHALLOWER than their deepest own frames, so that swapping the
	 * two fields is visible: draw needs max(DRAW_OWN, DRAW_CROSS + c), which
	 * with the test policy's c is the second term, and report likewise.
	 */
	wr32(MF(stack_own) + 4u * PLUGIN_SLOT_DRAW,   DRAW_OWN);
	wr32(MF(stack_cross) + 4u * PLUGIN_SLOT_DRAW, DRAW_CROSS);
	wr32(MF(stack_own) + 4u * PLUGIN_SLOT_REPORT,   REPORT_OWN);
	wr32(MF(stack_cross) + 4u * PLUGIN_SLOT_REPORT, REPORT_CROSS);
	wr32(MF(stack_crossing), (1u << PLUGIN_SLOT_DRAW) |
	                         (1u << PLUGIN_SLOT_REPORT));
	wr32(MF(stack_sink), SINK);
	wr32(MF(stack_accounting), PLUGIN_STACK_ACCOUNTING);

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
	/* The declaration comes back as declared, and the requirement is the
	 * loader's own sum at the policy's c (issue #111). */
	assert(view.stack_own[PLUGIN_SLOT_DRAW] == DRAW_OWN);
	assert(view.stack_cross[PLUGIN_SLOT_DRAW] == DRAW_CROSS);
	assert(view.stack_crossing == ((1u << PLUGIN_SLOT_DRAW) |
	                               (1u << PLUGIN_SLOT_REPORT)));
	assert(view.stack_sink == SINK);
	assert(view.stack[PLUGIN_SLOT_DRAW] == DRAW_CROSS + TEST_COST);
	assert(view.stack[PLUGIN_SLOT_REPORT] == REPORT_CROSS + TEST_COST);
	assert(view.stack[PLUGIN_SLOT_DECODE] == 256u);
	assert(view.stack[PLUGIN_SLOT_PARAM_SET] == 0u);
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
	one("the model is not aligned at all", SF(1, offset), MODEL_OFF + 1u,
	    PLUGIN_ERR_MODEL_ALIGN, 0);
	/*
	 * [!] THE CASE THAT WOULD HAVE CAUGHT THE REAL BUG.  Four-byte alignment is
	 * the flatbuffer's requirement and it is NOT enough: the Ethos-U driver
	 * refuses every base address that is not 16-byte aligned.  Until containers
	 * existed a model always sat at a 4 KB-aligned payload address, so 16 was
	 * met by accident and nothing here tested for it.  An offset that is
	 * 4-aligned and not 16-aligned is the shape that reached the hardware.
	 */
	one("the model is 4-byte aligned but not 16", SF(1, offset), MODEL_OFF + 4u,
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
	    MF(stack_own) + 4u * PLUGIN_SLOT_DECODE,
	    pol.stack_limit[PLUGIN_SLOT_DECODE] + 1u, PLUGIN_ERR_STACK, 1);
	/* [!] AND ZERO IS NOT ONE OF THEM (issue #103).  This case used to assert a
	 * refusal, on the reasoning that zero meant "nobody measured it".  A
	 * frameless callback measures zero -- the classifier plugin's entry point
	 * does -- so refusing it made the packer unable to declare what the gate had
	 * derived.  Absence has its own spelling in the SLOT field, checked
	 * separately below, so nothing needs zero to mean it here. */
	one("a present slot may declare a derived bound of zero",
	    MF(stack_own) + 4u * PLUGIN_SLOT_DECODE, 0u, PLUGIN_OK, 1);

	/* An absent slot must declare no stack either: one spelling of absence. */
	wr32(MF(stack_own) + 4u * PLUGIN_SLOT_PARAM_SET, 64u);
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
	wr32(MF(stack_own) + 4u * PLUGIN_SLOT_PARAM_SET, 128u);
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

	/* [!] AND IT STILL REFUSES A SLOT THAT ASKS FOR NOTHING.  While zero was
	 * also the spelling of "unmeasured", that refusal came for free out of
	 * `st == 0u ||`; now that a plugin may legitimately declare zero, the two
	 * meet -- and a limit of zero has to win, because it is the board saying
	 * this callback may not run at all rather than a size comparison. */
	wr32(MF(stack_own) + 4u * PLUGIN_SLOT_DRAW, 0u);
	wr32(MF(stack_cross) + 4u * PLUGIN_SLOT_DRAW, 0u);
	wr32(MF(stack_crossing), 1u << PLUGIN_SLOT_REPORT);
	restamp();
	assert(plugin_parse(buf, sizeof buf, &pol, &view) == PLUGIN_OK);
	assert(view.stack[PLUGIN_SLOT_DRAW] == 0u);
	assert(plugin_parse(buf, sizeof buf, &narrow, &view) == PLUGIN_ERR_STACK);
	printf("  %-46s -> %s\n", "... even for a plugin that asks for nothing",
	       plugin_result_name(PLUGIN_ERR_STACK));
	build();

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
	wr32(MF(stack_own) + 4u * PLUGIN_SLOT_REPORT,
	     pol.stack_limit[PLUGIN_SLOT_REPORT] + 1u);
	restamp();
	assert(run() == PLUGIN_ERR_STACK);
	assert(memcmp(&view, &zero, sizeof view) == 0);
	printf("  a late refusal leaves nothing behind\n");
	build();
}

/* ---- ABI 2: the declaration without the firmware's cost (issue #111) ----- */

static enum plugin_result parse_with(const struct plugin_policy *p)
{
	return plugin_parse(buf, sizeof buf, p, &view);
}

static void report(const char *what, enum plugin_result got,
                   enum plugin_result want)
{
	if (got != want) {
		printf("  FAIL: %s -- wanted %s, got %s\n", what,
		       plugin_result_name(want), plugin_result_name(got));
		assert(0);
	}
	printf("  %-46s -> %s\n", what, plugin_result_name(got));
}

/*
 * (a) THE POINT OF #111.  One container, unchanged, parsed under two firmwares
 * that differ only in c: the one whose c still fits accepts it, the one whose c
 * does not refuses it.  Under ABI 1 the container carried a c of its own and
 * both would have given the same answer.
 */
static void test_cost_is_the_firmware_s(void)
{
	struct plugin_policy p = pol;
	uint32_t fits = pol.stack_limit[PLUGIN_SLOT_REPORT] - REPORT_CROSS;

	printf(" case: the same container under two veneer costs\n");
	build();
	p.veneer_cost = fits;
	report("c at which report's crossing just fits", parse_with(&p),
	       PLUGIN_OK);
	assert(view.stack[PLUGIN_SLOT_REPORT] ==
	       pol.stack_limit[PLUGIN_SLOT_REPORT]);
	p.veneer_cost = fits + 1u;
	report("... and one byte more, no re-pack", parse_with(&p),
	       PLUGIN_ERR_STACK);

	/* A policy with no cost is a board mistake, never a free crossing. */
	p.veneer_cost = 0u;
	report("a policy with c = 0", parse_with(&p), PLUGIN_ERR_ARG);
	p = pol;
	p.stack_accounting = 0u;
	report("a policy with no accounting version", parse_with(&p),
	       PLUGIN_ERR_ARG);
	assert(run() == PLUGIN_OK);
}

/* (b) An ABI 1 container -- one number per slot, c inside it -- is refused at
 * the header, before a single stack field is read under the new layout. */
static void test_old_abi(void)
{
	printf(" case: an ABI 1 container\n");
	build();
	one("header says ABI 1", HF(abi_version), 1u, PLUGIN_ERR_ABI, 0);
	one("manifest says ABI 1", MF(abi_version), 1u, PLUGIN_ERR_ABI, 1);
	assert(run() == PLUGIN_OK);
}

/* (c) One canonical form.  Each case breaks exactly one rule of it. */
static void test_canonical_form(void)
{
	printf(" case: the canonical form of the stack declaration\n");
	build();
	one("an absent slot declares frames at a crossing",
	    MF(stack_cross) + 4u * PLUGIN_SLOT_PARAM_SET, 8u,
	    PLUGIN_ERR_STACK, 1);
	one("an absent slot carries a crossing bit", MF(stack_crossing),
	    rd(MF(stack_crossing)) | (1u << PLUGIN_SLOT_PARAM_SET),
	    PLUGIN_ERR_STACK, 1);
	one("a slot that does not cross declares A1",
	    MF(stack_cross) + 4u * PLUGIN_SLOT_DECODE, 8u,
	    PLUGIN_ERR_STACK, 1);
	/* [!] THE SWAP.  A1 above A0 is the two fields exchanged; accepted, it
	 * would charge c on top of the shallower number. */
	one("A1 above A0 (the two parts swapped)",
	    MF(stack_cross) + 4u * PLUGIN_SLOT_DRAW, DRAW_OWN + 1u,
	    PLUGIN_ERR_STACK, 1);
	one("A1 equal to A0 is canonical",
	    MF(stack_cross) + 4u * PLUGIN_SLOT_DRAW, DRAW_OWN, PLUGIN_OK, 1);
	one("a crossing bit for a slot that does not exist", MF(stack_crossing),
	    rd(MF(stack_crossing)) | (1u << PLUGIN_SLOT_COUNT),
	    PLUGIN_ERR_RESERVED, 1);
	one("the top crossing bit", MF(stack_crossing),
	    rd(MF(stack_crossing)) | 0x80000000u, PLUGIN_ERR_RESERVED, 1);
	assert(run() == PLUGIN_OK);
}

/*
 * (d) A1 + max(c, S) that wraps.  The limit is opened all the way so that the
 * ONLY thing between this container and an accept is the overflow check: the
 * wrapped sum would come out tiny, A0 would win the max, and A0 fits.
 */
static void test_overflow(void)
{
	struct plugin_policy p = pol;

	printf(" case: the sum at a crossing wraps\n");
	build();
	p.stack_limit[PLUGIN_SLOT_DRAW] = 0xFFFFFFFFu;
	wr32(MF(stack_own) + 4u * PLUGIN_SLOT_DRAW, 0xFFFFFF80u);
	wr32(MF(stack_cross) + 4u * PLUGIN_SLOT_DRAW, 0xFFFFFF80u);
	restamp();
	report("A1 + c past 2^32", parse_with(&p), PLUGIN_ERR_STACK);
	/* And the same declaration without the crossing is fine: A0 alone fits
	 * the widened limit, so the refusal above was the sum's. */
	wr32(MF(stack_cross) + 4u * PLUGIN_SLOT_DRAW, 0u);
	wr32(MF(stack_crossing), 1u << PLUGIN_SLOT_REPORT);
	restamp();
	report("... the same A0 with no crossing", parse_with(&p), PLUGIN_OK);
	build();
}

/* (e) The plugin's own sink deeper than the firmware's c: S is charged. */
static void test_sink(void)
{
	struct plugin_policy p = pol;
	uint32_t s = TEST_COST * 2u;

	printf(" case: a sink deeper than c\n");
	build();
	wr32(MF(stack_sink), s);
	restamp();
	p.stack_limit[PLUGIN_SLOT_DRAW] = DRAW_CROSS + s;
	report("draw's crossing charged S, at the limit", parse_with(&p),
	       PLUGIN_OK);
	assert(view.stack[PLUGIN_SLOT_DRAW] == DRAW_CROSS + s);
	p.stack_limit[PLUGIN_SLOT_DRAW] = DRAW_CROSS + s - 1u;
	report("... one byte under it (c alone would fit)", parse_with(&p),
	       PLUGIN_ERR_STACK);
	build();
}

/* (f) Another analysis is its own refusal, not a stack that is too big. */
static void test_accounting(void)
{
	struct plugin_policy p = pol;

	printf(" case: the stack accounting version\n");
	build();
	one("manifest declared under another analysis", MF(stack_accounting),
	    PLUGIN_STACK_ACCOUNTING + 1u, PLUGIN_ERR_STACK_ACCOUNTING, 1);
	one("manifest never set it", MF(stack_accounting), 0u,
	    PLUGIN_ERR_STACK_ACCOUNTING, 1);
	p.stack_accounting = PLUGIN_STACK_ACCOUNTING + 1u;
	report("firmware reads another analysis", parse_with(&p),
	       PLUGIN_ERR_STACK_ACCOUNTING);
	assert(run() == PLUGIN_OK);
}

/* (g) A slot that reaches no veneer needs its own frames and nothing else,
 * however large c is. */
static void test_no_crossing_ignores_cost(void)
{
	struct plugin_policy p = pol;
	unsigned i;

	printf(" case: no crossing, no charge\n");
	build();
	wr32(MF(stack_cross) + 4u * PLUGIN_SLOT_DRAW, 0u);
	wr32(MF(stack_cross) + 4u * PLUGIN_SLOT_REPORT, 0u);
	wr32(MF(stack_crossing), 0u);
	restamp();
	p.veneer_cost = 0xFFFFFFF0u;
	report("c near 2^32 and nothing crosses", parse_with(&p), PLUGIN_OK);
	for (i = 0u; i < PLUGIN_SLOT_COUNT; i++)
		assert(view.stack[i] == view.stack_own[i]);
	/* And one crossing slot under that c is refused, so the accept above
	 * was the mask's doing. */
	wr32(MF(stack_crossing), 1u << PLUGIN_SLOT_DRAW);
	restamp();
	report("... the same c with draw crossing", parse_with(&p),
	       PLUGIN_ERR_STACK);
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
	pol.veneer_cost      = TEST_COST;
	pol.stack_accounting = PLUGIN_STACK_ACCOUNTING;

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
	test_cost_is_the_firmware_s();
	test_old_abi();
	test_canonical_form();
	test_overflow();
	test_sink();
	test_accounting();
	test_no_crossing_ignores_cost();

	printf("test_plugin_load: all passed\n");
	return 0;
}
