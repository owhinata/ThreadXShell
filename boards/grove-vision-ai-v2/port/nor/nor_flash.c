/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    nor_flash.c
 * @brief   QSPI NOR lifecycle: bring-up, leases, postconditions (issue #86).
 *
 * The sequencing and everything that touches hardware.  The PRECEDENCE -- who
 * may bring up, whose release may drop a lease -- is in nor_state.c, as pure
 * functions the host test walks.
 *
 * THE BRING-UP IS STAGED, AND THE STAGES ARE NOT INTERCHANGEABLE
 *
 *   1. under PRIMASK: snapshot NVIC->ISER
 *   2. still masked: the vendor's QSPI open, and nothing else
 *   3. still masked: wrap every line the open turned on
 *   4. restore PRIMASK
 *   5. read the JEDEC id
 *   6. enable XIP
 *   7. verify, then probe
 *
 * [!] STEP 3 CANNOT MOVE EARLIER.  IRQ 133 is DISCOVERED by the very call that
 * enables it -- hx_lib_spi_eeprom_open() turns on DMAC1's combined interrupt
 * because the QSPI library moves data with DMA.  "Snapshot and wrap before
 * anything opens QSPI" is not a thing that can be done.
 *
 * [!] AND STEP 4 CANNOT MOVE LATER.  The XIP setup in step 6 reaches the
 * vendor's quad-enable, whose DMA helpers spin on flags that only a completion
 * interrupt clears.  Running that with PRIMASK still set would deadlock.
 *
 * [!] STEP 5 CANNOT MOVE AFTER STEP 6.  hx_lib_qspi_eeprom_read_ID() carries
 * the same XIP guard as the write entry points and returns the same refusal
 * once the window is up -- verified by disassembly of the inner form, which
 * branches to mvn.w r0,#27.
 * So the id is read here or never, and `nor info` reports it from cache.
 *
 * WHAT THE VENDOR DOES BEHIND STEP 6, WHICH THIS PORT DOES NOT
 *
 * hx_lib_qspi_eeprom_enable_XIP() calls EPII_QSPIXIP_MEM_Attribute_S and _NS,
 * EPII_MPU_Enable and _NS, hx_drv_scu_set_xip_en, hx_drv_scu_set_isp_write_en
 * and hx_InvalidateDCache_by_Addr -- and returns without checking any of them.
 * "This port does not configure the MPU" was true of our code and false of the
 * path we call.  So step 7 reads the MPU back architecturally, captures the SCU
 * word raw (the SVD gives the register but no field breakdown, and this board's
 * rule is to dump rather than decode what it cannot name), and only then
 * probes.
 *
 * The probe checks CONTENT, not just that two addresses differ.  The old
 * two-word probe caught the failure that actually happened -- a degenerate
 * window aliasing one register block across all 16 MB -- but "the flash image
 * starts with the Himax container magic" is evidence about the same question
 * that a register block cannot accidentally satisfy.
 */
#include "nor_flash.h"

#include <string.h>

#include "WE2_device.h"          /* CMSIS core: MPU, PRIMASK, barriers        */
#include "spi_eeprom_comm.h"

#include "WE2_core.h"        /* hx_InvalidateDCache_by_Addr */
#include "hx_drv_scu.h"     /* the xip_en / isp_write_en read-backs   */

#include "epk_irq_wrap.h"

#define LOG_TAG "nor"
#include "log.h"

/* SCU_HSC.SCU_ISP_XIP_SPICACHE.  Address from the SVD (WE2_S.svd, peripheral
 * SCU_HSC base 0x53070000, register offset 0xB00); the SVD gives no field
 * breakdown, so this is captured and reported, never decoded. */
#define SCU_HSC_BASE            0x53070000u
#define SCU_ISP_XIP_SPICACHE    (SCU_HSC_BASE + 0xB00u)

/* Every Himax .img begins with this container magic, and the firmware image is
 * what lives at flash offset 0.  Little-endian "ckBS". */
#define NOR_IMAGE_MAGIC         0x53426B63u

/* The two words the bring-up probe compares.  A degenerate window reads the
 * same everywhere, so two offsets whose contents differ are what distinguishes
 * "the flash is mapped" from "one register block is aliased across 16 MB".
 *
 * [!] THE SECOND ONE USED TO BE 0x00B00000, AND THAT IS INSIDE blob (issue
 * #90).  11 MB in was chosen because that is where the aliasing showed itself,
 * with no thought for who owns those bytes -- and blob is exactly the region
 * issue #88's writer is allowed to erase.  The check that says "the window came
 * back" would have been reading bytes the caller had just destroyed.
 *
 * The slot-header block is the one part of this flash nothing may ever write
 * (issue #85).  It is also the highest address in the part, so its address bits
 * differ from word 0 in more places than 0xB00000 did, and its content is known.
 */
#define NOR_PROBE_A_OFF         0u                 /* the firmware image     */
#define NOR_PROBE_B_OFF         NOR_PART_TAIL_END  /* the backup slot header */

/* [!] EVERY PROBE MUST LIE OUTSIDE THE INTERVAL A WRITER MAY ERASE.  Stated
 * once, as a rule over both offsets, so that moving a partition edge or adding
 * a third probe has to answer it rather than quietly reintroduce issue #90.
 * blob is [NOR_PART_FW_END, NOR_PART_BLOB_END) -- see board.cmake. */
#define NOR_PROBE_OUTSIDE_BLOB(off) \
	((off) < NOR_PART_FW_END || (off) >= NOR_PART_BLOB_END)
_Static_assert(NOR_PROBE_OUTSIDE_BLOB(NOR_PROBE_A_OFF),
               "XIP probe A reads flash a writer may erase (issue #90)");
_Static_assert(NOR_PROBE_OUTSIDE_BLOB(NOR_PROBE_B_OFF),
               "XIP probe B reads flash a writer may erase (issue #90)");

/* What the bootloader's slot header starts with, at flash_end - 0x1000 and
 * again at flash_end - 0x2000 (issue #85).  Little-endian "HIMA" "XWE2" -- the
 * same two words the 1st bootloader builds with movw/movt to compare.
 *
 * [!] OBSERVED, NOT REQUIRED.  A corrupt header still boots: the bootloader
 * says so and falls back to slot 0.  Making bring-up depend on a record this
 * port does not own would turn somebody else's recoverable damage into our
 * unrecoverable refusal, so `nor info` reports the match and nothing acts on
 * it.  The probe's acceptance condition is only that the two words differ. */
#define NOR_SLOT_HDR_MAGIC0     0x414D4948u
#define NOR_SLOT_HDR_MAGIC1     0x32455758u

static struct {
	enum nor_state state;
	uint32_t       live;        /* bitmask of slots holding a lease */
	uint32_t       gen;
	uint32_t       owner;       /* reservation token, or 0 (issue #91) */
	uint32_t       rsv_seq;     /* bumped per reservation, never reused */
	const char    *fail;
	struct epk_irq_wrapset irqs;
	struct nor_report      rep;
} nor;

static int fail(const char *why)
{
	if (nor.fail == NULL)
		nor.fail = why;
	nor.state = NOR_ST_FAULTED;
	LOG_ERR("%s", why);
	return -1;
}

/* This part's D-cache line, as npu_cache.c already assumes. */
#define NOR_CACHE_LINE          32u

/*
 * Invalidate [off, off+len) of the alias, rounded out to whole cache lines.
 *
 * [!] THE VENDOR DOES NOT DO THIS FOR US, AND NOT BECAUSE IT FORGOT ONE CALL.
 * hx_lib_qspi_eeprom_enable_XIP() ends with
 * hx_InvalidateDCache_by_Addr(alias_base, 512) -- five hundred and twelve
 * bytes, at the base, regardless of what the caller is about to read.  Word 0
 * happens to fall inside that; the second probe does not, and neither would any
 * range issue #88's writer had just changed.  So a read after XIP comes back up
 * can be answered from a line cached before it went down: the check that says
 * "the window is healthy" would never reach the bus.
 *
 * Today that is latent rather than live -- bring-up runs once, on a window that
 * has not existed before, so there is nothing cached to be stale.  It is being
 * fixed here anyway because the whole reason the probe exists is to not take
 * the window's health on trust, and "no path caches this line yet" is exactly
 * the kind of assumption that stops being true when the writer lands.
 */
static void invalidate_alias(uint32_t off, uint32_t len)
{
	uint32_t lo = (NOR_XIP_BASE + off) & ~(NOR_CACHE_LINE - 1u);
	uint32_t hi = (NOR_XIP_BASE + off + len + NOR_CACHE_LINE - 1u) &
	              ~(NOR_CACHE_LINE - 1u);

	hx_InvalidateDCache_by_Addr((volatile void *)(uintptr_t)lo,
	                            (int32_t)(hi - lo));
}

static uint32_t rd32(uint32_t addr)
{
	return *(volatile const uint32_t *)addr;
}

/* Find the MPU region covering the alias, if any, and record it.  Architectural
 * registers, so this is decodable -- unlike the SCU word above.  Returns the
 * region number or -1. */
static int32_t mpu_capture(struct nor_report *r)
{
	uint32_t saved = MPU->RNR;
	int32_t  found = -1;

	r->mpu_ctrl_s  = MPU->CTRL;
	r->mpu_mair0   = MPU->MAIR0;
	r->mpu_mair1   = MPU->MAIR1;
	r->mpu_ctrl_ns = MPU_NS->CTRL;

	/* Searched rather than assumed: the vendor happens to use one particular
	 * region today, and a number this port hardcoded would go quietly wrong
	 * the day an SDK pin moved it. */
	for (uint32_t i = 0u; i < 16u; i++) {
		uint32_t base, limit;

		MPU->RNR = i;
		base  = MPU->RBAR & MPU_RBAR_BASE_Msk;
		limit = MPU->RLAR & MPU_RLAR_LIMIT_Msk;
		if ((MPU->RLAR & MPU_RLAR_EN_Msk) == 0u)
			continue;
		if (NOR_XIP_BASE >= base && NOR_XIP_BASE <= limit) {
			found      = (int32_t)i;
			r->mpu_rbar = MPU->RBAR;
			r->mpu_rlar = MPU->RLAR;
			break;
		}
	}
	MPU->RNR = saved;
	r->mpu_region = found;
	return found;
}

/* Steps 1-3: the only part that must run masked, and the smallest it can be. */
static int open_and_wrap(void)
{
	struct epk_irq_snapshot snap;
	uint32_t pm = __get_PRIMASK();
	int      rc, wrapped;

	__disable_irq();
	grove_epk_irq_snapshot(&snap);
	rc = hx_lib_spi_eeprom_open(USE_DW_SPI_MST_Q);

	/* [!] THE DIFF IS TAKEN WHATEVER open() RETURNED.  Today it cannot fail
	 * after touching hardware -- the pinned archive's inner form has exactly
	 * one exit, `movs r0,#0; pop` -- so the wrap would run anyway.  But that is
	 * a fact about an SDK pin, and the invariant it protects ("every enabled
	 * line is accounted") must not depend on re-checking it after every bump.
	 * So: wrap what appeared; if the open failed, unwrap, which disables. */
	wrapped = grove_epk_irq_wrap_new(&snap, &nor.irqs);
	if (!wrapped || rc != 0)
		grove_epk_irq_unwrap_set(&nor.irqs);
	__set_PRIMASK(pm);

	if (rc != 0)
		return fail("QSPI open failed");
	if (!wrapped)
		return fail("could not account the QSPI interrupts (EPK wrap failed)");

	nor.rep.irq = (nor.irqs.count > 0u) ? nor.irqs.irqn[0] : -1;
	return 0;
}

/*
 * Steps 6-7, and the whole of what issue #88's writer has to redo after every
 * transaction.  Factored out of bring-up rather than copied into the writer:
 * "the window is up and it is really this flash" is one claim, and two versions
 * of it would be two claims that are free to drift.
 */
static int window_up(void)
{
	struct nor_report *r = &nor.rep;
	uint32_t first, probe;

	/* 6: quad, continuous-read.  Donor-identical -- this is the configuration
	 * the SDK's own classification app uses to read a model from this part. */
	if (hx_lib_spi_eeprom_enable_XIP(USE_DW_SPI_MST_Q, true, FLASH_QUAD,
	                                 true) != 0)
		return fail("QSPI XIP enable failed");

	/* 7: read back what the vendor was supposed to have established, BEFORE
	 * dereferencing the alias -- a probe against a mapping that was never
	 * installed faults instead of failing cleanly. */
	r->scu_xip_after = rd32(SCU_ISP_XIP_SPICACHE);
	if (mpu_capture(r) < 0)
		return fail("no MPU region covers the flash window after XIP enable");
	if ((r->mpu_ctrl_s & MPU_CTRL_ENABLE_Msk) == 0u)
		return fail("the Secure MPU is disabled after XIP enable");

	/* Invalidate every line the probe is about to read, THEN order it, THEN
	 * read.  The vendor's own 512 bytes at the base do not cover probe B, and
	 * a probe served from cache proves nothing about the window. */
	invalidate_alias(NOR_PROBE_A_OFF, 4u);
	invalidate_alias(NOR_PROBE_B_OFF, 8u);   /* both magic words */
	__DSB();
	__ISB();

	first = rd32(NOR_XIP_BASE + NOR_PROBE_A_OFF);
	probe = rd32(NOR_XIP_BASE + NOR_PROBE_B_OFF);

	/* Observation only -- see NOR_SLOT_HDR_MAGIC0.  Captured before the
	 * verdict below so that `nor info` can show it even on a refusal. */
	r->probe_off  = NOR_PROBE_B_OFF;
	r->probe_word = probe;
	r->probe_hdr  = (probe == NOR_SLOT_HDR_MAGIC0 &&
	                 rd32(NOR_XIP_BASE + NOR_PROBE_B_OFF + 4u) ==
	                 NOR_SLOT_HDR_MAGIC1) ? 1u : 0u;

	/* The failure that actually happened: one register block aliased across the
	 * whole window, so every address reads the same. */
	if (first == probe)
		return fail("flash window still aliases; XIP did not take");
	/* And the stronger question: is this the FLASH?  A register block cannot
	 * coincidentally be a Himax image header.
	 *
	 * [!] AND AFTER A WRITE TRANSACTION IT ANSWERS ONE MORE (issue #88).
	 * Taking the window down clears the part's quad-enable bit and bringing it
	 * back sets it again; if that had not taken, this continuous quad read
	 * would return something that is not a firmware header.  The check was
	 * already here, so the writer needs no separate one -- but it is now
	 * carrying that too. */
	if (first != NOR_IMAGE_MAGIC)
		return fail("flash window is readable but does not start with a "
		            "firmware image");
	return 0;
}

/* Steps 5-7. */
static int enable_xip_and_verify(void)
{
	struct nor_report *r = &nor.rep;

	/* 5: before XIP, or never. */
	if (hx_lib_spi_eeprom_read_ID(USE_DW_SPI_MST_Q, r->jedec) == 0)
		r->jedec_valid = 1u;
	else
		LOG_WRN("JEDEC id unreadable; `nor info` will not show one");

	r->scu_xip_before = rd32(SCU_ISP_XIP_SPICACHE);

	if (window_up() != 0)
		return -1;

	LOG_INF("QSPI XIP on at 0x%08lx, IRQ %d wrapped",
	        (unsigned long)NOR_XIP_BASE, nor.rep.irq);
	return 0;
}

/* Claim a slot and mint its token.  Caller holds the critical section. */
static void claim_locked(enum nor_lease_slot slot, uint32_t *out)
{
	nor.live |= (1u << (uint32_t)slot);
	nor.rep.readers = nor_readers(nor.live);
	*out = nor_token_make(nor.gen, slot);
}

int nor_acquire(enum nor_lease_slot slot, uint32_t *out)
{
	uint32_t pm;

	if (out != NULL)
		*out = 0u;
	if ((unsigned)slot >= (unsigned)NOR_LEASE_SLOTS || out == NULL)
		return -1;

	/* [!] The state is READ and the ENABLING claim PUBLISHED under the same
	 * critical section, or two callers both reading OFF would both be told to
	 * bring up a device that is already being brought up -- with the vendor's
	 * open running twice and one snapshot straddling the other's wrap. */
	pm = __get_PRIMASK();
	__disable_irq();
	if (nor.live & (1u << (uint32_t)slot)) {
		__set_PRIMASK(pm);
		return -1;              /* the slot is single-instance */
	}
	switch (nor_acquire_decide(nor.state)) {
	case NOR_ACQ_BRING_UP:
		nor.state = NOR_ST_ENABLING;
		break;
	case NOR_ACQ_TAKE:
		/* [!] CLAIMED HERE, not in a second critical section further down.
		 * Splitting the free-slot test from the claim leaves a window in
		 * which another caller for the SAME slot passes the same test. */
		claim_locked(slot, out);
		__set_PRIMASK(pm);
		return 0;
	case NOR_ACQ_BUSY:
	case NOR_ACQ_FAULTED:
	default:
		__set_PRIMASK(pm);
		return -1;
	}
	__set_PRIMASK(pm);

	/* The hardware work runs unmasked: XIP setup waits on DMA completions. */
	if (open_and_wrap() != 0 || enable_xip_and_verify() != 0)
		return -1;              /* fail() already made it terminal */

	/* [!] AND THE COMMIT IS ONE TRANSACTION.  Publishing NOR_ST_XIP first and
	 * claiming the slot afterwards leaves a window where a second caller for
	 * this same slot sees an up window and a free slot, takes it, and is minted
	 * a token IDENTICAL to the one this call is about to mint -- same
	 * generation, same slot.  Either holder's release would then drop the
	 * other's claim, and a future writer would see zero readers with one still
	 * out.  The generation bump, the state publication and the claim go
	 * together or the slot means nothing. */
	pm = __get_PRIMASK();
	__disable_irq();
	nor.gen++;
	nor.state = NOR_ST_XIP;
	claim_locked(slot, out);
	__set_PRIMASK(pm);
	return 0;
}

int nor_release(uint32_t token)
{
	enum nor_release verdict;
	uint32_t pm = __get_PRIMASK();
	int rc = -1;

	__disable_irq();
	verdict = nor_release_decide(nor.state, nor.live, token, nor.gen);
	if (verdict == NOR_REL_DROP) {
		nor.live &= ~(1u << ((token & 0xFFu) - 1u));
		nor.rep.readers = nor_readers(nor.live);
		rc = 0;
	}
	__set_PRIMASK(pm);

	/* NOT_HELD is the ordinary answer on the `nn close` path and is silent.
	 * The other two mean somebody is handing back a claim that is not theirs. */
	if (verdict == NOR_REL_STALE)
		LOG_WRN("release of a token from an older lifecycle, ignored");
	else if (verdict == NOR_REL_BAD_SLOT)
		LOG_WRN("release of a token naming no slot, ignored");
	return rc;
}

enum nor_state nor_lifecycle_state(void)
{
	return nor.state;
}

const char *nor_fail_reason(void)
{
	return nor.fail;
}

void nor_report(struct nor_report *r)
{
	uint32_t pm;

	if (r == NULL)
		return;

	/* [!] ONE CRITICAL SECTION FOR THE WHOLE SNAPSHOT (issue #91).  The owner
	 * of a reservation moves RESERVED -> WRITING without warning, and that
	 * transition takes the window down: a state sampled before it and
	 * registers sampled after it would describe a mapping that never existed.
	 * Everything below is register reads and a sixteen-entry MPU walk -- no
	 * waits, no vendor calls -- so the section stays short. */
	pm = __get_PRIMASK();
	__disable_irq();
	*r = nor.rep;
	r->state   = nor.state;
	r->live    = nor.live;
	r->readers = nor_readers(nor.live);
	r->irq_enabled = 0u;
	if (nor.rep.irq >= 0) {
		uint32_t n = (uint32_t)nor.rep.irq;
		r->irq_enabled = (NVIC->ISER[n >> 5] & (1u << (n & 31u))) ? 1u : 0u;
	}
	/* Re-read the live registers rather than trusting what bring-up latched:
	 * the point of `nor info` is to show what is true now.
	 *
	 * [!] ENUMERATED, and RESERVED is in the list because the window is UP in
	 * it -- a reservation bars readers, it does not take the mapping away.
	 * WRITING is deliberately absent: the window is down, so the registers
	 * would describe the transition rather than the mapping. regs_sampled is
	 * what stops the caller printing the stale copy as if it were current. */
	r->regs_sampled = 0u;
	switch (nor.state) {
	case NOR_ST_XIP:
	case NOR_ST_RESERVED:
		r->scu_xip_after = rd32(SCU_ISP_XIP_SPICACHE);
		(void)mpu_capture(r);
		r->regs_sampled = 1u;
		break;
	case NOR_ST_OFF:
	case NOR_ST_ENABLING:
	case NOR_ST_WRITING:
	case NOR_ST_FAULTED:
	default:
		break;
	}
	__set_PRIMASK(pm);
}

/* --- what a write transaction borrows (issue #88) --------------------------
 *
 * port/nor/nor_write.c owns the transaction; these five are the pieces of it
 * that touch this file's state or this file's window.  See nor_flash.h for the
 * contract and nor_write.h for the sequence they belong to.
 */

int nor_reserve(uint32_t *out)
{
	enum nor_reserve verdict;
	uint32_t pm;

	if (out == NULL)
		return -1;
	*out = 0u;

	pm = __get_PRIMASK();
	__disable_irq();
	verdict = nor_reserve_decide(nor.state, nor.live, nor.owner);
	if (verdict == NOR_RSV_GO) {
		/* [!] THE SEQUENCE, THE OWNER AND THE STATE GO TOGETHER.  Publishing
		 * NOR_ST_RESERVED first would leave a state whose owner is zero --
		 * which nor_owner_consistent() calls corruption precisely because
		 * nobody could ever unreserve it (nor_state.h).
		 *
		 * The counter is per-reservation and never reused, so a token kept
		 * past its unreserve cannot validate against the next one. */
		nor.rsv_seq++;
		nor.owner = nor_reservation_make(nor.rsv_seq);
		nor.state = NOR_ST_RESERVED;
		*out = nor.owner;
	}
	__set_PRIMASK(pm);

	if (verdict == NOR_RSV_FAULTED && nor.fail == NULL)
		LOG_WRN("reservation refused: the lifecycle bookkeeping disagrees");
	return (verdict == NOR_RSV_GO) ? 0 : -1;
}

int nor_unreserve(uint32_t token)
{
	enum nor_unreserve verdict;
	uint32_t pm = __get_PRIMASK();
	int rc = -1;

	__disable_irq();
	verdict = nor_unreserve_decide(nor.state, nor.owner, token);
	switch (verdict) {
	case NOR_UNRSV_DROP:
		nor.owner = 0u;
		nor.state = NOR_ST_XIP;
		rc = 0;
		break;
	/* The owner lets go; the fault stays.  Getting out of FAULTED is not
	 * something any caller can do -- it takes a reset. */
	case NOR_UNRSV_DROP_FAULTED:
		nor.owner = 0u;
		rc = 0;
		break;
	case NOR_UNRSV_BUSY:
	case NOR_UNRSV_NOT_HELD:
	case NOR_UNRSV_STALE:
	case NOR_UNRSV_INCONSISTENT:
	default:
		break;
	}
	__set_PRIMASK(pm);

	/* NOT_HELD is ordinary: the single release point runs with a token that
	 * is still zero whenever the reserve itself was refused.  The rest mean
	 * somebody is handing back a claim that is not theirs, or that this
	 * port's own bookkeeping has come apart. */
	if (verdict == NOR_UNRSV_STALE)
		LOG_WRN("unreserve with a token from an older reservation, ignored");
	else if (verdict == NOR_UNRSV_BUSY)
		LOG_ERR("unreserve while a write transaction is running, ignored");
	else if (verdict == NOR_UNRSV_INCONSISTENT)
		LOG_ERR("unreserve found the state and the owner disagreeing");
	return rc;
}

uint32_t nor_reservation_owner(void)
{
	return nor.owner;
}

enum nor_write nor_write_claim(uint32_t token)
{
	enum nor_write verdict;
	uint32_t pm = __get_PRIMASK();

	/* [!] The state, the lease mask and the owner are read TOGETHER and the
	 * claim is published before the section ends.  Two steps would leave an
	 * interval nor_acquire() only has to land in once for a reader to be
	 * handed a lease on a window that is about to disappear -- see
	 * nor_write_decide(). */
	__disable_irq();
	verdict = nor_write_decide(nor.state, nor.live, nor.owner, token);
	if (verdict == NOR_WR_GO)
		nor.state = NOR_ST_WRITING;
	__set_PRIMASK(pm);
	return verdict;
}

void nor_write_commit(int ok, const char *why)
{
	uint32_t pm;
	int faulted;

	pm = __get_PRIMASK();
	__disable_irq();
	/* [!] nor.fail IS CONSULTED, not just @p ok.  The window helpers latch
	 * through fail(), which has already set NOR_ST_FAULTED; a transaction that
	 * lost the window early and then succeeded at the steps after it must not
	 * be able to publish NOR_ST_XIP over the top of that. */
	if (ok && nor.fail == NULL) {
		/* [!] BACK TO RESERVED, NOT XIP (issue #91).  The transaction is over;
		 * the reservation is not.  Publishing XIP here is the gap a reader
		 * used to be handed a lease in, between one chunk of a `blob write`
		 * and the next. */
		nor.state = NOR_ST_RESERVED;
		faulted = 0;
	} else {
		if (nor.fail == NULL)
			nor.fail = (why != NULL) ? why : "write transaction failed";
		nor.state = NOR_ST_FAULTED;
		faulted = 1;
	}
	__set_PRIMASK(pm);

	if (faulted)
		LOG_ERR("write transaction left the port faulted: %s", nor.fail);
}

int nor_window_drop(void)
{
	uint8_t xip = 1u, isp = 1u;

	if (hx_lib_spi_eeprom_enable_XIP(USE_DW_SPI_MST_Q, false, FLASH_QUAD,
	                                 true) != 0)
		return fail("QSPI XIP disable failed");

	/* [!] ASKED IS NOT HAPPENED.  hx_lib_qspi_eeprom_enable_XIP() calls
	 * hx_drv_scu_set_xip_en() and hx_drv_scu_set_isp_write_en() and discards
	 * both results, so this is the only place the transition becomes a fact.
	 * The vendor's erase and program refuse with -28 while the window is up --
	 * but that is the vendor's own software flag, set by the same call, and a
	 * flag agreeing with itself is not evidence about the SCU. */
	if (hx_drv_scu_get_xip_en(&xip) != SCU_NO_ERROR || xip != 0u)
		return fail("the flash window did not go down when it was asked to");
	if (hx_drv_scu_get_isp_write_en(&isp) != SCU_NO_ERROR || isp != 0u)
		return fail("ISP write enable is set with the flash window down");

	nor.rep.scu_xip_after = rd32(SCU_ISP_XIP_SPICACHE);
	return 0;
}

int nor_window_restore(void)
{
	return window_up();
}

void nor_alias_invalidate(uint32_t off, uint32_t len)
{
	invalidate_alias(off, len);
}

int nor_jedec_recheck(uint8_t out[3])
{
	uint8_t id[3] = { 0u, 0u, 0u };
	int rc;

	if (out == NULL)
		return -1;
	rc = (hx_lib_spi_eeprom_read_ID(USE_DW_SPI_MST_Q, id) == 0) ? 0 : -1;
	out[0] = id[0];
	out[1] = id[1];
	out[2] = id[2];
	if (rc != 0)
		return -1;

	if (nor.rep.jedec_valid) {
		if (memcmp(id, nor.rep.jedec, sizeof(id)) != 0)
			return -1;
		return 0;
	}
	/* Bring-up could not read one -- it warns and carries on, because the id is
	 * reported and not required.  Latch whatever answers now so that the NEXT
	 * transaction has something to compare against. */
	memcpy(nor.rep.jedec, id, sizeof(id));
	nor.rep.jedec_valid = 1u;
	return 0;
}
