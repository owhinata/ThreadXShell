/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Host test for the DMA2D stop-and-confirm procedure and its two marks
 * (issue #113, port/ltdc/dma2d_quiesce.c).
 *
 * WHY THIS EXISTS.  Every path here is a failure the board cannot be made to
 * produce at a console: a transfer that times out, a TE / CE the HAL's poll
 * reports as HAL_OK, an engine that will not drop START after ABORT, a START
 * already set before a transfer was configured.  The real file is compiled
 * against a fake DMA2D (CR / ISR / IFCR) and a fake clock.
 *
 * The fake's CR: START drops by itself after `drop_after` CR reads once a
 * transfer is "running" (an IT/poll completion or an error); an ABORT write
 * arms `abort_drop_after` reads until START and ABORT both read 0 (negative:
 * never).  Without ABORT and with drop_after < 0, START stays 1 forever.
 *
 * [!] WHAT IT DOES NOT COVER (review-held): that psram_acquire() actually asks
 * dma2d_q_unquiesced(), and that the permanent mark is raised before
 * lcd_end() gives the PSRAM user slot back -- both are in ltdc_display.c /
 * psram.c / cmd_lcd.c, not here.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "dma2d_quiesce.h"

static int fails;

#define EQ(got, want, what)                                                  \
	do {                                                                 \
		unsigned long g_ = (unsigned long)(got);                     \
		unsigned long w_ = (unsigned long)(want);                    \
		if (g_ != w_) {                                              \
			printf("  FAIL %s:%d %s: got 0x%lx, wanted 0x%lx\n", \
			       __func__, __LINE__, what, g_, w_);            \
			fails++;                                             \
		}                                                            \
	} while (0)

/* ---- the fake DMA2D and clock ------------------------------------------- */

struct fake {
	uint32_t cr, isr;
	int drop_after;        /* reads until START drops by itself; <0 never  */
	int abort_drop_after;  /* reads after ABORT until both drop; <0 never  */
	int abort_armed;
	uint32_t clk, clk_step;
	uint32_t quiet_at_clk;   /* nonzero: START/ABORT read 0 from this time */
	uint32_t jump_at_read, jump_by; /* after the jump_at_read'th CR read, +jump_by */

	unsigned cr_reads, cr_writes, isr_reads, ifcr_writes, nows;
	unsigned abort_writes, start_written;
	uint32_t cr_at_last_clear;      /* CR as it was when IFCR was written */
	uint32_t last_ifcr;
};

static struct fake F;

static uint32_t f_cr_read(void *ctx)
{
	struct fake *f = ctx;

	uint32_t v;

	f->cr_reads++;
	if (f->quiet_at_clk != 0u && f->clk >= f->quiet_at_clk)
		f->cr &= ~(DMA2D_Q_CR_START | DMA2D_Q_CR_ABORT);
	else if (f->abort_armed) {
		if (f->abort_drop_after == 0)
			f->cr &= ~(DMA2D_Q_CR_START | DMA2D_Q_CR_ABORT);
		else if (f->abort_drop_after > 0)
			f->abort_drop_after--;
	} else if ((f->cr & DMA2D_Q_CR_START) != 0u) {
		if (f->drop_after == 0)
			f->cr &= ~DMA2D_Q_CR_START;
		else if (f->drop_after > 0)
			f->drop_after--;
	}
	v = f->cr;
	/* Preempted right after this read: time passes before the next step. */
	if (f->jump_at_read != 0u && f->cr_reads == f->jump_at_read)
		f->clk += f->jump_by;
	return v;
}

static void f_cr_write(void *ctx, uint32_t v)
{
	struct fake *f = ctx;

	f->cr_writes++;
	if ((v & DMA2D_Q_CR_START) != 0u)
		f->start_written++;
	if ((v & DMA2D_Q_CR_ABORT) != 0u) {
		f->abort_writes++;
		f->cr |= DMA2D_Q_CR_ABORT;
		f->abort_armed = 1;
	}
}

static uint32_t f_isr_read(void *ctx)
{
	struct fake *f = ctx;

	f->isr_reads++;
	return f->isr;
}

static void f_ifcr_write(void *ctx, uint32_t v)
{
	struct fake *f = ctx;

	f->ifcr_writes++;
	f->last_ifcr = v;
	f->cr_at_last_clear = f->cr;
	f->isr &= ~v;
}

static uint32_t f_now(void *ctx)
{
	struct fake *f = ctx;
	uint32_t t = f->clk;

	f->nows++;
	/* Runaway guard: a frozen clock under an implementation that never
	 * sees the engine stop would spin forever; past this many looks the
	 * clock jumps, so such an implementation fails instead of hanging. */
	if (f->nows > 100000u)
		f->clk += 0x100000u;
	f->clk += f->clk_step;
	return t;
}

static struct dma2d_q_io io = {
	f_cr_read, f_cr_write, f_isr_read, f_ifcr_write, f_now, 10u, &F,
};

/* A fresh engine: idle, clean flags, a clock ticking 1 per now(). */
static void reset(void)
{
	memset(&F, 0, sizeof F);
	F.drop_after = -1;
	F.abort_drop_after = 3;
	F.clk_step = 1u;
	io.timeout = 10u;
}

static unsigned accesses(void)
{
	return F.cr_reads + F.cr_writes + F.isr_reads + F.ifcr_writes;
}

/* ---- the plan's cases ---------------------------------------------------- */

/* Timeout -> abort -> START drops: this frame only; the flip refuses once and
 * lowers the mark, the next flip presents. */
static void test_timeout_abort_confirmed(void)
{
	struct dma2d_q s = { 0, 0 };

	reset();
	EQ(dma2d_q_admit(&s, &io), DMA2D_Q_OK, "admit");
	F.cr = DMA2D_Q_CR_START;            /* the transfer that never finishes */
	EQ(dma2d_q_conclude(&s, &io, dma2d_q_it_ok(false, false, false)),
	   DMA2D_Q_FRAME, "timed out, stopped");
	EQ(F.abort_writes, 1, "ABORT written once");
	EQ(F.start_written, 0, "START never written as 1");
	EQ(F.cr & (DMA2D_Q_CR_START | DMA2D_Q_CR_ABORT), 0, "engine quiet");
	EQ(F.last_ifcr, DMA2D_Q_FLAGS, "TC/TE/CE cleared");
	EQ(F.cr_at_last_clear & (DMA2D_Q_CR_START | DMA2D_Q_CR_ABORT), 0,
	   "flags cleared only after START/ABORT read back 0");
	EQ(s.frame_failed, 1, "frame mark up");
	EQ(dma2d_q_unquiesced(&s), 0, "not permanent");
	EQ(dma2d_q_present(&s), DMA2D_Q_FRAME, "flip refuses");
	EQ(s.frame_failed, 0, "flip lowered the mark");
	EQ(dma2d_q_present(&s), DMA2D_Q_OK, "next flip presents");
}

/* Timeout -> abort -> START never drops: permanent; nothing lowers it. */
static void test_abort_never_drops(void)
{
	struct dma2d_q s = { 0, 0 };
	unsigned before;
	int i;

	reset();
	F.abort_drop_after = -1;
	F.isr = DMA2D_Q_FLAG_TE;
	EQ(dma2d_q_admit(&s, &io), DMA2D_Q_OK, "admit");
	F.isr = DMA2D_Q_FLAG_TE;            /* the error this transfer raised */
	F.cr = DMA2D_Q_CR_START;
	EQ(dma2d_q_conclude(&s, &io, false), DMA2D_Q_DEAD, "not confirmed");
	EQ(F.abort_writes, 1, "ABORT written once");
	EQ(dma2d_q_unquiesced(&s), 1, "reconfiguration gate refuses");
	EQ(F.isr, DMA2D_Q_FLAG_TE, "flags left as the hardware has them");
	EQ(dma2d_q_present(&s), DMA2D_Q_DEAD, "flip refuses");

	/* The engine later drops START on its own and every later draw
	 * "succeeds": the mark stays, and none of it touches the hardware. */
	F.cr = 0u;
	F.isr = 0u;
	for (i = 0; i < 5; i++) {
		before = accesses();
		EQ(dma2d_q_admit(&s, &io), DMA2D_Q_DEAD, "admit refuses");
		EQ(dma2d_q_conclude(&s, &io, true), DMA2D_Q_DEAD, "conclude");
		EQ(accesses(), before, "no register touched once permanent");
		EQ(dma2d_q_present(&s), DMA2D_Q_DEAD, "flip still refuses");
		EQ(dma2d_q_unquiesced(&s), 1, "still permanent");
	}
}

/* The polled path: HAL_OK, but TE or CE is in the ISR. */
static void test_poll_ok_with_error_flag(void)
{
	static const uint32_t err[] = { DMA2D_Q_FLAG_TE, DMA2D_Q_FLAG_CE };
	unsigned i;

	for (i = 0; i < 2; i++) {
		struct dma2d_q s = { 0, 0 };

		reset();
		EQ(dma2d_q_admit(&s, &io), DMA2D_Q_OK, "admit");
		F.isr = err[i] | DMA2D_Q_FLAG_TC;   /* START already 0 (HW) */
		EQ(dma2d_q_conclude(&s, &io, true), DMA2D_Q_FRAME,
		   "HAL_OK with an error flag is a failure");
		EQ(F.abort_writes, 0, "START already 0: no ABORT");
		EQ(F.isr, 0u, "flags cleared");
		EQ(dma2d_q_present(&s), DMA2D_Q_FRAME, "not presented");
	}
}

/* The interrupt path's error callback: START already dropped by HW. */
static void test_it_error_callback(void)
{
	struct dma2d_q s = { 0, 0 };

	reset();
	EQ(dma2d_q_admit(&s, &io), DMA2D_Q_OK, "admit");
	/* The HAL handler cleared TEIF itself; only the callback knows. */
	EQ(dma2d_q_conclude(&s, &io, dma2d_q_it_ok(true, false, true)),
	   DMA2D_Q_FRAME, "TE callback");
	EQ(F.abort_writes, 0, "no ABORT on an idle engine");
	EQ(F.cr_writes, 0, "CR not written at all");
	EQ(s.frame_failed, 1, "frame mark");
}

/* START already 1 before anything is configured: nothing is set up or
 * started, permanent. */
static void test_start_already_set(void)
{
	struct dma2d_q s = { 0, 0 };

	reset();
	F.cr = DMA2D_Q_CR_START;
	F.isr = DMA2D_Q_FLAG_TC;
	EQ(dma2d_q_admit(&s, &io), DMA2D_Q_DEAD, "refused");
	EQ(F.cr_writes, 0, "CR not written");
	EQ(F.ifcr_writes, 0, "flags not cleared");
	EQ(dma2d_q_unquiesced(&s), 1, "permanent");
	EQ(dma2d_q_present(&s), DMA2D_Q_DEAD, "flip refuses");
}

/* TE and TC in one interrupt, and the HAL refusals. */
static void test_it_both_and_refusals(void)
{
	struct dma2d_q s = { 0, 0 };

	EQ(dma2d_q_it_ok(true, true, true), 0, "TE + TC is a failure");
	EQ(dma2d_q_it_ok(false, true, false), 0, "not woken");
	EQ(dma2d_q_it_ok(true, false, false), 0, "woken, no callback");
	EQ(dma2d_q_it_ok(true, true, false), 1, "the one success");

	/* Init / ConfigLayer refused: never started, START 0, no ABORT. */
	reset();
	EQ(dma2d_q_admit(&s, &io), DMA2D_Q_OK, "admit");
	EQ(dma2d_q_conclude(&s, &io, false), DMA2D_Q_FRAME, "config refused");
	EQ(F.abort_writes, 0, "START 0: no ABORT");
	EQ(dma2d_q_present(&s), DMA2D_Q_FRAME, "not presented");

	/* Start refused, but the engine reads START (some half-start): abort. */
	reset();
	EQ(dma2d_q_admit(&s, &io), DMA2D_Q_OK, "admit");
	F.cr = DMA2D_Q_CR_START;
	EQ(dma2d_q_conclude(&s, &io, false), DMA2D_Q_FRAME, "start refused");
	EQ(F.abort_writes, 1, "ABORT while START 1");
	EQ(F.start_written, 0, "START never written as 1");

	/* "Success" from the path, but START still reads 1: not a success. */
	reset();
	s.frame_failed = 0u;
	F.cr = DMA2D_Q_CR_START;
	EQ(dma2d_q_conclude(&s, &io, true), DMA2D_Q_FRAME,
	   "a success with START still 1 is a failure");
	EQ(s.frame_failed, 1, "frame mark");
}

/* A failure followed by a successful draw: the mark survives until a flip. */
static void test_failure_then_success(void)
{
	struct dma2d_q s = { 0, 0 };

	reset();
	EQ(dma2d_q_admit(&s, &io), DMA2D_Q_OK, "admit background");
	EQ(dma2d_q_conclude(&s, &io, false), DMA2D_Q_FRAME, "background fails");
	EQ(dma2d_q_admit(&s, &io), DMA2D_Q_OK, "admit rectangle");
	EQ(dma2d_q_conclude(&s, &io, true), DMA2D_Q_OK, "rectangle succeeds");
	EQ(s.frame_failed, 1, "a success does not lower the mark");
	EQ(dma2d_q_present(&s), DMA2D_Q_FRAME, "flip refuses");
	/* The next transaction can be presented.  (This is not "the half-drawn
	 * buffer is never shown", only "the failed command did not show it".) */
	EQ(dma2d_q_admit(&s, &io), DMA2D_Q_OK, "admit next");
	EQ(dma2d_q_conclude(&s, &io, true), DMA2D_Q_OK, "next succeeds");
	EQ(dma2d_q_present(&s), DMA2D_Q_OK, "next flip presents");
}

/* Admission clears stale flags so the verdict cannot read an old TE. */
static void test_admit_clears_stale(void)
{
	struct dma2d_q s = { 0, 0 };

	reset();
	F.isr = DMA2D_Q_FLAG_TE | DMA2D_Q_FLAG_CE | DMA2D_Q_FLAG_TC;
	EQ(dma2d_q_admit(&s, &io), DMA2D_Q_OK, "admit");
	EQ(F.isr, 0u, "stale flags cleared");
	EQ(dma2d_q_conclude(&s, &io, true), DMA2D_Q_OK, "clean transfer");
	EQ(s.frame_failed, 0, "no mark");
	EQ(dma2d_q_present(&s), DMA2D_Q_OK, "presented");
}

/* ---- the deadline is wall-clock ----------------------------------------- */

static void test_deadline_wall_clock(void)
{
	struct dma2d_q s = { 0, 0 };

	/* The clock does not move: 1000 reads is not "too long". */
	reset();
	F.clk_step = 0u;
	F.abort_drop_after = 1000;
	F.cr = DMA2D_Q_CR_START;
	EQ(dma2d_q_quiesce(&s, &io), DMA2D_Q_FRAME, "frozen clock still waits");
	EQ(dma2d_q_unquiesced(&s), 0, "not permanent");

	/* Preempted past the deadline right after a busy look, and the engine
	 * stopped meanwhile (at time 500): the expiry must be sampled BEFORE a
	 * read, so the deadline is only acted on after one more look.  Read 1
	 * is the one before ABORT, read 2 the first look of the loop. */
	reset();
	s.frame_failed = 0u;
	F.clk_step = 0u;
	F.abort_drop_after = -1;
	F.quiet_at_clk = 500u;
	F.jump_at_read = 2u;
	F.jump_by = 1000u;
	F.cr = DMA2D_Q_CR_START;
	EQ(dma2d_q_quiesce(&s, &io), DMA2D_Q_FRAME, "looked once after expiry");

	/* Wraps: t0 just below 2^32. */
	reset();
	s.frame_failed = 0u;
	F.clk = 0xFFFFFFFCu;
	F.abort_drop_after = 8;        /* inside the 10-unit deadline */
	F.cr = DMA2D_Q_CR_START;
	EQ(dma2d_q_quiesce(&s, &io), DMA2D_Q_FRAME, "deadline across the wrap");

	/* Never drops: gives up once the clock passes, in bounded reads. */
	reset();
	F.abort_drop_after = -1;
	F.cr = DMA2D_Q_CR_START;
	EQ(dma2d_q_quiesce(&s, &io), DMA2D_Q_DEAD, "gives up");
	if (F.cr_reads > 20u) {
		printf("  FAIL %s: %u reads for a 10-unit deadline\n", __func__,
		       F.cr_reads);
		fails++;
	}
}

int main(void)
{
	test_timeout_abort_confirmed();
	test_abort_never_drops();
	test_poll_ok_with_error_flag();
	test_it_error_callback();
	test_start_already_set();
	test_it_both_and_refusals();
	test_failure_then_success();
	test_admit_clears_stale();
	test_deadline_wall_clock();

	if (fails) {
		printf("test_dma2d_quiesce: %d FAILED\n", fails);
		return 1;
	}
	printf("test_dma2d_quiesce: OK\n");
	return 0;
}
