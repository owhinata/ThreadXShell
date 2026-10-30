/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * The shared Armv7-M MPU verdict (svc/plugin/plugin_mpu_v7m.c) on THIS board's
 * region set (issue #159 = #78 Step 4b, plan D6).
 *
 * The loader reads the MPU back before it branches into a plugin
 * (port/plugin/plugin_run.c) and refuses unless the reservation -- the top
 * 32 KB of internal SRAM, 0x20048000..0x20050000 -- is privileged-RW,
 * executable, Normal memory.  On this board no region covers it at all: it is
 * executable through the default memory map (PM0253 2.3.3, Table 16: SRAM is
 * Normal, executable), which the verdict only grants with PRIVDEFENA set.  So
 * what this pins is that the configuration src/bsp.c actually programs passes,
 * and that the configurations that must not -- the background map off, a
 * region placed over the reservation that makes it XN, Device or read-only, a
 * DREGION the snapshot cannot hold -- are refused.  None of those can be made
 * on the board, which is why the verdict is a pure function tested here.
 *
 * The regions are written out as src/bsp.c's mpu_config_sdram() programs them
 * through HAL_MPU_ConfigRegion() (RM0385 / PM0253 4.6.9 RASR layout):
 *   0: 0xC0000000, 8 MB, TEX=1 C=0 B=0 (Normal non-cacheable), AP full, XN
 *   1: 0xC0600000, 2 MB, TEX=1 C=1 B=1 (Normal WBWA),          AP full, XN
 *   2: disabled (tflm / null / stedgeai), or on stedgeai_reloc
 *      0xC0700000, 1 MB, Normal WBWA, AP full, executable
 * and HAL_MPU_Enable(MPU_PRIVILEGED_DEFAULT) = ENABLE | PRIVDEFENA.  The
 * STM32F746 MPU implements 8 regions (MPU_TYPE.DREGION = 8).
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

/* The reservation, stated here as its own literal (the fourth is the layout
 * gate's; none is derived from another). */
#define RES_LO 0x20048000u
#define RES_HI 0x20050000u

#define AP_FULL 3u
#define AP_PRIV_RO 5u
#define ATTR_NORMAL_NC   (1u << PL_MPU7_RASR_TEX_SHIFT)
#define ATTR_NORMAL_WBWA ((1u << PL_MPU7_RASR_TEX_SHIFT) | PL_MPU7_RASR_C | \
                          PL_MPU7_RASR_B)
#define ATTR_DEVICE      (PL_MPU7_RASR_B)

#define TYPE_8_REGIONS   (8u << PL_MPU7_TYPE_DREGION_SHIFT)
#define CTRL_BSP         (PL_MPU7_CTRL_ENABLE | PL_MPU7_CTRL_PRIVDEFENA)

static struct pl_mpu7_region mk(uint32_t base, unsigned size_log2, uint32_t ap,
                                uint32_t attr, int xn)
{
	struct pl_mpu7_region r;

	r.rbar = base & PL_MPU7_RBAR_ADDR_MASK;
	r.rasr = PL_MPU7_RASR_ENABLE |
	         ((uint32_t)(size_log2 - 1u) << PL_MPU7_RASR_SIZE_SHIFT) |
	         (ap << PL_MPU7_RASR_AP_SHIFT) | attr;
	if (xn)
		r.rasr |= PL_MPU7_RASR_XN;
	return r;
}

/* src/bsp.c's set; @p reloc adds region 2 as stedgeai_reloc programs it. */
static void bsp_regions(struct pl_mpu7_region *r, int reloc)
{
	memset(r, 0, sizeof(*r) * PL_MPU7_REGION_MAX);
	r[0] = mk(0xC0000000u, 23u, AP_FULL, ATTR_NORMAL_NC, 1);
	r[1] = mk(0xC0600000u, 21u, AP_FULL, ATTR_NORMAL_WBWA, 1);
	if (reloc)
		r[2] = mk(0xC0700000u, 20u, AP_FULL, ATTR_NORMAL_WBWA, 0);
}

int main(void)
{
	struct pl_mpu7_region r[PL_MPU7_REGION_MAX];

	printf("test_plugin_mpu_f746\n");

	bsp_regions(r, 0);
	CHECK(pl_mpu7_judge(CTRL_BSP, TYPE_8_REGIONS, r, PL_MPU7_REGION_MAX,
	                    RES_LO, RES_HI) == PL_MPU7_OK,
	      "src/bsp.c's regions (tflm): the reservation runs through the "
	      "default map");
	bsp_regions(r, 1);
	CHECK(pl_mpu7_judge(CTRL_BSP, TYPE_8_REGIONS, r, PL_MPU7_REGION_MAX,
	                    RES_LO, RES_HI) == PL_MPU7_OK,
	      "...and with stedgeai_reloc's executable region 2 (SDRAM, elsewhere)");

	bsp_regions(r, 0);
	CHECK(pl_mpu7_judge(PL_MPU7_CTRL_ENABLE, TYPE_8_REGIONS, r,
	                    PL_MPU7_REGION_MAX, RES_LO, RES_HI) ==
	      PL_MPU7_NO_DEFAULT,
	      "[!] the background map off: refused (nothing covers the SRAM)");

	bsp_regions(r, 0);
	r[3] = mk(0x20040000u, 16u, AP_FULL, ATTR_NORMAL_WBWA, 1);
	CHECK(pl_mpu7_judge(CTRL_BSP, TYPE_8_REGIONS, r, PL_MPU7_REGION_MAX,
	                    RES_LO, RES_HI) == PL_MPU7_XN,
	      "[!] an XN region over the reservation: refused");

	bsp_regions(r, 0);
	r[7] = mk(0x2004F000u, 12u, AP_FULL, ATTR_NORMAL_WBWA, 1);
	CHECK(pl_mpu7_judge(CTRL_BSP, TYPE_8_REGIONS, r, PL_MPU7_REGION_MAX,
	                    RES_LO, RES_HI) == PL_MPU7_XN,
	      "...even one covering only its last 4 KB, in the last region");

	bsp_regions(r, 0);
	r[4] = mk(0x20040000u, 16u, AP_FULL, ATTR_DEVICE, 0);
	CHECK(pl_mpu7_judge(CTRL_BSP, TYPE_8_REGIONS, r, PL_MPU7_REGION_MAX,
	                    RES_LO, RES_HI) == PL_MPU7_ATTR,
	      "a Device region over it: refused");

	bsp_regions(r, 0);
	r[4] = mk(0x20040000u, 16u, AP_PRIV_RO, ATTR_NORMAL_WBWA, 0);
	CHECK(pl_mpu7_judge(CTRL_BSP, TYPE_8_REGIONS, r, PL_MPU7_REGION_MAX,
	                    RES_LO, RES_HI) == PL_MPU7_AP,
	      "a read-only region over it: refused");

	bsp_regions(r, 0);
	r[5] = mk(0x20040000u, 16u, AP_FULL, ATTR_NORMAL_WBWA, 0);
	CHECK(pl_mpu7_judge(CTRL_BSP, TYPE_8_REGIONS, r, PL_MPU7_REGION_MAX,
	                    RES_LO, RES_HI) == PL_MPU7_OK,
	      "an executable Normal region over it: still accepted");

	bsp_regions(r, 0);
	CHECK(pl_mpu7_judge(CTRL_BSP, 17u << PL_MPU7_TYPE_DREGION_SHIFT, r,
	                    PL_MPU7_REGION_MAX, RES_LO, RES_HI) ==
	      PL_MPU7_TRUNCATED,
	      "a DREGION larger than the snapshot: refused, not clamped");

	if (fails) {
		printf("test_plugin_mpu_f746: %d FAILURE(S)\n", fails);
		return 1;
	}
	printf("test_plugin_mpu_f746: all passed\n");
	return 0;
}
