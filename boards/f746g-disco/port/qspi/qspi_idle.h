/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    qspi_idle.h
 * @brief   QUADSPI: the two decisions the driver takes after a receive and
 * after a failed HAL call, as pure functions of register values (issue #194).
 *
 * Board-observed (issue #194): after HAL_QSPI_Receive() returns HAL_OK the
 * controller is idle (SR.BUSY = 0) but CCR.FMODE is still 01 (indirect read;
 * the HAL aborts -- and so resets FMODE -- only when BUSY is still set).  A
 * few seconds later, with no driver call, the controller had started a read
 * on its own (BUSY, FIFO full, AR = 0) and every later command timed out.
 * The likely trigger (inferred, not established) is a speculative CPU read of
 * the 0x90000000 window reaching a controller left in indirect-read mode.  So
 * the driver does not leave it there: after every receive it puts FMODE back
 * to 00 (indirect write), reads back, and does not report success unless the
 * controller reads idle.  The same rules decide whether a failed HAL call has
 * been recovered.
 *
 * [!] The CCR may be rewritten only when the command form it holds has a data
 * phase (DMODE != 00).  RM0385 14.3.5 / 14.3.11: a command with no address and
 * no data starts as soon as CCR is written, so rewriting an instruction-only
 * form (write enable, clear flag status, chip erase) sends that instruction
 * again.  With DMODE != 00 and FMODE = 00 nothing starts until DR is written
 * (14.3.5), and the driver never writes AR here, so the restore starts
 * nothing.  A form without a data phase is already FMODE = 00 when the HAL
 * built it (HAL_QSPI_Command() always configures indirect write); one that
 * reads otherwise is not repaired -- the caller treats it as a failure.  An
 * address-only form (sector / subsector erase) would not start on a CCR write
 * (it waits for AR), but the rule does not lean on that: no data phase, no
 * write.
 *
 * CCR is writable only while SR.BUSY = 0 (RM0385 14.5.6).  An abort is
 * complete when CR.ABORT and SR.BUSY have both cleared (14.3.14).
 *
 * The bit positions are restated here (no HAL include, so the host test can
 * compile this); qspi_flash.c holds them equal to the CMSIS device header's
 * with _Static_assert.  test/test_qspi_idle.c walks both functions.
 */
#ifndef QSPI_IDLE_H
#define QSPI_IDLE_H

#include <stdint.h>

#define QSPI_IDLE_CR_ABORT    (1u << 1)    /* QUADSPI_CR.ABORT              */
#define QSPI_IDLE_SR_BUSY     (1u << 5)    /* QUADSPI_SR.BUSY               */
#define QSPI_IDLE_CCR_ADMODE  (3u << 10)   /* QUADSPI_CCR.ADMODE[1:0]       */
#define QSPI_IDLE_CCR_DMODE   (3u << 24)   /* QUADSPI_CCR.DMODE[1:0]        */
#define QSPI_IDLE_CCR_FMODE   (3u << 26)   /* QUADSPI_CCR.FMODE[1:0]        */

/** What to do with the CCR before the controller is judged idle. */
enum qspi_ccr_step {
	QSPI_CCR_KEEP    = 0,   /* FMODE already 00: do not write                */
	QSPI_CCR_RESTORE = 1,   /* data form, FMODE != 00: write ccr & ~FMODE    */
	QSPI_CCR_REFUSE  = 2,   /* BUSY, or a form without data not in FMODE 00:
	                         * do not write; the caller fails               */
};

static inline enum qspi_ccr_step qspi_ccr_step(uint32_t sr, uint32_t ccr)
{
	if (sr & QSPI_IDLE_SR_BUSY)
		return QSPI_CCR_REFUSE;
	if ((ccr & QSPI_IDLE_CCR_FMODE) == 0u)
		return QSPI_CCR_KEEP;
	if ((ccr & QSPI_IDLE_CCR_DMODE) == 0u)
		return QSPI_CCR_REFUSE;
	return QSPI_CCR_RESTORE;
}

/**
 * Nonzero when the controller reads idle: no abort pending, not busy, and
 * not left in a mode that a window access or a register write could start.
 * The caller reads CR / SR / CCR AFTER any write qspi_ccr_step() asked for.
 * After a receive this decides whether the receive reports success; after a
 * failed HAL call, whether the handle may go back to READY.
 */
static inline int qspi_idle_ok(uint32_t cr, uint32_t sr, uint32_t ccr)
{
	return (cr & QSPI_IDLE_CR_ABORT) == 0u &&
	       (sr & QSPI_IDLE_SR_BUSY) == 0u &&
	       (ccr & QSPI_IDLE_CCR_FMODE) == 0u;
}

#endif /* QSPI_IDLE_H */
