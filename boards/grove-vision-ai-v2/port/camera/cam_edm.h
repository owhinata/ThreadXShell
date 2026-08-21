/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    cam_edm.h
 * @brief   What this port remembers about an EDM interrupt it was not meant to
 *          get (issue #68).
 *
 * WHAT EDM IS AND WHY THERE IS ANYTHING HERE.  The Error Detection Module is a
 * Himax datapath monitor on IRQ 143.  Its interrupt service routine lives in
 * the prebuilt libdriver.a and dispatches on the three WDMA watchdog bits: set
 * -> the watchdog callback, clear -> the timing callback.  The vendor's
 * sensordp library registers the FORMER, and forwards each watchdog bit into
 * this port's datapath callback as a distinct negative status, so watchdog
 * timeouts are already handled.  Nothing in the SDK registers the LATTER, so an
 * interrupt that reaches that branch produces one line of vendor xprintf and is
 * then gone.
 *
 * That line is the last thing in the log ring before both observed `nn preview`
 * hangs, and it appears in none of the healthy bring-ups recorded beside them.
 * This file is how the next one leaves something behind.
 *
 * [!] WHAT ARRIVES HERE IS NOT A TIMING VIOLATION.  The datapath configuration
 * this port already performs masks every timing bit and leaves only the three
 * watchdog bits able to raise the interrupt.  So the branch this observer sits
 * on should be unreachable, and the event that was actually seen carried a
 * status of zero -- an interrupt taken while the status register read nothing.
 * Two readings fit: a stale pending bit delivered after its source was gone, or
 * a watchdog whose status was cleared between the NVIC latching the request and
 * the ISR's read (the per-frame retrigger rewrites the watchdog configuration,
 * so there is a writer in the right place).  The second would mean a
 * five-second datapath stall was reported and silently discarded.
 *
 * [!] WHAT IS RECORDED DOES NOT TELL THOSE TWO APART.  Both present as status
 * zero, the same mask, and watchdog counters the retrigger has just reloaded;
 * nothing here observes the age of a pending bit or the phase of the retrigger.
 * What this settles is the question with no answer at all today -- whether the
 * event happens ONCE or REPEATEDLY, at what rate, and in which stream -- which
 * is what the two readings above would then be argued from.
 *
 * WHY THIS IS ITS OWN FILE, and pure.  Same reason as cam_state.h: the part
 * that matters cannot be produced on hardware.  Nothing a console can type
 * makes EDM fire, so the accumulation and the logging policy would otherwise be
 * code that has never once run.  Here they are a function over a struct, and
 * test/test_cam_edm.c drives them.  camera.c owns the sequencing, the register
 * reads and the log record; cam_dp.c owns the vendor seam.
 *
 * THREADING.  cam_edm_note() runs in INTERRUPT context, cam_edm_snapshot() in
 * thread context.  Neither takes a lock; the caller holds interrupts off around
 * the snapshot so the tuple describes one instant.
 */
#ifndef CAM_EDM_H
#define CAM_EDM_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** One EDM interrupt, with the state around it already read out. */
struct cam_edm_event {
	uint32_t status;      /**< what the vendor ISR read and passed on   */
	uint32_t mask;        /**< EDM interrupt mask (1 == masked)         */
	uint32_t wdt[3];      /**< the three WDMA watchdog counters         */
	uint32_t tick;        /**< profile timer, free-running              */
	uint32_t generation;  /**< which stream this belongs to             */
};

/**
 * What the observer has seen since boot.  Zero-initialised is valid and means
 * "nothing", which is also the expected steady state.
 *
 * [!] CUMULATIVE SINCE BOOT, deliberately not per stream.  The event is rare
 * enough that two of them may be a session apart, and a counter cleared by the
 * next `camera preview` would throw away the only evidence there was.  The log
 * records carry the per-event detail; this carries the tally.
 */
struct cam_edm_state {
	uint32_t events;        /**< SATURATING count -- see cam_edm_note() */
	uint32_t first_status;
	uint32_t last_status;
	uint32_t first_tick;    /**< profile timer at the first event       */
	uint32_t first_gen;     /**< stream generation at the first event   */
};

/**
 * @brief  Fold one event in.  INTERRUPT CONTEXT.
 * @return non-zero if this event should be logged.
 *
 * [!] THE COUNT SATURATES rather than wrapping, and that is load-bearing.  The
 * logging policy below is "the first, then every power of two", which is what
 * keeps a storm from pushing the boot out of a ring that only survives the
 * recovery reset because nothing overwrote it.  A wrapping counter would walk
 * back through 1, 2, 4 ... and start the trail over, turning a bounded policy
 * into an unbounded one exactly when the ring can least afford it.
 * UINT32_MAX is not a power of two, so a saturated counter logs nothing more.
 */
int cam_edm_note(struct cam_edm_state *st, const struct cam_edm_event *ev);

/**
 * @brief  Is this the occurrence the policy logs?  (Exposed for the test.)
 *
 * The first, then every power of two: 1, 2, 4, 8 ... 0x80000000.  A rare event
 * still reports the moment it happens; a storm leaves a geometric trail whose
 * length is bounded by the width of the counter.
 */
int cam_edm_should_log(uint32_t count);

/** @brief  Copy the tally out.  Thread context, interrupts held off. */
void cam_edm_snapshot(const struct cam_edm_state *st,
                      struct cam_edm_state *out);

#ifdef __cplusplus
}
#endif

#endif /* CAM_EDM_H */
