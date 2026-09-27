/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    plugin_mpu_v7m.c
 * @brief   The Armv7-M verdict.  See plugin_mpu_v7m.h.
 *
 * References are to the ARMv7-M Architecture Reference Manual (DDI 0403E):
 * B3.5 for the PMSA, Table B3-1 for the default memory map, Table B3-13 for
 * the TEX/C/B encoding and Table B3-15 for AP.
 */
#include "plugin_mpu_v7m.h"

#include <stddef.h>

/* The finest granularity the architecture can express: a region base is
 * 32-byte aligned, and the smallest sub-region of the smallest sub-regioned
 * region (256 bytes / 8) is 32 bytes. */
#define GRANULE 32u

/*
 * What one region says about an address, or nothing.
 *
 * [!] "NOTHING" IS A THIRD ANSWER, NOT A REFUSAL.  A disabled sub-region falls
 * THROUGH: a lower-numbered region may still cover the address, and if none
 * does the background map may.  Treating a hole as a denial would refuse a
 * configuration the architecture permits.
 */
enum region_says {
	SAYS_NOTHING = 0,   /* does not cover this address (or a hole in it)  */
	SAYS_YES,           /* privileged RW, executable, Normal              */
	SAYS_XN,
	SAYS_AP,
	SAYS_ATTR,
};

/* Table B3-13.  Normal memory, or not -- reserved encodings are not. */
static int attr_is_normal(uint32_t rasr)
{
	uint32_t tex = (rasr >> PL_MPU7_RASR_TEX_SHIFT) & PL_MPU7_RASR_TEX_MASK;
	uint32_t c   = (rasr & PL_MPU7_RASR_C) ? 1u : 0u;
	uint32_t b   = (rasr & PL_MPU7_RASR_B) ? 1u : 0u;

	/* TEX[2] == 1 is "cacheable Normal memory", inner policy from C,B and
	 * outer from TEX[1:0].  Every combination of those four bits is Normal. */
	if (tex & 0x4u)
		return 1;

	switch (tex) {
	case 0u:
		/* 000: 00 strongly-ordered, 01 device, 1x Normal (WT / WB). */
		return c == 1u;
	case 1u:
		/* 001: 00 Normal non-cacheable, 11 Normal WBWA; 01 and 10 reserved. */
		if (c == 0u && b == 0u)
			return 1;
		if (c == 1u && b == 1u)
			return 1;
		return 0;
	case 2u:
		/* 010: 00 Device non-shareable; everything else reserved. */
		return 0;
	default:
		/* 011 is reserved. */
		return 0;
	}
}

/* Table B3-15.  Privileged read/write, whatever unprivileged gets. */
static int ap_allows_priv_rw(uint32_t rasr)
{
	uint32_t ap = (rasr >> PL_MPU7_RASR_AP_SHIFT) & PL_MPU7_RASR_AP_MASK;

	return ap == 1u || ap == 2u || ap == 3u;
}

/*
 * Is @p rgn well formed?  A region the architecture calls UNPREDICTABLE cannot
 * be judged, so it is refused rather than interpreted.
 */
static int region_is_sane(const struct pl_mpu7_region *r)
{
	uint32_t size = (r->rasr >> PL_MPU7_RASR_SIZE_SHIFT) & PL_MPU7_RASR_SIZE_MASK;
	uint32_t srd  = (r->rasr >> PL_MPU7_RASR_SRD_SHIFT) & PL_MPU7_RASR_SRD_MASK;
	uint32_t base = r->rbar & PL_MPU7_RBAR_ADDR_MASK;
	uint64_t bytes;

	if (!(r->rasr & PL_MPU7_RASR_ENABLE))
		return 1;                /* disabled regions say nothing at all */

	/* SIZE encodes 2^(SIZE+1) bytes, and below 32 bytes is not a region.  In
	 * 64-bit arithmetic, because SIZE 31 is the whole 4 GiB and that does not
	 * fit the width of the addresses it describes. */
	if (size < 4u || size > 31u)
		return 0;
	bytes = (uint64_t)2u << size;

	/* Naturally aligned, or UNPREDICTABLE. */
	if (((uint64_t)base & (bytes - 1u)) != 0u)
		return 0;

	/* [!] SUB-REGIONS BELOW 256 BYTES ARE UNPREDICTABLE, not ignored.  A
	 * region of 32, 64 or 128 bytes with SRD set is a configuration with no
	 * defined meaning, and guessing one is how a check starts passing for a
	 * reason nobody chose. */
	if (size < 7u && srd != 0u)
		return 0;

	return 1;
}

static enum region_says region_at(const struct pl_mpu7_region *r, uint32_t addr)
{
	uint32_t size = (r->rasr >> PL_MPU7_RASR_SIZE_SHIFT) & PL_MPU7_RASR_SIZE_MASK;
	uint32_t srd  = (r->rasr >> PL_MPU7_RASR_SRD_SHIFT) & PL_MPU7_RASR_SRD_MASK;
	uint32_t base = r->rbar & PL_MPU7_RBAR_ADDR_MASK;
	uint64_t bytes, off;

	if (!(r->rasr & PL_MPU7_RASR_ENABLE))
		return SAYS_NOTHING;

	bytes = (uint64_t)2u << size;          /* 64-bit: SIZE 31 is 4 GiB */
	if ((uint64_t)addr < (uint64_t)base ||
	    (uint64_t)addr >= (uint64_t)base + bytes)
		return SAYS_NOTHING;

	/* A sub-region that is switched off is a hole, and a hole says nothing. */
	if (size >= 7u) {
		off = ((uint64_t)addr - (uint64_t)base) / (bytes / 8u);
		if (srd & (1u << (unsigned)off))
			return SAYS_NOTHING;
	}

	if (r->rasr & PL_MPU7_RASR_XN)
		return SAYS_XN;
	if (!ap_allows_priv_rw(r->rasr))
		return SAYS_AP;
	if (!attr_is_normal(r->rasr))
		return SAYS_ATTR;
	return SAYS_YES;
}

/*
 * Table B3-1.  What the background map says, for privileged access.
 *
 * Only the two questions this file asks: is it Normal memory, and may it be
 * executed?  Privileged read/write holds across the whole default map.
 */
static enum region_says default_map_at(uint32_t addr)
{
	/* Code and the two RAM quarters are Normal and executable; the SRAM
	 * quarter this board's reservation lives in is one of them. */
	if (addr < 0x20000000u)            /* Code   */
		return SAYS_YES;
	if (addr < 0x40000000u)            /* SRAM   */
		return SAYS_YES;
	if (addr < 0x60000000u)            /* Peripheral: Device, XN */
		return SAYS_XN;
	if (addr < 0xA0000000u)            /* RAM x2 */
		return SAYS_YES;
	return SAYS_XN;                    /* Device, PPB, vendor: all XN */
}

static enum pl_mpu7_verdict says_to_verdict(enum region_says s)
{
	switch (s) {
	case SAYS_XN:   return PL_MPU7_XN;
	case SAYS_AP:   return PL_MPU7_AP;
	case SAYS_ATTR: return PL_MPU7_ATTR;
	default:        return PL_MPU7_OK;
	}
}

enum pl_mpu7_verdict pl_mpu7_judge(uint32_t ctrl, uint32_t type,
                                   const struct pl_mpu7_region *rgn, unsigned n,
                                   uint32_t lo, uint32_t hi)
{
	unsigned dregion, i;
	uint32_t addr;

	if (hi <= lo || (lo % GRANULE) != 0u || (hi % GRANULE) != 0u)
		return PL_MPU7_ARG;

	dregion = (unsigned)((type >> PL_MPU7_TYPE_DREGION_SHIFT) &
	                     PL_MPU7_TYPE_DREGION_MASK);

	/*
	 * [!] REFUSED, NOT CLAMPED.  The region that was left out is a
	 * higher-numbered one, and on this architecture higher numbers WIN -- so a
	 * truncated snapshot is not a partial answer, it is the wrong one.
	 */
	if (dregion > n || dregion > PL_MPU7_REGION_MAX)
		return PL_MPU7_TRUNCATED;
	if (rgn == NULL && dregion != 0u)
		return PL_MPU7_ARG;

	for (i = 0u; i < dregion; i++)
		if (!region_is_sane(&rgn[i]))
			return PL_MPU7_REGION_BAD;

	/*
	 * With the MPU off, the default map is what is in force everywhere -- and
	 * that is a legitimate configuration, not an absence of one.
	 */
	if (!(ctrl & PL_MPU7_CTRL_ENABLE))
		dregion = 0u;

	for (addr = lo; addr < hi; addr += GRANULE) {
		enum region_says s = SAYS_NOTHING;

		/* [!] HIGHEST-NUMBERED MATCH WINS, so walk downwards and stop at the
		 * first region that says anything.  This board's own mpu.c depends on
		 * it: region 3 carves a cacheable window out of region 0. */
		for (i = dregion; i-- > 0u; ) {
			s = region_at(&rgn[i], addr);
			if (s != SAYS_NOTHING)
				break;
		}

		if (s == SAYS_NOTHING) {
			/* Nothing covers it.  The background map applies only when it is
			 * switched on, or when the MPU is off entirely. */
			if ((ctrl & PL_MPU7_CTRL_ENABLE) &&
			    !(ctrl & PL_MPU7_CTRL_PRIVDEFENA))
				return PL_MPU7_NO_DEFAULT;
			s = default_map_at(addr);
		}

		if (s != SAYS_YES)
			return says_to_verdict(s);
	}

	return PL_MPU7_OK;
}

const char *pl_mpu7_strerror(enum pl_mpu7_verdict v)
{
	switch (v) {
	case PL_MPU7_OK:         return "ok";
	case PL_MPU7_ARG:        return "bad range";
	case PL_MPU7_TRUNCATED:  return "the MPU snapshot is incomplete";
	case PL_MPU7_NO_DEFAULT: return "uncovered, and the background map is off";
	case PL_MPU7_XN:         return "execute-never";
	case PL_MPU7_AP:         return "not privileged read/write";
	case PL_MPU7_ATTR:       return "not Normal memory";
	case PL_MPU7_REGION_BAD: return "a region is malformed";
	}
	return "unknown";
}
