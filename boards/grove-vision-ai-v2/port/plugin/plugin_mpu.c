/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    plugin_mpu.c
 * @brief   The MPU verdict.  See plugin_mpu.h.
 */
#include "plugin_mpu.h"

#include <stddef.h>

/*
 * The default memory map's Normal, executable span.
 *
 * Armv8-M gives 0x20000000..0x3FFFFFFF as SRAM: Normal memory, not execute-
 * never.  The plugin reservation is inside it, which is why a board with the
 * MPU disabled -- or with PRIVDEFENA set and nothing covering the range -- is
 * still allowed to execute there.  Stated rather than assumed, so that a
 * reservation moved outside this span stops being silently blessed.
 */
#define DEFAULT_SRAM_LO 0x20000000u
#define DEFAULT_SRAM_HI 0x3FFFFFFFu   /* inclusive */

/*
 * Is this MAIR byte Normal memory?
 *
 * [!] "NOT DEVICE" IS NOT THE SAME QUESTION.  The encoding is outer[7:4] and
 * inner[3:0].  Outer == 0b0000 selects the Device types -- that much a
 * not-Device test would catch -- but outer != 0 with inner == 0b0000 is
 * RESERVED, and a test that only excluded Device would accept it.  Only both
 * halves non-zero is architecturally Normal.
 */
static int mair_is_normal(uint8_t attr)
{
	return (attr >> 4) != 0u && (attr & 0x0Fu) != 0u;
}

static uint8_t mair_byte(uint32_t mair0, uint32_t mair1, unsigned idx)
{
	uint32_t w = (idx < 4u) ? mair0 : mair1;

	return (uint8_t)((w >> (8u * (idx & 3u))) & 0xFFu);
}

/* Privileged execution needs to read AND write: the loader zeroes .bss and the
 * plugin writes its own data while it runs.  RBAR.AP: 0b00 RW privileged only,
 * 0b01 RW any, 0b10 RO privileged only, 0b11 RO any. */
static int ap_allows_priv_rw(uint32_t rbar)
{
	uint32_t ap = (rbar >> PLUGIN_MPU_RBAR_AP_SHIFT) & PLUGIN_MPU_RBAR_AP_MASK;

	return ap == 0u || ap == 1u;
}

enum plugin_mpu_verdict plugin_mpu_judge(uint32_t ctrl, uint32_t type,
                                         const struct plugin_mpu_region *rgn,
                                         unsigned nregion,
                                         uint32_t mair0, uint32_t mair1,
                                         uint32_t lo, uint32_t hi)
{
	unsigned dregion, i, hits = 0u;
	unsigned covering = 0u;
	uint32_t last;
	int covered = 0;

	if (hi <= lo)
		return PLUGIN_MPU_ARG;
	last = hi - 1u;                     /* inclusive, to compare with limits */

	if ((ctrl & PLUGIN_MPU_CTRL_ENABLE) == 0u) {
		/* No MPU: the default map decides, and only the SRAM span is Normal
		 * and executable. */
		return (lo >= DEFAULT_SRAM_LO && last <= DEFAULT_SRAM_HI)
		       ? PLUGIN_MPU_OK : PLUGIN_MPU_DEFAULT_MAP;
	}

	if (rgn == NULL)
		return PLUGIN_MPU_ARG;

	dregion = (type >> PLUGIN_MPU_TYPE_DREGION_SHIFT)
	          & PLUGIN_MPU_TYPE_DREGION_MASK;
	if (dregion > PLUGIN_MPU_REGION_MAX)
		dregion = PLUGIN_MPU_REGION_MAX;
	if (dregion > nregion)
		dregion = nregion;              /* never read past what we were given */

	for (i = 0u; i < dregion; i++) {
		uint32_t base, limit;

		if ((rgn[i].rlar & PLUGIN_MPU_RLAR_EN) == 0u)
			continue;

		base  = rgn[i].rbar & PLUGIN_MPU_RBAR_BASE_MASK;
		/* The region runs to the END of its last 32-byte block. */
		limit = (rgn[i].rlar & PLUGIN_MPU_RLAR_LIMIT_MASK) | 0x1Fu;

		if (limit < lo || base > last)
			continue;                   /* no intersection */
		hits++;
		if (base <= lo && limit >= last) {
			covered  = 1;
			covering = i;
		}
	}

	if (hits == 0u) {
		/* [!] PRIVDEFENA IS PER-ADDRESS, and we have already established that
		 * NO enabled region touches ANY address in the range -- which is the
		 * only shape in which the default map may be relied on.  A partial
		 * match is handled below and never falls back. */
		if ((ctrl & PLUGIN_MPU_CTRL_PRIVDEFENA) == 0u)
			return PLUGIN_MPU_NO_REGION;
		return (lo >= DEFAULT_SRAM_LO && last <= DEFAULT_SRAM_HI)
		       ? PLUGIN_MPU_OK : PLUGIN_MPU_DEFAULT_MAP;
	}

	if (hits > 1u)
		return PLUGIN_MPU_MULTIPLE;     /* PMSAv8 faults; it does not resolve */

	if (!covered)
		return PLUGIN_MPU_PARTIAL;

	if ((rgn[covering].rbar & PLUGIN_MPU_RBAR_XN) != 0u)
		return PLUGIN_MPU_XN;
	if ((rgn[covering].rlar & PLUGIN_MPU_RLAR_PXN) != 0u)
		return PLUGIN_MPU_PXN;
	if (!ap_allows_priv_rw(rgn[covering].rbar))
		return PLUGIN_MPU_AP;
	if (!mair_is_normal(mair_byte(mair0, mair1,
	                              (rgn[covering].rlar
	                               >> PLUGIN_MPU_RLAR_ATTR_SHIFT)
	                              & PLUGIN_MPU_RLAR_ATTR_MASK)))
		return PLUGIN_MPU_ATTR;

	return PLUGIN_MPU_OK;
}

const char *plugin_mpu_strerror(enum plugin_mpu_verdict v)
{
	switch (v) {
	case PLUGIN_MPU_OK:          return "ok";
	case PLUGIN_MPU_ARG:         return "bad argument";
	case PLUGIN_MPU_NO_REGION:   return "no region covers it and no default map";
	case PLUGIN_MPU_PARTIAL:     return "a region covers only part of it";
	case PLUGIN_MPU_MULTIPLE:    return "more than one region intersects it";
	case PLUGIN_MPU_XN:          return "execute-never";
	case PLUGIN_MPU_PXN:         return "privileged execute-never";
	case PLUGIN_MPU_AP:          return "not privileged read/write";
	case PLUGIN_MPU_ATTR:        return "Device or reserved memory attributes";
	case PLUGIN_MPU_DEFAULT_MAP: return "outside the default map's Normal SRAM";
	}
	return "unknown";
}
