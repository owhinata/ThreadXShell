/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    nor_state.h
 * @brief   The NOR port's lifecycle states, and the decisions taken over them
 *          (issue #86).
 *
 * WHY THIS IS ITS OWN FILE, like the camera's cam_state.h: the decisions are
 * the parts of the port that CANNOT be produced from a console.  This board has
 * one shell, and its background jobs run below the foreground one under
 * TX_NO_TIME_SLICE, so two callers cannot be steered into the `OFF ->
 * ENABLING` window by typing.  As pure functions over the state they are
 * tables, and test/test_nor_state.c walks them.
 *
 * So this file owns the PRECEDENCE; nor_flash.c owns the sequencing and
 * everything that touches hardware.
 *
 * [!] WHAT FAILS OPEN HERE IS A WIDER TEST.  The states are distinct
 * enumerators, so no reordering of equality tests can confuse them -- but
 * `st != NOR_ST_XIP -> bring it up` would send a second caller into a bring-up
 * that is already running, and `st != NOR_ST_FAULTED -> hand out a lease` would
 * hand one out before the window exists.  Both tables therefore ENUMERATE the
 * states that may act, and anything unknown falls to a refusal.
 */
#ifndef NOR_STATE_H
#define NOR_STATE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief The QSPI/XIP lifecycle.
 *
 * [!] There is no teardown, and that is not an omission.  The vendor library
 * offers no close for this path, and IRQ 133 and the XIP mapping stay live once
 * established -- which is the whole point of issue #86: they used to be torn
 * down by `nn close`, through an EPK wrapset that was never the NPU's to own.
 * So OFF is reached once, at boot, and never again.
 */
enum nor_state {
	NOR_ST_OFF = 0,   /**< nothing brought up; the alias is not readable  */
	NOR_ST_ENABLING,  /**< one caller is bringing it up; others wait out  */
	NOR_ST_XIP,       /**< the window is up and has been probed           */
	NOR_ST_FAULTED,   /**< bring-up failed or could not be verified       */
};

/** What a caller asking for a reader lease should do. */
enum nor_acquire {
	NOR_ACQ_BRING_UP = 0, /**< nothing is up: this caller owns the bring-up */
	NOR_ACQ_TAKE,         /**< the window is up: take a lease and go        */
	NOR_ACQ_BUSY,         /**< somebody else is bringing it up: refuse      */
	NOR_ACQ_FAULTED,      /**< terminal: refuse, and keep refusing          */
};

/**
 * @brief  Decide what an acquire attempt may do, from the state alone.
 *
 * [!] EXACTLY ONE CALLER MAY GET NOR_ACQ_BRING_UP.  The caller that receives it
 * must publish NOR_ST_ENABLING under the same critical section that read the
 * state, or a second caller reading NOR_ST_OFF would be told to bring up a
 * device that is already being brought up -- with the vendor's QSPI open
 * running twice and an EPK snapshot straddling the other's wrap.
 *
 * [!] BUSY IS A REFUSAL, NOT A WAIT.  Bring-up runs the vendor's XIP setup,
 * which moves data with DMA and waits on its completion; a second caller
 * blocking inside the same lifecycle would be waiting on work it cannot see
 * finish.  Refusing matches the camera port's rule and keeps the console
 * answering.
 */
enum nor_acquire nor_acquire_decide(enum nor_state st);

/**
 * @brief Who may hold a reader lease.
 *
 * [!] THE TOKEN CARRIES A HOLDER, NOT JUST A GENERATION, and that is not
 * decoration.  A generation-only token is the SAME VALUE for every lease of the
 * same lifecycle, so two concurrent holders cannot be told apart: one holder
 * releasing twice is indistinguishable from both holders releasing once, and
 * the second decrement takes the other's lease away.  The host test found that
 * before the code existed.
 *
 * Each slot is single-instance, enforced where the slot is taken -- the NPU's
 * by its existing ready flag, the scan's by its own gate, the same shape
 * `nn` and the benchmarks already use.  So a bitmask of live slots is the whole
 * reader accounting, and "how many readers" is its population count.
 */
enum nor_lease_slot {
	NOR_LEASE_NPU = 0,  /**< held across npu_hw_init .. npu_hw_deinit */
	NOR_LEASE_SCAN,     /**< held for the whole of one `nor scan`     */
	/* [!] `devmem` reads the XIP alias too, and used to do it with no lease at
	 * all (issue #90).  That was wrong in two directions, not one: nothing
	 * stopped it reading a window that had never been brought up -- which does
	 * not fault and does not read 0xFF, it aliases one register across all
	 * 16 MB, so the dump printed plausible nonsense as flash contents -- and
	 * nothing stopped a writer dropping XIP underneath an access in flight.
	 * Taking a lease answers both, because acquiring one is what brings the
	 * window up.
	 *
	 * [!] Only the first half was observed on hardware.  The second is
	 * bounded -- a dump of the alias is at most CLI_DEVMEM_DUMP_MAX_LEN bytes
	 * -- and cannot be produced by typing, because background jobs run below
	 * the foreground one under TX_NO_TIME_SLICE.  It is real, and it is the
	 * kind of real this file exists to decide about without hardware. */
	NOR_LEASE_DEVMEM,   /**< held across one `devmem` access to the alias */
	NOR_LEASE_SLOTS,
};

/**
 * @brief  Build the token a holder keeps.  Never 0 for a valid lease.
 *
 * The generation occupies the high bits so that a token from a previous
 * lifecycle compares unequal even when the same slot is taken again.
 */
uint32_t nor_token_make(uint32_t gen, enum nor_lease_slot slot);

/** Why a release is or is not allowed to drop a reader. */
enum nor_release {
	NOR_REL_DROP = 0,  /**< the token is live: drop this holder's lease  */
	NOR_REL_NOT_HELD,  /**< no token, or that slot is not currently held */
	NOR_REL_STALE,     /**< a token from a previous lifecycle generation */
	NOR_REL_BAD_SLOT,  /**< a token naming no slot this port has         */
};

/**
 * @brief  Decide whether a release may drop its holder's lease.
 *
 * @param st    the lifecycle state
 * @param live  bitmask of slots currently holding a lease
 * @param token the caller's token; 0 means "I hold nothing"
 * @param gen   the lifecycle's current generation
 *
 * [!] A HOLDER MAY ALWAYS GIVE ITS LEASE BACK, INCLUDING WHILE FAULTED.  A
 * fault refuses new acquires; refusing releases too would strand every caller
 * that was already holding one, and the mask would never come back empty.  That
 * is why @p st is not consulted.
 *
 * [!] AND A RELEASE WITH NOTHING HELD IS ORDINARY, NOT AN ERROR TO SHOUT ABOUT.
 * `nn close` calls its teardown unconditionally, so NOR_REL_NOT_HELD is the
 * expected answer whenever no bring-up succeeded -- it has to be distinguishable
 * from releasing somebody else's lease, which is what the slot gives it.
 */
enum nor_release nor_release_decide(enum nor_state st, uint32_t live,
                                    uint32_t token, uint32_t gen);

/** How many leases the mask says are out.  Population count, nothing more. */
uint32_t nor_readers(uint32_t live);

/** Human-readable names, for `nor info` and for the host test's diagnostics. */
const char *nor_state_name(enum nor_state st);

#ifdef __cplusplus
}
#endif

#endif /* NOR_STATE_H */
