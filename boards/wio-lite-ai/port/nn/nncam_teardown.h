/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    nncam_teardown.h
 * @brief   Is the last nn session's teardown finished?  Three questions, one
 *          state (issue #142 review).
 *
 * nn's band step is drained by `nn stream stop`: disarm it, then wait for the
 * producer to leave (port/camera/cam_band.h).  A drain can time out -- a
 * producer stuck inside nn's callback -- and then the input tensor may still be
 * written, so nothing the step touches may be given back.  Three places have to
 * agree on that:
 *
 *   - a FRESH start (not a re-arm) must refuse, retryably, BEFORE it touches
 *     anything (the frame word, the record boundary, the session, the camera):
 *     a start refused only at the step's arm, at the end, had already moved all
 *     of those -- a refusal with side effects;
 *   - the worker, leaving its loop, must not give the guards back while a stop
 *     has not seen the step drained (it used to, whatever the drain said, so the
 *     session went free while a callback could still write the arena);
 *   - a second `nn stream stop` must still find work to do while the step is
 *     not drained, even once nothing else is held.
 *
 * DRAIN PENDING is the stop's own mark: raised before the stop lowers
 * nncam_run (so a worker that sees the run end sees the mark too) and lowered
 * only by a stop whose drain succeeded.  STEP IDLE is the step disarmed with
 * nobody inside (cam_step_may_arm()).  Only a start arms the step, and a start
 * is serialised by the NN session, so a fresh start's "idle" cannot be undone
 * before it arms.
 *
 * Pure, for test/test_nncam_teardown.c.
 */
#ifndef NNCAM_TEARDOWN_H
#define NNCAM_TEARDOWN_H

struct nncam_td {
	int run;            /**< nncam_run                                     */
	int holds_guards;   /**< the NN session + OCTOSPI1 guard are held       */
	int step_idle;      /**< nn's band step disarmed, nobody inside        */
	int drain_pending;  /**< a stop has not yet seen that step drained     */
};

/** May a fresh start begin?  0: refuse with NNCAM_ERR_TEARING, touching
 *  nothing. */
static inline int nncam_td_fresh_start_ok(const struct nncam_td *s)
{
	return s->step_idle && !s->drain_pending;
}

/** Has a stop nothing to do (NNCAM_ERR_NOTRUN)? */
static inline int nncam_td_stop_notrun(const struct nncam_td *s)
{
	return !s->run && !s->holds_guards && s->step_idle && !s->drain_pending;
}

/** May the worker, leaving its run loop, give the guards back?  Not while a
 *  stop has not seen the step drained: that stop's retry gives them. */
static inline int nncam_td_worker_gives(const struct nncam_td *s)
{
	return !s->drain_pending;
}

#endif /* NNCAM_TEARDOWN_H */
