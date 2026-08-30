/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    plugin_mpu.h
 * @brief   Is the plugin reservation executable?  The verdict, as a pure
 *          function (issue #103).
 *
 * The loader reads the MPU back immediately before it branches into a loaded
 * image, because the vendor's `enable_XIP()` reconfigures the MPU and this port
 * therefore cannot assume the reservation is still Normal, readable and
 * executable (port/nor/nor_flash.c makes the same observation for the XIP
 * window and captures the MPU for the same reason).
 *
 * [!] PURE, BECAUSE THE FAILING CASES CANNOT BE PRODUCED ON THIS HARDWARE.
 * A board on which the reservation is Device memory, or covered by two regions,
 * or execute-never, is not a board this project can arrange to have -- so a
 * check written inline in the loader would be a check nobody has ever seen say
 * no.  Taking the register values as arguments makes every branch reachable
 * from a host test.  Same reasoning as fp_enforce.h, and #42/#66 is what
 * happens without it.
 *
 * WHAT THE CALLER MUST DO, AND THIS FILE CANNOT.  The snapshot has to be
 * consistent: MPU_CTRL, MPU_TYPE, every region and both MAIRs must be read
 * without the MPU changing underneath, and a `DSB; ISB` must separate the read
 * from the branch.  Holding the reservation stable for the whole of a callback
 * is a different problem again, and it is already solved -- see the loader.
 */
#ifndef PLUGIN_MPU_H
#define PLUGIN_MPU_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* MPU_CTRL */
#define PLUGIN_MPU_CTRL_ENABLE     (1u << 0)
#define PLUGIN_MPU_CTRL_HFNMIENA   (1u << 1)
#define PLUGIN_MPU_CTRL_PRIVDEFENA (1u << 2)

/* MPU_TYPE */
#define PLUGIN_MPU_TYPE_DREGION_SHIFT 8
#define PLUGIN_MPU_TYPE_DREGION_MASK  0xFFu

/* RBAR */
#define PLUGIN_MPU_RBAR_XN         (1u << 0)
#define PLUGIN_MPU_RBAR_AP_SHIFT   1
#define PLUGIN_MPU_RBAR_AP_MASK    0x3u
#define PLUGIN_MPU_RBAR_BASE_MASK  0xFFFFFFE0u

/* RLAR */
#define PLUGIN_MPU_RLAR_EN         (1u << 0)
#define PLUGIN_MPU_RLAR_ATTR_SHIFT 1
#define PLUGIN_MPU_RLAR_ATTR_MASK  0x7u
#define PLUGIN_MPU_RLAR_PXN        (1u << 4)
#define PLUGIN_MPU_RLAR_LIMIT_MASK 0xFFFFFFE0u

/** One region, as read from the Secure bank. */
struct plugin_mpu_region {
	uint32_t rbar;
	uint32_t rlar;
};

/** Most regions this decoder will look at.  MPU_TYPE.DREGION is 8 bits, but no
 *  Armv8-M implementation defines more than 16 and this port has no reason to
 *  walk a table that large. */
#define PLUGIN_MPU_REGION_MAX 16u

enum plugin_mpu_verdict {
	PLUGIN_MPU_OK = 0,
	PLUGIN_MPU_ARG,        /**< nonsense arguments                        */
	PLUGIN_MPU_NO_REGION,  /**< enabled, no default map, nothing covers   */
	PLUGIN_MPU_PARTIAL,    /**< a region covers only part of the range    */
	PLUGIN_MPU_MULTIPLE,   /**< more than one enabled region intersects   */
	PLUGIN_MPU_XN,         /**< execute-never                             */
	PLUGIN_MPU_PXN,        /**< privileged execute-never                  */
	PLUGIN_MPU_AP,         /**< not privileged read/write                 */
	PLUGIN_MPU_ATTR,       /**< Device, or a reserved MAIR encoding       */
	PLUGIN_MPU_DEFAULT_MAP /**< the default map does not make it Normal   */
};

/**
 * @brief  Decide whether @p lo .. @p hi (exclusive) may be executed from.
 *
 * @param ctrl     MPU_CTRL
 * @param type     MPU_TYPE; only DREGION is used
 * @param rgn      the region table, @p nregion entries, Secure bank
 * @param nregion  entries in @p rgn; the caller clamps DREGION to what it read
 * @param mair0    MPU_MAIR0
 * @param mair1    MPU_MAIR1
 * @param lo, hi   the range, @p hi exclusive
 *
 * [!] ARMv8-M HAS NO "HIGHEST REGION WINS".  PMSAv7 resolved overlap by region
 * number; PMSAv8 does not -- a matching address in more than one enabled region
 * invalidates the match and the access faults.  So more than one intersecting
 * region is a refusal, not something to resolve.
 *
 * [!] AND THE LIMIT INCLUDES ITS LAST 32-BYTE BLOCK.  RLAR holds the limit with
 * the low five bits stripped, and the region runs to the end of that block:
 * `limit = (RLAR & LIMIT_MASK) | 0x1F`.  Comparing against the masked value
 * alone -- which port/nor/nor_flash.c's diagnostic capture does -- makes a
 * region look 31 bytes shorter than it is, and a reservation that ends exactly
 * at a region's end would be reported as only partly covered.
 */
enum plugin_mpu_verdict plugin_mpu_judge(uint32_t ctrl, uint32_t type,
                                         const struct plugin_mpu_region *rgn,
                                         unsigned nregion,
                                         uint32_t mair0, uint32_t mair1,
                                         uint32_t lo, uint32_t hi);

/** @return a short description of a verdict (never NULL). */
const char *plugin_mpu_strerror(enum plugin_mpu_verdict v);

#ifdef __cplusplus
}
#endif

#endif /* PLUGIN_MPU_H */
