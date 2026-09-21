/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Host tests for port/plugin/plugin_mpu_v7m.c (issue #110).
 *
 * WHY A HOST TEST IS THE ONLY TEST.  Every case below except the first is a
 * configuration this board cannot be made to have: the four regions mpu.c
 * programs are fixed, none of them covers the plugin reservation, and nothing
 * reconfigures the MPU after boot.  A check written inline in the loader would
 * therefore be a check nobody has ever seen say no -- which is the shape issues
 * #42 and #66 spent two rounds removing from this repository.  The verdict is a
 * pure function of the register values, so every branch is reachable here.
 *
 * [!] AND THE ARCHITECTURE IS NOT GROVE'S.  On Armv7-M the highest-numbered
 * matching region wins and the others are not consulted; on Armv8-M two matches
 * is a fault.  This board RELIES on the v7-M rule -- mpu.c carves a cacheable
 * window out of a larger non-cacheable one -- so a judgement copied from the
 * other board would refuse the configuration this board ships with.
 */
#include "plugin_mpu_v7m.h"

#include <stdio.h>
#include <string.h>

static int fails;

#define CHECK(cond, what)                                                     \
	do {                                                                      \
		if (cond) {                                                           \
			printf("  ok   %s\n", (what));                                    \
		} else {                                                              \
			printf("  FAIL %s  (%s:%d)\n", (what), __FILE__, __LINE__);       \
			fails++;                                                          \
		}                                                                     \
	} while (0)

/* The reservation this board actually has: the top 32 KB of AXI-SRAM. */
#define RES_LO 0x24048000u
#define RES_HI 0x24050000u

/* --- region construction -------------------------------------------------- */

#define AP_NONE     0u
#define AP_PRIV_RW  1u
#define AP_PRIV_RW_USER_RO 2u
#define AP_FULL     3u
#define AP_PRIV_RO  5u
#define AP_RO       6u

/* TEX/C/B triples from Table B3-13. */
#define ATTR_NORMAL_NC   (1u << PL_MPU7_RASR_TEX_SHIFT)                       /* 001 0 0 */
#define ATTR_NORMAL_WBWA ((1u << PL_MPU7_RASR_TEX_SHIFT) | PL_MPU7_RASR_C | PL_MPU7_RASR_B)
#define ATTR_NORMAL_WT   (PL_MPU7_RASR_C)                                     /* 000 1 0 */
#define ATTR_DEVICE      (PL_MPU7_RASR_B)                                     /* 000 0 1 */
#define ATTR_SO          (0u)                                                 /* 000 0 0 */
#define ATTR_RESERVED    ((1u << PL_MPU7_RASR_TEX_SHIFT) | PL_MPU7_RASR_B)    /* 001 0 1 */
#define ATTR_DEV_NS      (2u << PL_MPU7_RASR_TEX_SHIFT)                       /* 010 0 0 */
#define ATTR_CACHEABLE   (4u << PL_MPU7_RASR_TEX_SHIFT)                       /* 1BB ... */

/* size_log2 is the region size as a power of two, e.g. 15 for 32 KB. */
static struct pl_mpu7_region mk(uint32_t base, unsigned size_log2, uint32_t ap,
                                uint32_t attr, int xn, uint32_t srd)
{
	struct pl_mpu7_region r;

	r.rbar = base & PL_MPU7_RBAR_ADDR_MASK;
	r.rasr = PL_MPU7_RASR_ENABLE |
	         ((uint32_t)(size_log2 - 1u) << PL_MPU7_RASR_SIZE_SHIFT) |
	         (srd << PL_MPU7_RASR_SRD_SHIFT) |
	         (ap << PL_MPU7_RASR_AP_SHIFT) | attr;
	if (xn)
		r.rasr |= PL_MPU7_RASR_XN;
	return r;
}

static struct pl_mpu7_region disabled(void)
{
	struct pl_mpu7_region r = { 0u, 0u };

	return r;
}

static uint32_t type_of(unsigned dregion)
{
	return (uint32_t)dregion << PL_MPU7_TYPE_DREGION_SHIFT;
}

#define CTRL_ON_DEFAULT (PL_MPU7_CTRL_ENABLE | PL_MPU7_CTRL_PRIVDEFENA)

/* --- cases ---------------------------------------------------------------- */

/*
 * This board's real configuration, as mpu.c programs it: PSRAM at 0x90000000
 * (8 MB, XN, non-cacheable shareable), ITCM at 0 (64 KB, read-only, executable),
 * the OCTOSPI2 quarter fenced off at 0x70000000 (256 MB, no access, XN), and a
 * cacheable 2 MB carve-out at 0x90600000 that OVERLAPS region 0.
 */
static void mpu_as_this_board_has_it(struct pl_mpu7_region *r)
{
	r[0] = mk(0x90000000u, 23u, AP_FULL,    ATTR_NORMAL_NC,   1, 0u);
	r[1] = mk(0x00000000u, 16u, AP_PRIV_RO, ATTR_NORMAL_NC,   0, 0u);
	r[2] = mk(0x70000000u, 28u, AP_NONE,    ATTR_NORMAL_NC,   1, 0u);
	r[3] = mk(0x90600000u, 21u, AP_FULL,    ATTR_NORMAL_WBWA, 1, 0u);
}

static void test_this_board(void)
{
	struct pl_mpu7_region r[PL_MPU7_REGION_MAX];

	printf("the configuration this board actually ships with:\n");
	memset(r, 0, sizeof r);
	mpu_as_this_board_has_it(r);

	CHECK(pl_mpu7_judge(CTRL_ON_DEFAULT, type_of(4u), r, 4u, RES_LO, RES_HI) ==
	              PL_MPU7_OK,
	      "the reservation is executable: no region covers it and the "
	      "background map says Normal");

	/* [!] The overlap that would be a fault on Armv8-M.  Region 3 sits inside
	 * region 0 and must simply win where they meet. */
	CHECK(pl_mpu7_judge(CTRL_ON_DEFAULT, type_of(4u), r, 4u,
	                    0x90600000u, 0x90600020u) == PL_MPU7_XN,
	      "[!] two regions covering one address is ordinary here, and the "
	      "higher-numbered one is the one consulted");
	/* [!] ITCM is executable and NOT acceptable, which is the distinction
	 * this judge has to make: mpu.c marks it read-only on purpose (a stray
	 * NULL write would otherwise overwrite the ISRs), and the loader has to
	 * WRITE a reservation before it runs it.  "Code can run there" is not the
	 * question being asked. */
	CHECK(pl_mpu7_judge(CTRL_ON_DEFAULT, type_of(4u), r, 4u,
	                    0x00000000u, 0x00000020u) == PL_MPU7_AP,
	      "[!] ITCM is executable but read-only, and a reservation must be "
	      "written before it is run");
}

static void test_no_region_and_the_background_map(void)
{
	struct pl_mpu7_region r[PL_MPU7_REGION_MAX];

	printf("what happens when nothing covers the reservation:\n");
	memset(r, 0, sizeof r);

	CHECK(pl_mpu7_judge(CTRL_ON_DEFAULT, type_of(0u), NULL, 0u,
	                    RES_LO, RES_HI) == PL_MPU7_OK,
	      "with no regions at all the background map answers");
	CHECK(pl_mpu7_judge(PL_MPU7_CTRL_ENABLE, type_of(0u), NULL, 0u,
	                    RES_LO, RES_HI) == PL_MPU7_NO_DEFAULT,
	      "[!] but not when PRIVDEFENA is clear -- then it is a fault, not a "
	      "default");
	CHECK(pl_mpu7_judge(0u, type_of(0u), NULL, 0u, RES_LO, RES_HI) ==
	              PL_MPU7_OK,
	      "and with the MPU switched off the background map is all there is");

	/* PRIVDEFENA is irrelevant where a region DOES cover the range. */
	r[0] = mk(0x24000000u, 19u, AP_FULL, ATTR_NORMAL_WBWA, 0, 0u);
	CHECK(pl_mpu7_judge(PL_MPU7_CTRL_ENABLE, type_of(1u), r, 1u,
	                    RES_LO, RES_HI) == PL_MPU7_OK,
	      "a region that covers it answers whether or not the background map "
	      "is on");

	printf("and what the background map says elsewhere:\n");
	CHECK(pl_mpu7_judge(CTRL_ON_DEFAULT, type_of(0u), NULL, 0u,
	                    0x40000000u, 0x40000020u) == PL_MPU7_XN,
	      "the peripheral quarter is Device and execute-never");
	CHECK(pl_mpu7_judge(CTRL_ON_DEFAULT, type_of(0u), NULL, 0u,
	                    0xE0000000u, 0xE0000020u) == PL_MPU7_XN,
	      "and so is the private peripheral bus");
}

static void test_permissions_and_attributes(void)
{
	struct pl_mpu7_region r[PL_MPU7_REGION_MAX];
	unsigned i;
	static const uint32_t refused_ap[] = { AP_NONE, AP_PRIV_RO, AP_RO, 7u, 4u };
	static const uint32_t allowed_ap[] = { AP_PRIV_RW, AP_PRIV_RW_USER_RO,
	                                       AP_FULL };

	printf("permissions:\n");
	memset(r, 0, sizeof r);

	for (i = 0u; i < sizeof allowed_ap / sizeof allowed_ap[0]; i++) {
		r[0] = mk(0x24000000u, 19u, allowed_ap[i], ATTR_NORMAL_WBWA, 0, 0u);
		CHECK(pl_mpu7_judge(CTRL_ON_DEFAULT, type_of(1u), r, 1u,
		                    RES_LO, RES_HI) == PL_MPU7_OK,
		      "an AP that grants privileged read/write is accepted");
	}
	for (i = 0u; i < sizeof refused_ap / sizeof refused_ap[0]; i++) {
		r[0] = mk(0x24000000u, 19u, refused_ap[i], ATTR_NORMAL_WBWA, 0, 0u);
		CHECK(pl_mpu7_judge(CTRL_ON_DEFAULT, type_of(1u), r, 1u,
		                    RES_LO, RES_HI) == PL_MPU7_AP,
		      "one that does not is refused (reserved encodings included)");
	}

	r[0] = mk(0x24000000u, 19u, AP_FULL, ATTR_NORMAL_WBWA, 1, 0u);
	CHECK(pl_mpu7_judge(CTRL_ON_DEFAULT, type_of(1u), r, 1u,
	                    RES_LO, RES_HI) == PL_MPU7_XN,
	      "execute-never is refused, however good the permissions are");

	printf("memory type -- 'not Device' is not the same as 'Normal':\n");
	r[0] = mk(0x24000000u, 19u, AP_FULL, ATTR_NORMAL_NC, 0, 0u);
	CHECK(pl_mpu7_judge(CTRL_ON_DEFAULT, type_of(1u), r, 1u,
	                    RES_LO, RES_HI) == PL_MPU7_OK,
	      "Normal non-cacheable is Normal");
	r[0] = mk(0x24000000u, 19u, AP_FULL, ATTR_NORMAL_WT, 0, 0u);
	CHECK(pl_mpu7_judge(CTRL_ON_DEFAULT, type_of(1u), r, 1u,
	                    RES_LO, RES_HI) == PL_MPU7_OK,
	      "so is Normal write-through");
	r[0] = mk(0x24000000u, 19u, AP_FULL, ATTR_CACHEABLE, 0, 0u);
	CHECK(pl_mpu7_judge(CTRL_ON_DEFAULT, type_of(1u), r, 1u,
	                    RES_LO, RES_HI) == PL_MPU7_OK,
	      "and so is every TEX[2]=1 cacheable encoding");
	r[0] = mk(0x24000000u, 19u, AP_FULL, ATTR_DEVICE, 0, 0u);
	CHECK(pl_mpu7_judge(CTRL_ON_DEFAULT, type_of(1u), r, 1u,
	                    RES_LO, RES_HI) == PL_MPU7_ATTR,
	      "Device is not");
	r[0] = mk(0x24000000u, 19u, AP_FULL, ATTR_SO, 0, 0u);
	CHECK(pl_mpu7_judge(CTRL_ON_DEFAULT, type_of(1u), r, 1u,
	                    RES_LO, RES_HI) == PL_MPU7_ATTR,
	      "nor is strongly-ordered");
	r[0] = mk(0x24000000u, 19u, AP_FULL, ATTR_DEV_NS, 0, 0u);
	CHECK(pl_mpu7_judge(CTRL_ON_DEFAULT, type_of(1u), r, 1u,
	                    RES_LO, RES_HI) == PL_MPU7_ATTR,
	      "nor non-shareable Device");
	r[0] = mk(0x24000000u, 19u, AP_FULL, ATTR_RESERVED, 0, 0u);
	CHECK(pl_mpu7_judge(CTRL_ON_DEFAULT, type_of(1u), r, 1u,
	                    RES_LO, RES_HI) == PL_MPU7_ATTR,
	      "[!] and a RESERVED encoding is refused rather than read as Normal");
}

static void test_coverage_and_subregions(void)
{
	struct pl_mpu7_region r[PL_MPU7_REGION_MAX];

	printf("partial coverage, and the holes only this architecture has:\n");
	memset(r, 0, sizeof r);

	/* A region that covers only the first half of the reservation, over a
	 * background map that happens to be fine -- so the shortfall is invisible
	 * unless every granule is resolved. */
	r[0] = mk(RES_LO, 14u, AP_FULL, ATTR_NORMAL_WBWA, 1, 0u);   /* 16 KB, XN */
	CHECK(pl_mpu7_judge(CTRL_ON_DEFAULT, type_of(1u), r, 1u,
	                    RES_LO, RES_HI) == PL_MPU7_XN,
	      "[!] a region covering only part of the range is still consulted "
	      "for that part");
	CHECK(pl_mpu7_judge(CTRL_ON_DEFAULT, type_of(1u), r, 1u,
	                    RES_LO + 0x4000u, RES_HI) == PL_MPU7_OK,
	      "and the rest falls to the background map");

	/* A 32 KB region exactly over the reservation with one sub-region -- 4 KB
	 * -- switched off.  Underneath is the background map, which is fine, so
	 * the hole is only visible because a hole falls THROUGH rather than
	 * denying. */
	r[0] = mk(RES_LO, 15u, AP_FULL, ATTR_NORMAL_WBWA, 1, 0x04u);
	CHECK(pl_mpu7_judge(CTRL_ON_DEFAULT, type_of(1u), r, 1u,
	                    RES_LO + 0x2000u, RES_LO + 0x3000u) == PL_MPU7_OK,
	      "[!] a disabled sub-region falls through to what is underneath");
	CHECK(pl_mpu7_judge(CTRL_ON_DEFAULT, type_of(1u), r, 1u,
	                    RES_LO, RES_HI) == PL_MPU7_XN,
	      "and the sub-regions that are still enabled are still consulted");

	/* The same hole with a lower-numbered region underneath it. */
	r[0] = mk(0x24000000u, 19u, AP_FULL, ATTR_DEVICE, 0, 0u);
	r[1] = mk(RES_LO, 15u, AP_FULL, ATTR_NORMAL_WBWA, 0, 0x04u);
	CHECK(pl_mpu7_judge(CTRL_ON_DEFAULT, type_of(2u), r, 2u,
	                    RES_LO + 0x2000u, RES_LO + 0x3000u) == PL_MPU7_ATTR,
	      "a hole in the higher region exposes the lower one, whatever it says");
	CHECK(pl_mpu7_judge(CTRL_ON_DEFAULT, type_of(2u), r, 2u,
	                    RES_LO, RES_LO + 0x2000u) == PL_MPU7_OK,
	      "while the covered part still answers from the higher region");

	/* Disabled regions say nothing even when they cover the range. */
	memset(r, 0, sizeof r);
	r[0] = mk(0x24000000u, 19u, AP_FULL, ATTR_NORMAL_WBWA, 0, 0u);
	r[1] = disabled();
	CHECK(pl_mpu7_judge(CTRL_ON_DEFAULT, type_of(2u), r, 2u,
	                    RES_LO, RES_HI) == PL_MPU7_OK,
	      "a disabled higher region does not shadow an enabled lower one");
}

static void test_malformed_and_truncated(void)
{
	struct pl_mpu7_region r[PL_MPU7_REGION_MAX];

	printf("[!] snapshots and regions that cannot be judged:\n");
	memset(r, 0, sizeof r);
	mpu_as_this_board_has_it(r);

	CHECK(pl_mpu7_judge(CTRL_ON_DEFAULT, type_of(8u), r, 4u,
	                    RES_LO, RES_HI) == PL_MPU7_TRUNCATED,
	      "[!] a snapshot shorter than DREGION is REFUSED, not clamped -- the "
	      "regions it omits are the higher-numbered ones, which win");
	CHECK(pl_mpu7_judge(CTRL_ON_DEFAULT, type_of(64u), r,
	                    PL_MPU7_REGION_MAX, RES_LO, RES_HI) ==
	              PL_MPU7_TRUNCATED,
	      "and so is a DREGION larger than this decoder will walk");
	CHECK(pl_mpu7_judge(CTRL_ON_DEFAULT, type_of(2u), NULL, 2u,
	                    RES_LO, RES_HI) == PL_MPU7_ARG,
	      "a null table with a non-zero DREGION is an argument error");

	/* Misaligned, undersized, and sub-regions where they are meaningless. */
	memset(r, 0, sizeof r);
	r[0] = mk(0x24000000u, 19u, AP_FULL, ATTR_NORMAL_WBWA, 0, 0u);
	r[0].rbar = 0x24000020u;          /* 512 KB region not at a 512 KB boundary */
	CHECK(pl_mpu7_judge(CTRL_ON_DEFAULT, type_of(1u), r, 1u,
	                    RES_LO, RES_HI) == PL_MPU7_REGION_BAD,
	      "a region that is not naturally aligned is UNPREDICTABLE, so refused");

	r[0] = mk(0x24000000u, 19u, AP_FULL, ATTR_NORMAL_WBWA, 0, 0u);
	r[0].rasr = (r[0].rasr & ~(PL_MPU7_RASR_SIZE_MASK << PL_MPU7_RASR_SIZE_SHIFT)) |
	            (2u << PL_MPU7_RASR_SIZE_SHIFT);
	CHECK(pl_mpu7_judge(CTRL_ON_DEFAULT, type_of(1u), r, 1u,
	                    RES_LO, RES_HI) == PL_MPU7_REGION_BAD,
	      "and so is one smaller than the 32-byte minimum");

	r[0] = mk(RES_LO, 6u, AP_FULL, ATTR_NORMAL_WBWA, 0, 0x01u);  /* 64 B + SRD */
	CHECK(pl_mpu7_judge(CTRL_ON_DEFAULT, type_of(1u), r, 1u,
	                    RES_LO, RES_HI) == PL_MPU7_REGION_BAD,
	      "[!] sub-regions below 256 bytes are UNPREDICTABLE, not ignored");

	printf("arguments:\n");
	CHECK(pl_mpu7_judge(CTRL_ON_DEFAULT, type_of(0u), NULL, 0u,
	                    RES_HI, RES_LO) == PL_MPU7_ARG,
	      "an inverted range is refused");
	CHECK(pl_mpu7_judge(CTRL_ON_DEFAULT, type_of(0u), NULL, 0u,
	                    RES_LO, RES_LO) == PL_MPU7_ARG,
	      "and so is an empty one");
	CHECK(pl_mpu7_judge(CTRL_ON_DEFAULT, type_of(0u), NULL, 0u,
	                    RES_LO + 1u, RES_HI) == PL_MPU7_ARG,
	      "a range that is not granule-aligned cannot be resolved");
}

int main(void)
{
	printf("test_plugin_mpu_v7m (port/plugin/plugin_mpu_v7m.c):\n");
	test_this_board();
	test_no_region_and_the_background_map();
	test_permissions_and_attributes();
	test_coverage_and_subregions();
	test_malformed_and_truncated();

	if (fails != 0) {
		printf("test_plugin_mpu_v7m: %d FAILED\n", fails);
		return 1;
	}
	printf("test_plugin_mpu_v7m: all passed\n");
	return 0;
}
