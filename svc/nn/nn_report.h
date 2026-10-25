/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    nn_report.h
 * @brief   Capturing an external decoder's account of its own result
 *          (issue #110 = #78 Step 3b).
 *
 * The buffer and the contract are in svc/nn_svc.h, where the shared command
 * that owns the storage can see them.  This is the bounded sink a BOARD hands
 * to its decoder, and the mapping from what the decoder returned to what the
 * operator is told.
 *
 * [!] IT MAY RUN UNDER A GATE, and must: the only moment at which an external
 * result is still guaranteed to be the one the snapshot describes is the one
 * in which it is protected.  Copying bytes into memory cannot block, which is
 * what makes that legal -- the rule these calls do NOT inherit is the one about
 * console writes.
 */
#ifndef NN_REPORT_H
#define NN_REPORT_H

#include <stddef.h>

#include "nn_svc.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Begin a capture: no bytes, no status yet.  Safe on a NULL capture, so a
 * board need not branch before it starts.
 */
void nn_report_begin(struct nn_report_capture *r);

/**
 * The sink, shaped as an @ref nn_svc_write_fn so a decoder writes to it exactly
 * as it would to a console.  @p ctx is the @ref nn_report_capture.
 *
 * [!] OVERFLOW LATCHES AND STOPS.  Once it has refused, it refuses everything
 * after -- a decoder that kept writing would otherwise leave a report with a
 * hole in the middle and no sign of one.  What fit is kept, deliberately: a
 * truncated first line is more use than none.
 *
 * @return the bytes taken, or negative once the buffer is full.
 */
int nn_report_write(void *ctx, const char *s, size_t len);

/**
 * Finish a capture, given what the decoder's own report() returned.
 *
 * [!] TRUNCATION OUTRANKS REFUSAL.  A decoder that propagated this sink's
 * refusal returns negative, and reporting that as "the decoder failed" would
 * blame it for running out of somebody else's buffer.  Overflow is checked
 * first for that reason.
 */
void nn_report_end(struct nn_report_capture *r, int rc);

/** State the capture as an outcome with no decoder involved. */
void nn_report_set(struct nn_report_capture *r, enum nn_report_status st);

#ifdef __cplusplus
}
#endif

#endif /* NN_REPORT_H */
