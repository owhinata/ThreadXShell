/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Host unit test for port/plugin/plugin_mpu.c (issue #103).
 *
 * WHY THIS FILE IS THE ONLY CHECK.  Every refusal here describes a board this
 * project cannot arrange to have: the plugin reservation covered by two MPU
 * regions, or marked Device, or execute-never.  A check written inline in the
 * loader would be one nobody has ever seen say no -- the failure mode issues
 * #42/#66 spent themselves on, where a gate passed seven shapes it could not
 * decode.  Taking the register values as arguments is what makes every branch
 * reachable from here.
 */
#include "plugin_mpu.h"

#include <stdio.h>
#include <stdlib.h>

#define LO  0x341E0000u
#define HI  0x34200000u

static int failures;

static const char *name(enum plugin_mpu_verdict v)
{
	return plugin_mpu_strerror(v);
}

static void expect(const char *what, enum plugin_mpu_verdict got,
                   enum plugin_mpu_verdict want)
{
	if (got != want) {
		printf("  FAIL %-52s got %s, want %s\n", what, name(got), name(want));
		failures++;
	} else {
		printf("  ok   %-52s %s\n", what, name(got));
	}
}

/* A region table with one entry covering exactly the reservation, Normal,
 * privileged RW, executable.  Everything else is derived from this by changing
 * one thing, so a refusal is about that thing. */
static struct plugin_mpu_region good[2];
static uint32_t mair0, mair1;

#define TYPE_2   (2u << PLUGIN_MPU_TYPE_DREGION_SHIFT)
#define CTRL_ON  PLUGIN_MPU_CTRL_ENABLE

static void reset(void)
{
	/* base 0x341E0000, AP = 0b01 (RW any), XN clear */
	good[0].rbar = 0x341E0000u | (1u << PLUGIN_MPU_RBAR_AP_SHIFT);
	/* limit block containing 0x341FFFFF, AttrIndx 0, enabled, PXN clear */
	good[0].rlar = (0x341FFFE0u) | PLUGIN_MPU_RLAR_EN;
	good[1].rbar = 0u;
	good[1].rlar = 0u;                       /* disabled */
	mair0 = 0x000000FFu;                     /* index 0: Normal WB/WB */
	mair1 = 0u;
}

static enum plugin_mpu_verdict run(uint32_t ctrl)
{
	return plugin_mpu_judge(ctrl, TYPE_2, good, 2u, mair0, mair1, LO, HI);
}

static void test_ok(void)
{
	printf(" case: the arrangement this board is expected to have\n");
	reset();
	expect("one Normal, executable region covering it exactly",
	       run(CTRL_ON), PLUGIN_MPU_OK);
}

static void test_limit_decoding(void)
{
	printf(" case: RLAR limit decoding\n");

	/*
	 * [!] THE CASE THAT CATCHES THE MISTAKE NEXT DOOR.  RLAR keeps the limit
	 * with its low five bits stripped and the region runs to the END of that
	 * 32-byte block.  port/nor/nor_flash.c's diagnostic capture compares
	 * against the masked value, which makes every region look 31 bytes short.
	 * Here the reservation ends exactly at the region's end -- the one place
	 * that error turns a full cover into a partial one.
	 */
	reset();
	expect("a region ending exactly at the reservation's end covers it",
	       run(CTRL_ON), PLUGIN_MPU_OK);

	/* One block short: now it really is partial. */
	reset();
	good[0].rlar = 0x341FFFC0u | PLUGIN_MPU_RLAR_EN;
	expect("one 32-byte block short is partial", run(CTRL_ON),
	       PLUGIN_MPU_PARTIAL);
}

static void test_coverage(void)
{
	printf(" case: coverage and overlap\n");

	reset();
	good[0].rbar = 0x341E0020u | (1u << PLUGIN_MPU_RBAR_AP_SHIFT);
	expect("a region starting inside it is partial", run(CTRL_ON),
	       PLUGIN_MPU_PARTIAL);

	/*
	 * [!] PMSAv8 DOES NOT RESOLVE OVERLAP.  PMSAv7 let the highest-numbered
	 * region win; here a match in two enabled regions invalidates the match and
	 * the access faults, so two is a refusal rather than a question of which
	 * one applies.
	 */
	reset();
	good[1].rbar = 0x341F0000u | (1u << PLUGIN_MPU_RBAR_AP_SHIFT);
	good[1].rlar = 0x341F8000u | PLUGIN_MPU_RLAR_EN;
	expect("a second enabled region intersecting is refused", run(CTRL_ON),
	       PLUGIN_MPU_MULTIPLE);

	/* A disabled second region is not a second region. */
	reset();
	good[1].rbar = 0x341F0000u;
	good[1].rlar = 0x341F8000u;              /* EN clear */
	expect("a disabled overlapping region is ignored", run(CTRL_ON),
	       PLUGIN_MPU_OK);

	/* Adjacent but not touching. */
	reset();
	good[1].rbar = 0x34100000u | (1u << PLUGIN_MPU_RBAR_AP_SHIFT);
	good[1].rlar = 0x341DFFE0u | PLUGIN_MPU_RLAR_EN;
	expect("a region ending one block below is not an intersection",
	       run(CTRL_ON), PLUGIN_MPU_OK);
}

static void test_permissions(void)
{
	printf(" case: permissions\n");

	reset();
	good[0].rbar |= PLUGIN_MPU_RBAR_XN;
	expect("execute-never", run(CTRL_ON), PLUGIN_MPU_XN);

	reset();
	good[0].rlar |= PLUGIN_MPU_RLAR_PXN;
	expect("privileged execute-never", run(CTRL_ON), PLUGIN_MPU_PXN);

	/* AP 0b10 and 0b11 are read-only.  A plugin writes its own .data and .bss,
	 * and the loader zeroes .bss before it ever runs. */
	reset();
	good[0].rbar = (good[0].rbar & ~(PLUGIN_MPU_RBAR_AP_MASK
	                                 << PLUGIN_MPU_RBAR_AP_SHIFT))
	               | (2u << PLUGIN_MPU_RBAR_AP_SHIFT);
	expect("read-only, privileged", run(CTRL_ON), PLUGIN_MPU_AP);

	reset();
	good[0].rbar = (good[0].rbar & ~(PLUGIN_MPU_RBAR_AP_MASK
	                                 << PLUGIN_MPU_RBAR_AP_SHIFT))
	               | (3u << PLUGIN_MPU_RBAR_AP_SHIFT);
	expect("read-only, any privilege", run(CTRL_ON), PLUGIN_MPU_AP);
}

static void test_attributes(void)
{
	printf(" case: memory attributes\n");

	/* Device: outer nibble zero. */
	reset();
	mair0 = 0x00000004u;
	expect("Device memory", run(CTRL_ON), PLUGIN_MPU_ATTR);

	/*
	 * [!] THE ONE A "NOT DEVICE" TEST WOULD HAVE LET THROUGH.  Outer non-zero
	 * with inner zero is a RESERVED encoding, not Normal and not Device.  This
	 * is why the decode is a decode and not an inequality.
	 */
	reset();
	mair0 = 0x000000F0u;
	expect("outer set, inner zero -- reserved, not Normal", run(CTRL_ON),
	       PLUGIN_MPU_ATTR);

	/* AttrIndx picks a different byte, and the index selects MAIR1 above 3. */
	reset();
	good[0].rlar |= (5u << PLUGIN_MPU_RLAR_ATTR_SHIFT);
	mair1 = 0x0000FF00u;                     /* index 5 -> second byte of MAIR1 */
	expect("AttrIndx 5 reads MAIR1", run(CTRL_ON), PLUGIN_MPU_OK);

	reset();
	good[0].rlar |= (5u << PLUGIN_MPU_RLAR_ATTR_SHIFT);
	mair1 = 0u;
	expect("AttrIndx 5 with a zero MAIR1 byte is refused", run(CTRL_ON),
	       PLUGIN_MPU_ATTR);
}

static void test_default_map(void)
{
	printf(" case: the default map\n");

	/* MPU off: the default map governs, and the reservation is in its SRAM. */
	reset();
	expect("MPU disabled, inside the default SRAM span",
	       plugin_mpu_judge(0u, TYPE_2, good, 2u, mair0, mair1, LO, HI),
	       PLUGIN_MPU_OK);

	expect("MPU disabled, outside it",
	       plugin_mpu_judge(0u, TYPE_2, good, 2u, mair0, mair1,
	                        0x60000000u, 0x60001000u),
	       PLUGIN_MPU_DEFAULT_MAP);

	/*
	 * [!] PRIVDEFENA IS PER-ADDRESS, so it may only be leaned on when NO
	 * enabled region touches ANY part of the range.  A partial match never
	 * falls back -- the covered half would take the region's attributes and the
	 * rest the default map's, which is not a thing this loader can reason
	 * about.
	 */
	reset();
	good[0].rlar &= ~PLUGIN_MPU_RLAR_EN;     /* nothing enabled at all */
	expect("enabled MPU, nothing matches, PRIVDEFENA set",
	       run(CTRL_ON | PLUGIN_MPU_CTRL_PRIVDEFENA), PLUGIN_MPU_OK);

	expect("enabled MPU, nothing matches, PRIVDEFENA clear",
	       run(CTRL_ON), PLUGIN_MPU_NO_REGION);

	reset();
	good[0].rbar = 0x341E0020u | (1u << PLUGIN_MPU_RBAR_AP_SHIFT);
	expect("a partial match does NOT fall back to the default map",
	       run(CTRL_ON | PLUGIN_MPU_CTRL_PRIVDEFENA), PLUGIN_MPU_PARTIAL);
}

static void test_arguments(void)
{
	printf(" case: arguments\n");

	reset();
	expect("an empty range", plugin_mpu_judge(CTRL_ON, TYPE_2, good, 2u,
	                                          mair0, mair1, LO, LO),
	       PLUGIN_MPU_ARG);
	expect("a NULL table with the MPU on",
	       plugin_mpu_judge(CTRL_ON, TYPE_2, NULL, 2u, mair0, mair1, LO, HI),
	       PLUGIN_MPU_ARG);

	/* DREGION larger than what the caller actually read must not walk past the
	 * table it was given. */
	reset();
	expect("DREGION beyond the table given is clamped",
	       plugin_mpu_judge(CTRL_ON, 16u << PLUGIN_MPU_TYPE_DREGION_SHIFT,
	                        good, 2u, mair0, mair1, LO, HI),
	       PLUGIN_MPU_OK);
}

int main(void)
{
	printf("test_plugin_mpu (port/plugin/plugin_mpu.c):\n");
	test_ok();
	test_limit_decoding();
	test_coverage();
	test_permissions();
	test_attributes();
	test_default_map();
	test_arguments();

	if (failures != 0) {
		printf("test_plugin_mpu: %d failure(s)\n", failures);
		return 1;
	}
	printf("test_plugin_mpu: all cases pass\n");
	return 0;
}
