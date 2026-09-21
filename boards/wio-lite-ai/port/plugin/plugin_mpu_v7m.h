/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    plugin_mpu_v7m.h
 * @brief   Is the plugin reservation executable?  The verdict for an Armv7-M
 *          PMSA, as a pure function (issue #110).
 *
 * [!] THIS IS NOT grove-vision-ai-v2's plugin_mpu.c WITH THE NAMES CHANGED, and
 * the file is called v7m so that nobody reaches for that one.  The two
 * architectures resolve the same question by opposite rules:
 *
 *   - Armv7-M: when two enabled regions cover an address, the HIGHEST-NUMBERED
 *     one wins and the other is simply not consulted.  On Armv8-M an address
 *     covered twice is a fault, so "more than one region matches" is a refusal
 *     there and an ordinary configuration here -- this board's own mpu.c relies
 *     on it, carving a cacheable window out of a larger non-cacheable one.
 *   - Armv7-M has SUB-REGIONS: a region of 256 bytes or more is eight equal
 *     slices and RASR.SRD can switch any of them off, at which point that slice
 *     falls through to whatever is underneath.  Armv8-M has nothing like it.
 *   - The attributes live in different registers with different encodings
 *     (TEX/C/B/S and a 3-bit AP here, MAIR indices and RLAR there).
 *
 * [!] AND THE DEFAULT MAP IS THE ANSWER HERE, NOT A REFUSAL.  Grove requires an
 * explicit region over its reservation.  This board deliberately has none: the
 * four regions mpu.c programs cover the PSRAM window, ITCM and the fenced-off
 * OCTOSPI2 quarter, and AXI-SRAM is left to the background map -- which the
 * Armv7-M architecture defines as Normal, privileged read/write and executable
 * for 0x20000000..0x3FFFFFFF.  So this decodes the default map rather than
 * rejecting it, and what it insists on is that the background map is actually
 * in force: MPU_CTRL.PRIVDEFENA set, or the MPU disabled outright.
 *
 * [!] PURE, BECAUSE THE FAILING CASES CANNOT BE PRODUCED ON THIS HARDWARE.  A
 * board on which the reservation is Device memory, or execute-never, or has a
 * sub-region hole punched in it, is not a board this project can arrange to
 * have -- so a check written inline in the loader would be a check nobody has
 * ever seen say no.  Taking the register values as arguments makes every branch
 * reachable from a host test.  Same reasoning as Grove's, and #42/#66 is what
 * happens without it.
 *
 * [!] AN INCOMPLETE SNAPSHOT IS REFUSED.  Grove's judge clamps MPU_TYPE.DREGION
 * to the size of the table it was handed and carries on, and its host test
 * expects that to succeed.  On this architecture the region it would skip is a
 * HIGHER-numbered one, which is precisely the one that would have won -- so a
 * truncated read cannot answer the question at all.  It says so instead.
 *
 * WHAT THE CALLER MUST DO, AND THIS FILE CANNOT.  The snapshot has to be
 * consistent: MPU_CTRL, MPU_TYPE and every region read without the MPU changing
 * underneath, and a `DSB; ISB` between the read and the branch.
 */
#ifndef PLUGIN_MPU_V7M_H
#define PLUGIN_MPU_V7M_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* MPU_CTRL */
#define PL_MPU7_CTRL_ENABLE      (1u << 0)
#define PL_MPU7_CTRL_HFNMIENA    (1u << 1)
#define PL_MPU7_CTRL_PRIVDEFENA  (1u << 2)

/* MPU_TYPE */
#define PL_MPU7_TYPE_DREGION_SHIFT 8
#define PL_MPU7_TYPE_DREGION_MASK  0xFFu

/* RASR */
#define PL_MPU7_RASR_ENABLE      (1u << 0)
#define PL_MPU7_RASR_SIZE_SHIFT  1
#define PL_MPU7_RASR_SIZE_MASK   0x1Fu
#define PL_MPU7_RASR_SRD_SHIFT   8
#define PL_MPU7_RASR_SRD_MASK    0xFFu
#define PL_MPU7_RASR_B           (1u << 16)
#define PL_MPU7_RASR_C           (1u << 17)
#define PL_MPU7_RASR_S           (1u << 18)
#define PL_MPU7_RASR_TEX_SHIFT   19
#define PL_MPU7_RASR_TEX_MASK    0x7u
#define PL_MPU7_RASR_AP_SHIFT    24
#define PL_MPU7_RASR_AP_MASK     0x7u
#define PL_MPU7_RASR_XN          (1u << 28)

/* RBAR */
#define PL_MPU7_RBAR_ADDR_MASK   0xFFFFFFE0u

/** One region, as read back through MPU_RNR. */
struct pl_mpu7_region {
	uint32_t rbar;
	uint32_t rasr;
};

/**
 * Most regions this decoder will look at.  MPU_TYPE.DREGION is 8 bits wide;
 * this core implements 16 and no Armv7-M implementation defines more.  A
 * DREGION larger than this is not clamped -- see PL_MPU7_TRUNCATED.
 */
#define PL_MPU7_REGION_MAX 16u

enum pl_mpu7_verdict {
	PL_MPU7_OK = 0,
	PL_MPU7_ARG,          /**< no range, or an inverted one                */
	PL_MPU7_TRUNCATED,    /**< DREGION exceeds the table handed over       */
	PL_MPU7_NO_DEFAULT,   /**< nothing covers it and PRIVDEFENA is clear   */
	PL_MPU7_XN,           /**< execute-never                               */
	PL_MPU7_AP,           /**< not privileged read/write                   */
	PL_MPU7_ATTR,         /**< not Normal memory, or a reserved encoding   */
	PL_MPU7_REGION_BAD,   /**< a region is malformed: size, alignment, SRD */
};

/**
 * @brief  Decide whether [@p lo, @p hi) is privileged-RW, executable, Normal
 *         memory under this MPU configuration.
 *
 * @param ctrl     MPU_CTRL
 * @param type     MPU_TYPE
 * @param rgn      regions 0..@p n-1, read through MPU_RNR
 * @param n        how many entries @p rgn holds
 * @param lo, hi   the reservation, half-open
 *
 * Every 32-byte granule of the range is resolved separately, because that is
 * the finest granularity the architecture can express -- a region base is
 * 32-byte aligned and the smallest sub-region of the smallest sub-regioned
 * region is 32 bytes.  A range that is mostly fine and has one hole in it is
 * exactly the configuration a coarser check would pass.
 */
enum pl_mpu7_verdict pl_mpu7_judge(uint32_t ctrl, uint32_t type,
                                   const struct pl_mpu7_region *rgn, unsigned n,
                                   uint32_t lo, uint32_t hi);

/** Short description of a verdict (never NULL). */
const char *pl_mpu7_strerror(enum pl_mpu7_verdict v);

#ifdef __cplusplus
}
#endif

#endif /* PLUGIN_MPU_V7M_H */
