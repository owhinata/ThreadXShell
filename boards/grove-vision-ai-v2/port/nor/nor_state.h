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
	/* [!] ONE WRITER OWNS THE PART, BUT NO TRANSACTION IS RUNNING (issue
	 * #91).  The window is still up and still correct; what has changed is
	 * that no reader may take a lease, because the owner is about to drop it
	 * again.  This state exists because a WRITE THAT IS BIGGER THAN ONE
	 * TRANSACTION had nothing holding the part between its transactions:
	 * nor_write_commit() published NOR_ST_XIP, and a reader only had to land
	 * in that gap to be handed a lease -- which the next transaction would
	 * then refuse, killing a transfer that was minutes in.  It is reachable:
	 * `blob write` blocks the foreground thread on the UART between chunks,
	 * which is exactly when a lower-priority background job runs.
	 *
	 * Only the holder of the reservation token may move RESERVED -> WRITING,
	 * and only nor_unreserve() gets back out to XIP. */
	NOR_ST_RESERVED,  /**< one writer owns it; window up, readers barred  */
	/* [!] The one non-terminal state the window is NOT readable in (issue
	 * #88).  A write has to drop XIP -- the vendor's erase and program entry
	 * points refuse outright while it is on -- so the alias dies for the
	 * duration and every reader has to be kept out.  It is not a teardown:
	 * the transaction restores XIP, re-probes and commits back to
	 * NOR_ST_RESERVED, or fails and commits to NOR_ST_FAULTED.  "OFF is
	 * reached once, at boot" above stays true; what changes is that OFF is no
	 * longer the only state XIP can be left for. */
	NOR_ST_WRITING,   /**< a transaction is running; the alias is down    */
	NOR_ST_FAULTED,   /**< bring-up failed or could not be verified       */
};

/**
 * @brief  Does the owner field agree with the state? (issue #91)
 *
 * [!] THIS IS THE PART THAT IS NOT DECORATION.  Adding an owner to a state
 * machine adds combinations that no correct transition produces, and the two
 * that matter fail in opposite directions:
 *
 *   RESERVED with no owner   nobody can ever unreserve it, so every answer
 *                            this port gives is BUSY for the rest of the
 *                            session.  Refusing it as "wrong token" -- which
 *                            is what a token comparison alone would do -- is
 *                            precisely the wrong answer.
 *   XIP with an owner        a reservation that was released without the
 *                            state following it; the next writer would be
 *                            told the part is free while somebody holds it.
 *
 * Neither is reachable through the transitions below, because the state and
 * the owner are always published in one critical section.  They are checked
 * because "unreachable" is a property of today's call sites, and the cost of
 * being wrong about that is a port nobody can use.  A disagreement is
 * TERMINAL, not busy: it is evidence the port's own bookkeeping is corrupt.
 *
 * @return non-zero when @p owner is consistent with @p st.
 */
int nor_owner_consistent(enum nor_state st, uint32_t owner);

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

/* --- the writer reservation (issue #91) ------------------------------------
 *
 * A reservation is what makes a write that spans SEVERAL transactions safe.
 * It is deliberately NOT a fourth lease slot: a lease lives in @ref live, and
 * anything in @ref live makes nor_write_decide() answer BUSY -- so a writer
 * holding one would refuse itself.  Turning the mask into "these bits are
 * readers and that bit is a writer" would put mask-awareness into the acquire
 * table, which is decided by state alone today and is simpler for it.
 *
 * The state says "somebody owns this"; the token says who.  Both are needed:
 * a state alone cannot tell the owner's second transaction apart from a
 * stranger's first.
 */

/** What a reserve attempt may do. */
enum nor_reserve {
	NOR_RSV_GO = 0,   /**< window up, no readers, no owner: take it        */
	NOR_RSV_BUSY,     /**< readers out, a bring-up in flight, or an owner  */
	/** Terminal.  The port is faulted, the state is unknown, or the state
	 *  and the owner field disagree (nor_owner_consistent()). */
	NOR_RSV_FAULTED,
};

/**
 * @brief  Decide whether a reservation may be taken.
 *
 * [!] AS WITH nor_acquire_decide(), THE CALLER THAT GETS NOR_RSV_GO MUST
 * PUBLISH BOTH THE STATE AND THE OWNER BEFORE LEAVING THE CRITICAL SECTION
 * THAT PRODUCED THEM.  Publishing the state first would leave RESERVED with no
 * owner -- see nor_owner_consistent() for why that is the worst of the
 * reachable mistakes.
 *
 * [!] AND NOR_ST_OFF IS BUSY HERE TOO, for the reason it is busy for a write:
 * bring-up is a reader's errand (it runs the vendor's XIP setup and takes the
 * EPK snapshot), and a writer that owned it would be running two unrelated
 * transactions in one path.
 */
enum nor_reserve nor_reserve_decide(enum nor_state st, uint32_t live,
                                    uint32_t owner);

/** What an unreserve attempt may do. */
enum nor_unreserve {
	NOR_UNRSV_DROP = 0,     /**< clear the owner and go back to XIP        */
	/** Clear the owner but STAY faulted.  A holder may always give a
	 *  reservation back -- refusing while faulted would strand the owner
	 *  field for the rest of the session -- but giving it back is not
	 *  evidence the port recovered. */
	NOR_UNRSV_DROP_FAULTED,
	/** [!] REFUSED, and this is the one refusal that is about the CALLER'S
	 *  code rather than about contention.  A transaction is in flight; the
	 *  only way out of NOR_ST_WRITING is nor_write_commit().  Letting an
	 *  unreserve through here would either strip the owner from a running
	 *  transaction or publish NOR_ST_XIP over a window that is still down.
	 *
	 *  Unreachable through nor_write.c, whose run() cannot return without
	 *  committing -- which is exactly why the host test hands this case to
	 *  the pure function directly.  A rule nobody has watched fail is worth
	 *  as little as no rule. */
	NOR_UNRSV_BUSY,
	NOR_UNRSV_NOT_HELD,     /**< a zero token, or nobody owns it           */
	NOR_UNRSV_STALE,        /**< a token that is not the current owner     */
	/** Terminal: the state and the owner field disagree. */
	NOR_UNRSV_INCONSISTENT,
};

/**
 * @brief  Decide whether a reservation may be given back.
 *
 * @param st     the lifecycle state
 * @param owner  the reservation token currently recorded, or 0
 * @param token  the caller's token; 0 means "I hold nothing"
 */
enum nor_unreserve nor_unreserve_decide(enum nor_state st, uint32_t owner,
                                        uint32_t token);

/**
 * @brief  Build a reservation token.  Never 0 for @p seq != 0.
 *
 * The low byte is 0xFF, which no lease token can carry (a lease's low byte is
 * its slot + 1, and there are three slots).  So a lease token handed to an
 * unreserve, or a reservation token handed to a release, cannot be mistaken
 * for a live claim of the other kind -- it simply fails to match.
 *
 * @p seq is a counter bumped on every reservation, NOT the lifecycle
 * generation: the generation only moves at bring-up, so two reservations in
 * one lifecycle would mint the same token and a stale one from the first would
 * validate during the second.
 */
uint32_t nor_reservation_make(uint32_t seq);

/** What a caller asking to run one write transaction should do. */
enum nor_write {
	NOR_WR_GO = 0,    /**< the caller owns the reservation: claim it       */
	NOR_WR_BUSY,      /**< readers out, a bring-up in flight, or not ours  */
	NOR_WR_FAULTED,   /**< terminal: refuse, and keep refusing             */
};

/**
 * @brief  Decide whether one write transaction may start (issues #88, #91).
 *
 * @param st     the lifecycle state
 * @param live   bitmask of slots currently holding a reader lease
 * @param owner  the reservation token currently recorded, or 0
 * @param token  the caller's reservation token
 *
 * [!] THE ANSWER AND THE CLAIM MUST SHARE ONE CRITICAL SECTION.  This function
 * reads @p st, @p live and @p owner together; the caller that gets NOR_WR_GO
 * must publish NOR_ST_WRITING before releasing the section that produced them.
 * Checking "no readers" and dropping XIP as two steps leaves the interval in
 * between, and nor_acquire() only has to land there once for a reader to be
 * handed a lease on a window that is about to disappear.
 *
 * [!] A TRANSACTION NOW STARTS FROM NOR_ST_RESERVED, NOT FROM NOR_ST_XIP
 * (issue #91).  Every writer holds a reservation first, so XIP means "nobody
 * has claimed this" and is BUSY here.  @p live is still checked even though a
 * reservation can only be taken with no readers out: the cost is one
 * comparison and the alternative is a reader who has to re-derive the
 * reservation's guarantee to believe this one.
 *
 * [!] AND NOR_ST_OFF IS BUSY, NOT "BRING IT UP".  A writer needs the QSPI
 * master open, which is what bring-up does -- but bring-up is a reader's
 * errand: it runs the vendor's XIP setup, waits on DMA completion, and takes
 * the EPK snapshot.  Letting a writer own that would put two unrelated
 * transactions in one path.  Refusing is answerable by the caller: bring the
 * window up first, then reserve, then write.
 *
 * As with nor_acquire_decide(), the states that may act are ENUMERATED.
 * "anything that is not WRITING" would let a writer start from OFF, and
 * "state == RESERVED" alone would let a stranger use somebody else's claim.
 */
enum nor_write nor_write_decide(enum nor_state st, uint32_t live,
                                uint32_t owner, uint32_t token);

/** Human-readable names, for `nor info` and for the host test's diagnostics. */
const char *nor_state_name(enum nor_state st);

#ifdef __cplusplus
}
#endif

#endif /* NOR_STATE_H */
