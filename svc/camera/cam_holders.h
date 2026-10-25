/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    cam_holders.h
 * @brief   Who is holding the camera stream open -- one table for every board
 *          (issue #142, epic #141).
 *
 * A camera streams while, and only while, somebody holds it: the operator
 * (`camera stream start`) or nn (`nn stream start`, `nn run`).  This file is
 * the record of who that is, and the answers that follow from it: "the first
 * holder arrived, start the camera", "the last one left, stop it", "the
 * operator may not leave while nn still needs the stream".  The board acts on
 * the answers; this file only decides them.
 *
 * WHO A HOLDER IS.  A pointer to a static struct cam_holder the caller owns.
 * Identity is the pointer, so two holders that happen to share a name are two
 * holders; the name is only what `camera stream stats` prints.
 *
 * KEEPS-STREAM.  A holder registered with CAM_HOLDER_KEEPS_STREAM is one whose
 * leaving is refused while a holder WITHOUT that attribute is present (issue
 * #142 D8: `camera stream stop` with nn running says "still in use: nn" and
 * changes nothing -- the operator does not leave either).  The table does not
 * know which holder is "the operator"; the board says so when it registers
 * one.  Two holders that both keep the stream do not refuse each other.
 *
 * START IN ONE CALL.  nn joins a running stream, or -- when nothing streams --
 * registers the operator's holder on the operator's behalf and then itself
 * (cam_holders_add_or_start()).  The call says exactly what it added, because
 * a failure afterwards undoes only that (closing is not the inverse of
 * opening: a holder that was already there must survive nn's rollback).
 *
 * [!] EVERY REGISTRATION HAS A GENERATION, AND AN UNDO NAMES IT.  The claim
 * covers only the start; a caller that fails AFTER its join committed (nn's
 * step refusing to open) undoes later, outside any claim -- and by then the
 * table may have been emptied by a confirmed end and the operator may have
 * registered again, or have started the stream by hand on top of nn's join.
 * An undo that removed "the operator" by pointer would take that NEW
 * registration with it and stop the operator's stream.  So each registration
 * (and each re-registration of a holder already present) gets a fresh
 * generation from a counter that only cam_holders_init() resets, the join
 * hands its generations back in a ticket, and cam_holders_undo() removes an
 * entry only while it still carries the generation the ticket names.
 *
 * THE CLAIM.  A registration that takes the table from empty to non-empty
 * returns CAM_HOLDERS_T_START and leaves the table CLAIMED: the board is now
 * starting the camera, and until it calls cam_holders_commit() (it started) or
 * cam_holders_abort() (it did not, which removes what that registration added
 * and nothing else) every other add and remove is refused with
 * CAM_HOLDERS_E_CLAIMED, which is retryable.  That is what keeps a rollback
 * from removing a holder that arrived while the start was in flight.
 *
 * [!] AN EMPTY TABLE IS NOT A PERMISSION TO TEAR DOWN.  CAM_HOLDERS_T_STOP says
 * "nobody holds the stream any more, request a stop".  Resources go back only
 * after the board's own confirmed stop (a producer that stopped, a drained
 * callback); a stop that cannot be confirmed keeps them, whatever this table
 * says.
 *
 * [!] THE LOCK IS THE CALLER'S.  Every function here is a pure function over a
 * table the caller owns, and each call must run under the board's camera lock,
 * in the same locked region as the start or stop request its answer leads to.
 * A table read under one lock and acted on under another decides nothing.
 *
 * [!] SHARED, SO IT OWNS NO MUTABLE STORAGE AND KNOWS NO BOARD.  It includes no
 * RTOS or board header; the shared-storage audit compiles it in each board's
 * context.
 */
#ifndef CAM_HOLDERS_H
#define CAM_HOLDERS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** How many holders a table records.  The plan names two (the operator and
 *  nn); the rest is headroom, and a registration past it is refused. */
#define CAM_HOLDERS_MAX 4

/** A holder.  The caller keeps it static; its address is its identity. */
struct cam_holder {
	const char *name;   /**< printed by the stats line; must not be NULL */
};

/** Attributes given at registration. */
#define CAM_HOLDER_KEEPS_STREAM 0x01u   /**< leaving is refused while a holder
                                         *   without this attribute is present */
#define CAM_HOLDER_ATTRS_KNOWN  0x01u

/** The table.  Zero-initialised is empty and unclaimed; the caller owns it,
 *  places it and locks it. */
struct cam_holders {
	const struct cam_holder *who[CAM_HOLDERS_MAX];  /**< registration order */
	uint32_t gen[CAM_HOLDERS_MAX];  /**< each entry's registration          */
	unsigned char attr[CAM_HOLDERS_MAX];
	unsigned char n;           /**< entries in use                          */
	unsigned char claimed;     /**< a start is in flight                    */
	unsigned char claim_base;  /**< n before the registration that claimed  */
	uint32_t next_gen;         /**< last generation handed out; survives
	                            *   cam_holders_clear()                     */
};

/** What cam_holders_add_or_start() registered, for cam_holders_undo().  A
 *  generation of 0 is never handed out. */
struct cam_holders_ticket {
	unsigned added;            /**< CAM_HOLDERS_ADDED_*                     */
	uint32_t self_gen;         /**< the holder's registration, if added     */
	uint32_t starter_gen;      /**< the starter's registration, if added    */
};

/* ---- answers ------------------------------------------------------------
 *
 * A non-negative return is a success and says which transition it made; a
 * negative one changed NOTHING.  The refusals stay separate codes: each one
 * means something different to the operator, and cam_holders_disp_of() is the
 * one place that says which of them a retry can cure.
 */
#define CAM_HOLDERS_T_NONE    0   /**< the table changed (or not) and the camera
                                   *   need not                                 */
#define CAM_HOLDERS_T_START   1   /**< empty -> non-empty: start the camera, then
                                   *   commit or abort (the table is claimed)   */
#define CAM_HOLDERS_T_STOP    2   /**< non-empty -> empty: request a stop       */

#define CAM_HOLDERS_E_INVAL     (-1)  /**< bad argument, or a malformed table  */
#define CAM_HOLDERS_E_FULL      (-2)  /**< no slot for a new holder            */
#define CAM_HOLDERS_E_CLAIMED   (-3)  /**< a start is in flight                */
#define CAM_HOLDERS_E_IN_USE    (-4)  /**< a keeps-stream holder may not leave
                                       *   while another holder needs it      */
#define CAM_HOLDERS_E_NOT_CLAIMED (-5) /**< commit / abort with no start in
                                        *   flight                            */

/** What a refusal leaves the caller able to do.  Kept apart from the status:
 *  "busy for a moment" and "will not work" are different answers. */
#define CAM_HOLDERS_DISP_DONE      0   /**< it succeeded                      */
#define CAM_HOLDERS_DISP_RETRYABLE 1   /**< the same call can succeed once the
                                        *   in-flight start settles on its own */
#define CAM_HOLDERS_DISP_TERMINAL  2   /**< the same call fails again unless
                                        *   something else changes first      */

/** Bits of cam_holders_add_or_start()'s @p added. */
#define CAM_HOLDERS_ADDED_SELF     0x01u
#define CAM_HOLDERS_ADDED_STARTER  0x02u

/** Make @p t empty and unclaimed (a zero-initialised table already is).  Also
 *  restarts the generations: for boot only -- an end uses cam_holders_clear(). */
void cam_holders_init(struct cam_holders *t);

/** Empty @p t and release any claim, KEEPING the generation counter, so a
 *  ticket from before can never match a registration made after (a confirmed
 *  end of the stream). */
void cam_holders_clear(struct cam_holders *t);

/**
 * Register @p h with @p attr.
 *
 * Already registered with the same attributes: CAM_HOLDERS_T_NONE, and the
 * entry takes a fresh generation -- this caller now holds it too, so an older
 * ticket's undo no longer removes it.  Already registered with DIFFERENT attributes: CAM_HOLDERS_E_INVAL
 * (an operator holder that silently stopped keeping the stream would turn
 * `camera stream stop` into a teardown under nn).  First holder:
 * CAM_HOLDERS_T_START, and the table is claimed.
 */
int cam_holders_add(struct cam_holders *t, const struct cam_holder *h,
                    unsigned attr);

/**
 * Register @p h; when the table is empty, register @p starter first (with
 * @p starter_attr) so the stream has the holder it would have had if it had
 * been started by hand.  One call, so nothing can come between the two.
 *
 * @param tk  out: what THIS call registered and under which generations --
 *            what the caller undoes if it fails afterwards (cam_holders_abort()
 *            while the call's claim is still held, cam_holders_undo() after
 *            it committed).  Zeroed on any refusal.
 *
 * On a non-empty table @p starter is not touched, whether or not it is there.
 * @p h already registered: T_NONE, nothing added (its entry takes a fresh
 * generation, as for cam_holders_add()).
 */
int cam_holders_add_or_start(struct cam_holders *t, const struct cam_holder *h,
                             unsigned attr, const struct cam_holder *starter,
                             unsigned starter_attr,
                             struct cam_holders_ticket *tk);

/**
 * Undo what @p tk registered: @p h if the ticket added it, then @p starter if
 * it added that -- each only while its entry still carries the ticket's
 * generation.  An entry that went away (a clear) or was registered again since
 * (by anyone, including a re-registration of the same holder) is left alone.
 * The keeps-stream refusal applies as for cam_holders_remove().
 *
 * @return the transition of the last removal made (T_STOP if the table became
 *         empty), T_NONE if nothing matched, or CAM_HOLDERS_E_*
 */
int cam_holders_undo(struct cam_holders *t, const struct cam_holder *h,
                     const struct cam_holder *starter,
                     const struct cam_holders_ticket *tk);

/**
 * Remove @p h.  Not registered: CAM_HOLDERS_T_NONE, nothing changes.  The last
 * holder: CAM_HOLDERS_T_STOP.  A keeps-stream holder while a holder without the
 * attribute is present: CAM_HOLDERS_E_IN_USE, and it stays.
 */
int cam_holders_remove(struct cam_holders *t, const struct cam_holder *h);

/** The start succeeded: release the claim. */
int cam_holders_commit(struct cam_holders *t);

/**
 * The start failed: remove what the claiming registration added -- and only
 * that -- and release the claim.  The table is empty again afterwards, because
 * a claim only begins on an empty table, but the rule is "what was added", not
 * "everything".
 *
 * @return the number of holders removed, or CAM_HOLDERS_E_*
 */
int cam_holders_abort(struct cam_holders *t);

/** Holders registered, claimed or not (0 for a malformed table). */
unsigned cam_holders_count(const struct cam_holders *t);

/** Whether a start is in flight. */
int cam_holders_claimed(const struct cam_holders *t);

/** Whether @p h is registered. */
int cam_holders_has(const struct cam_holders *t, const struct cam_holder *h);

/**
 * Copy up to @p max holders, in registration order, into @p out (and their
 * attributes into @p attr, which may be NULL).  Taken under the lock, printed
 * after it: the names are the holders' static strings, so the copy stays valid.
 *
 * @return the number copied
 */
unsigned cam_holders_list(const struct cam_holders *t,
                          const struct cam_holder **out, unsigned char *attr,
                          unsigned max);

/** Map a return of this file onto CAM_HOLDERS_DISP_*.  An unknown negative is
 *  terminal: a code nobody documented carries no evidence that a retry helps. */
int cam_holders_disp_of(int rc);

#ifdef __cplusplus
}
#endif

#endif /* CAM_HOLDERS_H */
