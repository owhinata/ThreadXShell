/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    nor_flash.h
 * @brief   Board-owned lifecycle for the external QSPI NOR (issue #86).
 *
 * WHAT THIS OWNS, AND WHY IT IS NOT IN port/npu/ ANY MORE
 *
 * The QSPI master, the memory-mapped read window, and the interrupt the vendor
 * library uses to move data with DMA.  Those used to be brought up inside
 * npu_hw_init()'s EPK snapshot, which made IRQ 133 part of the NPU's wrapset --
 * so `nn close` unwrapped it, and unwrapping DISABLES the line.  The one-way
 * latch in the old npu_flash.c went on saying "initialised" over the top of it.
 * Nothing has noticed because only memory-mapped reads follow today.
 *
 * [!] THERE IS NO TEARDOWN.  The vendor library offers no close for this path,
 * so once the window is up it stays up: OFF is reached once, at boot.  That is
 * a property of the hardware path, not a shortcut -- see nor_state.h.
 *
 * WHAT A CALLER GETS is a reader lease.  While any lease is out the window is
 * readable; issue #88's writer will refuse while one exists, because a write
 * has to drop XIP and the window dies with it.  Each slot is single-instance
 * (nor_state.h), so the token a caller stores is the whole of its claim.
 */
#ifndef NOR_FLASH_H
#define NOR_FLASH_H

#include <stdint.h>

#include "nor_state.h"

#ifdef __cplusplus
extern "C" {
#endif

/** The memory-mapped read alias.  Not live until a lease has been granted. */
#define NOR_XIP_BASE   0x3A000000u
/** W25Q128JW, and what the bootloader reports as flash size[5]. */
#define NOR_SIZE       0x01000000u

/**
 * @brief  Take a reader lease, bringing the window up if nobody has yet.
 *
 * @param slot  which single-instance holder is asking
 * @param out   receives the token to store; set to 0 on any refusal
 * @return 0 on success, -1 if refused (the port is faulted, the slot is already
 *         held, or another caller is mid bring-up)
 *
 * [!] THE CALLER MUST STORE @p out AND HAND IT BACK.  It is the only proof of
 * the claim: nor_release() with a token from a previous lifecycle, or for a
 * slot that is not held, is refused rather than allowed to decrement somebody
 * else's lease.
 *
 * Thread context only -- bring-up waits on DMA completion.
 */
int nor_acquire(enum nor_lease_slot slot, uint32_t *out);

/**
 * @brief  Give a lease back.  Safe to call with a zero token.
 *
 * @return 0 if a lease was dropped, -1 if the token held nothing.  Callers that
 *         run unconditionally -- `nn close` does -- should ignore -1.
 */
int nor_release(uint32_t token);

/** The lifecycle state, for `nor info` and for refusing early. */
enum nor_state nor_lifecycle_state(void);

/** Why the port is faulted, or NULL.  Latched at the first failure. */
const char *nor_fail_reason(void);

/** Raw snapshot for `nor info`; everything here is captured, not decoded. */
struct nor_report {
	uint8_t  jedec[3];        /**< read BEFORE XIP; the vendor refuses after */
	uint8_t  jedec_valid;
	uint32_t scu_xip_before;  /**< SCU_ISP_XIP_SPICACHE across the transition */
	uint32_t scu_xip_after;
	uint32_t mpu_ctrl_s;      /**< architectural, so these ARE decodable      */
	uint32_t mpu_ctrl_ns;
	int32_t  mpu_region;      /**< the region covering the alias, or -1       */
	uint32_t mpu_rbar;
	uint32_t mpu_rlar;
	uint32_t mpu_mair0;
	uint32_t mpu_mair1;
	/* The bring-up probe that proves the window is not a degenerate alias.
	 * [!] It reads the bootloader's slot-header block, NOT `blob` -- issue #90.
	 * A probe inside blob would be reading bytes issue #88's writer is allowed
	 * to erase, so "the window came back" could be answered by the very bytes
	 * the caller had just destroyed. */
	uint32_t probe_off;       /**< flash offset probe B reads                 */
	uint32_t probe_word;      /**< what it read (raw, never decoded)          */
	/* [!] OBSERVED, NOT REQUIRED.  A corrupt slot header still boots -- the
	 * bootloader falls back to slot 0 -- so bring-up must not depend on it.
	 * Reported so a human can see whether it is intact; nothing acts on it. */
	uint8_t  probe_hdr;       /**< 1 if "HIMAXWE2" is at probe_off            */
	int      irq;             /**< the line this port wrapped, or -1          */
	/* [!] The observable for issue #86 itself.  The line used to be part of
	 * the NPU's EPK wrapset, so `nn close` unwrapped it and unwrapping
	 * DISABLES.  `nor info` after an `nn close` is how that is checked on
	 * hardware without a debugger. */
	uint8_t  irq_enabled;
	uint32_t readers;
};

/** Fill @p r.  Valid at any state; fields not established yet read as zero. */
void nor_report(struct nor_report *r);

#ifdef __cplusplus
}
#endif

#endif /* NOR_FLASH_H */
