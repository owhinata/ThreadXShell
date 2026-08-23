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

/* Steps 5-7. */
static int enable_xip_and_verify(void)
{
	struct nor_report *r = &nor.rep;
	uint32_t first, probe;

	/* 5: before XIP, or never. */
	if (hx_lib_spi_eeprom_read_ID(USE_DW_SPI_MST_Q, r->jedec) == 0)
		r->jedec_valid = 1u;
	else
		LOG_WRN("JEDEC id unreadable; `nor info` will not show one");

	r->scu_xip_before = rd32(SCU_ISP_XIP_SPICACHE);

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
	 * coincidentally be a Himax image header. */
	if (first != NOR_IMAGE_MAGIC)
		return fail("flash window is readable but does not start with a "
		            "firmware image");

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
	int      mine = 0;

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
		mine = 1;
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
	if (r == NULL)
		return;
	*r = nor.rep;
	r->readers = nor_readers(nor.live);
	r->irq_enabled = 0u;
	if (nor.rep.irq >= 0) {
		uint32_t n = (uint32_t)nor.rep.irq;
		r->irq_enabled = (NVIC->ISER[n >> 5] & (1u << (n & 31u))) ? 1u : 0u;
	}
	/* Re-read the live registers rather than trusting what bring-up latched:
	 * the point of `nor info` is to show what is true now. */
	if (nor.state == NOR_ST_XIP) {
		r->scu_xip_after = rd32(SCU_ISP_XIP_SPICACHE);
		(void)mpu_capture(r);
	}
}
