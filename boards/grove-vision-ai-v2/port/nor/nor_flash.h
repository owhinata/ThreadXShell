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

/* --- what a write transaction borrows from the lifecycle (issue #88) -------
 *
 * These five exist for port/nor/nor_write.c, which owns the transaction, and
 * for nothing else.  They are here rather than there because the window, the
 * lease mask and the fault latch are THIS file's state: a writer that reached
 * into them would be a second owner of the lifecycle, and the one thing issue
 * #86 established is that this part must have exactly one.
 *
 * [!] THEY DO NOT COMPOSE INTO ANYTHING BUT THE SEQUENCE nor_write.h STATES.
 * nor_write_claim() leaves the port in NOR_ST_WRITING -- readable by nobody,
 * writable by one caller -- and only nor_write_commit() gets it out again.  A
 * path that claimed and returned early would strand the part in a state whose
 * every answer is "busy", for the rest of the session.
 */

/**
 * @brief  Claim the part for one write transaction.
 *
 * Reads the state and the lease mask together and publishes NOR_ST_WRITING
 * under the same critical section, which is the whole point: see
 * nor_write_decide() in nor_state.h.  On anything but NOR_WR_GO nothing was
 * claimed and nothing has to be committed.
 */
enum nor_write nor_write_claim(void);

/**
 * @brief  End the transaction claimed above.
 *
 * @param ok   non-zero if everything the transaction promised held
 * @param why  the reason to latch when it did not; may be NULL
 *
 * [!] A FAULT LATCHED EARLIER WINS OVER @p ok.  The window helpers below latch
 * through the same first-failure record nor_fail_reason() reports, so a
 * transaction that lost the window at step 2 and then succeeded at everything
 * afterwards still commits to NOR_ST_FAULTED.  Without that, "ok" from a later
 * step would hand the alias back to readers over a window this port has already
 * said it does not understand.
 */
void nor_write_commit(int ok, const char *why);

/**
 * @brief  Take the memory-mapped window down, and establish that it went.
 *
 * The vendor's enable_XIP calls the SCU and the MPU and returns without
 * checking any of them, so "asked" and "happened" are read back apart here:
 * SCU xip_en and isp_write_en must both read 0 before anything reaches the
 * erase or program path.  @return 0, or -1 with the port already faulted.
 */
int nor_window_drop(void);

/**
 * @brief  Bring the window back, verify it and re-probe it.
 *
 * The same sequence bring-up runs, for the same reason and with the same
 * probes -- both of which sit outside anything a writer may erase (issue #90).
 * @return 0, or -1 with the port already faulted.
 */
int nor_window_restore(void);

/**
 * @brief  Invalidate [off, off+len) of the alias, rounded out to cache lines.
 *
 * [!] REQUIRED BEFORE READING BACK WHAT WAS JUST WRITTEN.  The vendor's XIP
 * restore invalidates 512 bytes at the base of the window and nothing else, so
 * a comparison against a range that was in the cache before the window went
 * down would be answered without the bus being touched at all.
 */
void nor_alias_invalidate(uint32_t off, uint32_t len);

/**
 * @brief  Re-read the JEDEC id with the window down, and check it.
 *
 * @param out  receives the three bytes; zeroed if the part did not answer
 * @return 0 if it answered and agrees with what bring-up recorded, else -1
 *
 * The canary described in nor_write.h.  It uses the same DMA_send_recv path
 * every part of the vendor's write path is built on, and it changes nothing.
 */
int nor_jedec_recheck(uint8_t out[3]);

#ifdef __cplusplus
}
#endif

#endif /* NOR_FLASH_H */
