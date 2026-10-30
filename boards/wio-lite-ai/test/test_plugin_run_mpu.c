/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Host test for this board's MPU read-back (port/plugin/plugin_run.c,
 * plugin_run_mpu_ok(); issue #159 Step 4b review).  The REAL binding is compiled
 * against test/plugin_run_shim, whose MPU answers RBAR / RASR for the region RNR
 * selects, and whose PRIMASK is a variable.  The verdict itself is
 * svc/plugin/plugin_mpu_v7m.c's (linked, real) and tested on its own; what is
 * pinned here is the loop that feeds it, which the shared lease test stubs:
 *
 *   - every region 0..DREGION-1 is read (an XN region in the LAST one refuses);
 *   - RNR is put back to what it was, and PRIMASK to what it was -- masked
 *     around the read, restored after, and left masked if the caller had it so;
 *   - DREGION above the snapshot refuses (not clamped); DREGION 0 with the
 *     background map off refuses; with it on, the reservation passes.
 */
#include "plugin_run_core.h"
#include "plugin_mpu_v7m.h"
#include "stm32h7xx_hal.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

MPU_Type test_mpu_bank[TEST_MPU_BANKS];
unsigned test_mpu_sel;
uint32_t test_primask;
unsigned test_irq_disables;

/* What the binding links against that is not under test. */
void log_write(unsigned level, const char *tag, const char *fmt, ...)
{
	(void)level; (void)tag; (void)fmt;
}

int plugin_run_exec_ok(uint32_t lo, uint32_t hi, const char **why)
{
	(void)lo; (void)hi; (void)why;
	return -1;
}

int plugin_run_source_ok(const void *c, uint32_t len, uintptr_t token,
                         const char **why)
{
	(void)c; (void)len; (void)token; (void)why;
	return -1;
}

uint8_t __plugin_start[16], __plugin_end[16];

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

#define RES_LO 0x24048000u
#define LO RES_LO
#define HI (RES_LO + 0x8000u)

/* Every block: the same CTRL and TYPE; regions disabled; RNR at @p rnr. */
static void mpu(uint32_t ctrl, unsigned dregion, uint32_t rnr)
{
	unsigned k;

	memset(test_mpu_bank, 0, sizeof test_mpu_bank);
	for (k = 0u; k < TEST_MPU_BANKS; k++) {
		test_mpu_bank[k].CTRL = ctrl;
		test_mpu_bank[k].TYPE = (uint32_t)dregion <<
		                        PL_MPU7_TYPE_DREGION_SHIFT;
		test_mpu_bank[k].RNR  = rnr;
	}
	test_mpu_sel = rnr;
	test_primask = 0u;
	test_irq_disables = 0u;
}

/* An enabled XN Normal-WBWA full-access region over [base, base + 2^log2). */
static void xn_region(unsigned i, uint32_t base, unsigned log2)
{
	test_mpu_bank[i].RBAR = base & PL_MPU7_RBAR_ADDR_MASK;
	test_mpu_bank[i].RASR = PL_MPU7_RASR_ENABLE |
		((uint32_t)(log2 - 1u) << PL_MPU7_RASR_SIZE_SHIFT) |
		(3u << PL_MPU7_RASR_AP_SHIFT) |
		(1u << PL_MPU7_RASR_TEX_SHIFT) | PL_MPU7_RASR_C | PL_MPU7_RASR_B |
		PL_MPU7_RASR_XN;
}

#define CTRL_ON  (PL_MPU7_CTRL_ENABLE | PL_MPU7_CTRL_PRIVDEFENA)

int main(void)
{
	const char *why;

	printf("test_plugin_run_mpu (wio-lite-ai)\n");

	mpu(CTRL_ON, 8u, 5u);
	why = NULL;
	CHECK(plugin_run_mpu_ok(LO, HI, &why) == 0,
	      "the background map, no region over the reservation: accepted");
	CHECK(MPU->RNR == 5u,
	      "[!] RNR is put back to what it was (5)");
	CHECK(test_primask == 0u && test_irq_disables == 1u,
	      "interrupts masked for the read, and unmasked after");

	mpu(CTRL_ON, 8u, 2u);
	xn_region(7u, LO, 15u);
	why = NULL;
	CHECK(plugin_run_mpu_ok(LO, HI, &why) != 0 && why != NULL,
	      "[!] an XN region in the LAST region refuses -- every region is read");
	CHECK(MPU->RNR == 2u, "...and RNR is back at 2");

	mpu(CTRL_ON, 8u, 0u);
	xn_region(3u, LO, 15u);
	CHECK(plugin_run_mpu_ok(LO, HI, &why) != 0, "...and one in the middle");

	mpu(CTRL_ON, 6u, 0u);
	xn_region(7u, LO, 15u);
	CHECK(plugin_run_mpu_ok(LO, HI, &why) == 0,
	      "a region beyond DREGION is not read (the part has no such region)");

	mpu(CTRL_ON, 17u, 0u);
	why = NULL;
	CHECK(plugin_run_mpu_ok(LO, HI, &why) != 0 && why != NULL,
	      "[!] DREGION above the snapshot refuses, not clamped");

	mpu(PL_MPU7_CTRL_ENABLE, 0u, 0u);
	CHECK(plugin_run_mpu_ok(LO, HI, &why) != 0,
	      "no region and the background map off: refused");

	mpu(CTRL_ON, 0u, 0u);
	CHECK(plugin_run_mpu_ok(LO, HI, &why) == 0,
	      "no region and the background map on: accepted");

	mpu(CTRL_ON, 8u, 1u);
	test_primask = 1u;
	(void)plugin_run_mpu_ok(LO, HI, &why);
	CHECK(test_primask == 1u,
	      "a caller that had interrupts masked keeps them masked");

	if (fails) {
		printf("test_plugin_run_mpu: %d FAILURE(S)\n", fails);
		return 1;
	}
	printf("test_plugin_run_mpu: all passed\n");
	return 0;
}
