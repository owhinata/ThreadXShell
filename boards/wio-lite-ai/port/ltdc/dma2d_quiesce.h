/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    dma2d_quiesce.h
 * @brief   Has the DMA2D stopped writing?  The admission check, the one
 *          stop-and-confirm procedure, the verdict on a finished transfer and
 *          the two marks that carry a failure to the flip and to the OCTOSPI1
 *          reconfiguration gate (issue #113, #156).
 *
 * The DMA2D writes the back frame buffer in the OCTOSPI1 PSRAM.  A transfer
 * that failed (timeout, TE, CE, a HAL refusal) leaves two questions: may the
 * half-drawn back buffer be shown (no), and has the engine actually stopped
 * writing PSRAM (only the hardware can say).  This file answers both from
 * register reads alone, so it is compiled unchanged on the host
 * (test/test_dma2d_quiesce.c): it includes no HAL, no CMSIS and no ThreadX.
 * Register access and the clock are injected through struct dma2d_q_io.
 *
 * Two outcomes for a failure:
 *
 *   (a) FRAME -- the engine was stopped and CR.START / CR.ABORT were read
 *       back 0 within the deadline.  Only this frame is lost: the frame mark
 *       is raised, the next dma2d_q_present() refuses once and lowers it.
 *       A later SUCCESSFUL draw does not lower it (a failed background under
 *       a successful rectangle is still a half-drawn frame).
 *   (b) DEAD -- START (or ABORT) never read back 0, or START was already 1
 *       before a transfer was even configured (some path lost track of one).
 *       The permanent mark is raised.  It has exactly one assignment (in
 *       dma2d_quiesce.c) and NO path lowers it: there is no clear function,
 *       and the firmware keeps one zero-initialised instance.  The caller
 *       latches the display down; dma2d_q_unquiesced() is what the OCTOSPI1
 *       reconfiguration gate must ask, so a DMA2D that may still be writing is
 *       never hidden behind "scan-out is off".  Recovery is a reset.
 *
 * [!] ASSUMPTION THE REFERENCE MANUAL DOES NOT GUARANTEE.  "CR.START reads 0,
 * therefore every DMA2D write to the PSRAM has completed" is assumed, not
 * documented: RM0468 sec 18.3.15 / 18.5.1 say START is cleared by hardware at
 * the end of a transfer, on ABORT, on a transfer error and on a configuration
 * error, and ABORT is cleared when START is -- but nothing says the AXI write
 * path has drained at that moment, and START (with ABORT) is the only status
 * the DMA2D exposes.  ES0491 lists no DMA2D erratum.  The successful path has
 * always rested on the same assumption (a transfer is "done" when START
 * drops); (a) adds nothing new, it just uses it for the failed path too.
 *
 * The deadline is wall-clock (io->now, in whatever unit io->timeout is in),
 * never an iteration count: a preempted poll must not give up early, and a
 * fast loop must not give up late.
 *
 * Bit positions are RM0468 sec 18.5 (DMA2D_CR / _ISR / _IFCR).  The firmware
 * binding checks them against the CMSIS device header with _Static_assert.
 *
 * Concurrency: all of this runs under the LTDC frame lock, in thread context.
 * The marks are volatile single bytes on a single core; the reconfiguration
 * gate reads the permanent one with PRIMASK set.
 */
#ifndef DMA2D_QUIESCE_H
#define DMA2D_QUIESCE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* RM0468 sec 18.5.1 DMA2D_CR. */
#define DMA2D_Q_CR_START   0x00000001u
#define DMA2D_Q_CR_ABORT   0x00000004u
/* RM0468 sec 18.5.2 DMA2D_ISR / sec 18.5.3 DMA2D_IFCR (same positions). */
#define DMA2D_Q_FLAG_TE    0x00000001u
#define DMA2D_Q_FLAG_TC    0x00000002u
#define DMA2D_Q_FLAG_CE    0x00000020u
#define DMA2D_Q_FLAGS      (DMA2D_Q_FLAG_TE | DMA2D_Q_FLAG_TC | DMA2D_Q_FLAG_CE)

/* Results.  OK is the only success. */
#define DMA2D_Q_OK     0   /* admit: go ahead / conclude: drawn / present: show */
#define DMA2D_Q_FRAME  1   /* stopped and confirmed: this frame only (a)        */
#define DMA2D_Q_DEAD   2   /* not confirmed stopped: permanent (b)              */

/* Register and clock access, supplied by the caller (the firmware binds these
 * to the DMA2D registers and the tick; the host test to fakes). */
struct dma2d_q_io {
	uint32_t (*cr_read)(void *ctx);
	void     (*cr_write)(void *ctx, uint32_t v);
	uint32_t (*isr_read)(void *ctx);
	void     (*ifcr_write)(void *ctx, uint32_t v);
	uint32_t (*now)(void *ctx);   /* free-running, wraps; unit of timeout */
	uint32_t timeout;             /* how long START/ABORT may take to drop */
	void    *ctx;
};

/* The two marks.  Zero-initialised; nothing in this API zeroes them again. */
struct dma2d_q {
	volatile uint8_t frame_failed;   /* (a): this back buffer is not to be shown */
	volatile uint8_t unquiesced;     /* (b): permanent, never lowered            */
};

/**
 * Before a transfer is configured (before HAL_DMA2D_Init / ConfigLayer).
 * Permanent mark already up: touches nothing, DMA2D_Q_DEAD.  CR.START already
 * 1: configures nothing (the setup registers cannot be written during a
 * transfer, RM0468 sec 18.5.1), raises the permanent mark, DMA2D_Q_DEAD.
 * Otherwise clears TC / TE / CE so the verdict cannot read a stale one, and
 * returns DMA2D_Q_OK.
 */
int dma2d_q_admit(struct dma2d_q *s, const struct dma2d_q_io *io);

/**
 * Stop the engine and confirm it stopped -- the one procedure both paths use.
 * The caller has already masked the DMA2D interrupts and dropped its in-flight
 * record.  ABORT is set only if START reads 1 (and START is never written as
 * 1).  Then CR is read back until START and ABORT are both 0 or the deadline
 * passes.  Confirmed: TC / TE / CE are cleared, the frame mark raised,
 * DMA2D_Q_FRAME.  Not confirmed: the permanent mark raised, flags left as the
 * hardware has them, DMA2D_Q_DEAD.  Never returns DMA2D_Q_OK.
 *
 * [!] A RACE THAT ENDS PERMANENT, ACCEPTED.  If the transfer finishes between
 * the read that saw START = 1 and the ABORT write, ABORT lands on an idle
 * engine.  RM0468 sec 18.5.1 has the hardware clear ABORT "when the START bit
 * is reset" -- an event that has then already happened -- so ABORT may read
 * 1 until the deadline and this returns DMA2D_Q_DEAD for an engine that had in
 * fact stopped.  The window is one register read wide and the error is in the
 * safe direction (the display goes down, OCTOSPI1 reconfiguration is refused);
 * recovery is a reset.
 */
int dma2d_q_quiesce(struct dma2d_q *s, const struct dma2d_q_io *io);

/**
 * Verdict on a transfer that was started (or refused).  @p ok_so_far is the
 * path's own answer: the polled path passes "the HAL wait returned HAL_OK",
 * the interrupt path passes dma2d_q_it_ok(), a refused Init / ConfigLayer /
 * Start passes false.  Success needs @p ok_so_far AND no TE / CE in the ISR
 * AND START / ABORT reading 0 -- anything else runs dma2d_q_quiesce().
 * Success leaves the frame mark as it was (it does not lower it).
 * Permanent mark already up: touches nothing, DMA2D_Q_DEAD.
 */
int dma2d_q_conclude(struct dma2d_q *s, const struct dma2d_q_io *io,
                     bool ok_so_far);

/**
 * The interrupt path's own answer: woken in time, the completion callback ran
 * and the error callback did not.  TE and TC in one IRQ is a failure (the HAL
 * handler processes TE, then TC, and may leave State READY).
 */
bool dma2d_q_it_ok(bool woken, bool cplt_seen, bool err_seen);

/**
 * ltdc_flip()'s question, under the frame lock, before it stages anything.
 * Permanent mark up: DMA2D_Q_DEAD (nothing lowered).  Frame mark up: lowers it
 * and returns DMA2D_Q_FRAME (do not present).  Otherwise DMA2D_Q_OK.
 */
int dma2d_q_present(struct dma2d_q *s);

/** The OCTOSPI1 reconfiguration gate's question: true once the DMA2D could not
 *  be confirmed stopped, and forever after. */
bool dma2d_q_unquiesced(const struct dma2d_q *s);

#ifdef __cplusplus
}
#endif

#endif /* DMA2D_QUIESCE_H */
