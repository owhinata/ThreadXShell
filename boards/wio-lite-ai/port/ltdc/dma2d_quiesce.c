/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    dma2d_quiesce.c
 * @brief   DMA2D stop-and-confirm and the two failure marks (issue #113).
 *          See dma2d_quiesce.h; host-tested by test/test_dma2d_quiesce.c.
 */
#include "dma2d_quiesce.h"

/* [!] THE ONLY ASSIGNMENT OF THE PERMANENT MARK.  Every path that cannot
 * confirm the engine stopped comes through here, and nothing anywhere writes
 * the mark back to 0. */
static int mark_unquiesced(struct dma2d_q *s)
{
	s->unquiesced = 1u;
	return DMA2D_Q_DEAD;
}

int dma2d_q_admit(struct dma2d_q *s, const struct dma2d_q_io *io)
{
	if (s->unquiesced != 0u)
		return DMA2D_Q_DEAD;
	if ((io->cr_read(io->ctx) & DMA2D_Q_CR_START) != 0u)
		return mark_unquiesced(s);
	io->ifcr_write(io->ctx, DMA2D_Q_FLAGS);
	return DMA2D_Q_OK;
}

int dma2d_q_quiesce(struct dma2d_q *s, const struct dma2d_q_io *io)
{
	uint32_t cr = io->cr_read(io->ctx);
	uint32_t t0;

	/* ABORT only while a transfer runs; START is written back as 0, which
	 * does not affect it (only the hardware clears it, only software can
	 * set it).  A transfer that ends between this read and the write can
	 * leave ABORT set on an idle engine, which then reads as DEAD below --
	 * accepted, see dma2d_q_quiesce() in the header. */
	if ((cr & DMA2D_Q_CR_START) != 0u)
		io->cr_write(io->ctx, (cr & ~DMA2D_Q_CR_START) | DMA2D_Q_CR_ABORT);

	/* Wall-clock deadline.  The expiry is sampled BEFORE the read, so the
	 * last read always happens after the deadline passed: a thread
	 * preempted past the deadline still gets one look at the register. */
	t0 = io->now(io->ctx);
	for (;;) {
		bool expired = (uint32_t)(io->now(io->ctx) - t0) > io->timeout;

		cr = io->cr_read(io->ctx);
		if ((cr & (DMA2D_Q_CR_START | DMA2D_Q_CR_ABORT)) == 0u)
			break;
		if (expired)
			return mark_unquiesced(s);
	}

	/* Stopped (as far as START can say -- see the header): drop the flags
	 * this transfer left, and keep its frame from being presented. */
	io->ifcr_write(io->ctx, DMA2D_Q_FLAGS);
	s->frame_failed = 1u;
	return DMA2D_Q_FRAME;
}

int dma2d_q_conclude(struct dma2d_q *s, const struct dma2d_q_io *io,
                     bool ok_so_far)
{
	if (s->unquiesced != 0u)
		return DMA2D_Q_DEAD;
	if (ok_so_far &&
	    (io->isr_read(io->ctx) & (DMA2D_Q_FLAG_TE | DMA2D_Q_FLAG_CE)) == 0u &&
	    (io->cr_read(io->ctx) & (DMA2D_Q_CR_START | DMA2D_Q_CR_ABORT)) == 0u)
		return DMA2D_Q_OK;      /* the frame mark is left as it is */
	return dma2d_q_quiesce(s, io);
}

bool dma2d_q_it_ok(bool woken, bool cplt_seen, bool err_seen)
{
	return woken && cplt_seen && !err_seen;
}

int dma2d_q_present(struct dma2d_q *s)
{
	if (s->unquiesced != 0u)
		return DMA2D_Q_DEAD;
	if (s->frame_failed != 0u) {
		s->frame_failed = 0u;
		return DMA2D_Q_FRAME;
	}
	return DMA2D_Q_OK;
}

bool dma2d_q_unquiesced(const struct dma2d_q *s)
{
	return s->unquiesced != 0u;
}
