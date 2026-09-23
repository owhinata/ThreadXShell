/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    nn_rec.h
 * @brief   This board's decode record (issue #118): the last result `nn run` or
 *          the stream's producer published, which `nn dets` reads.
 *
 * The decisions are svc/nn_det_record.c's, shared with the other two boards;
 * what is here is the storage and this board's lock around it.
 *
 * [!] THE LOCK IS A SHORT INTERRUPT-DISABLED SECTION, NOT A MUTEX.  The producer
 * publishes from inside consume(), where nothing may block, sleep or take
 * another lock (CLAUDE.md, Grove).  A publish is a handful of stores and one
 * bounded clear; a snapshot is a copy.  Neither calls out of this file.
 *
 * [!] WHY A STOP CANNOT SUPERSEDE THE LAST RESULT HERE.  On wio a stop lands in
 * the middle of an inference, the decode that follows rewrites the plugin's
 * result, and its publish is then dropped (issue #118, c88d0ab).  This board's
 * stop takes its boundary only AFTER camera_stream_stop() has confirmed the
 * producer is out of consume() -- and the producer publishes inside consume(),
 * immediately after the decode.  So every decode the producer ran was published
 * before the boundary, and no decode can follow it.  `nn run` decodes and
 * publishes under the `nn` gate, which also excludes every boundary.
 */
#ifndef NN_REC_H
#define NN_REC_H

#include <stdint.h>

#include "nn_det_record.h"

#ifdef __cplusplus
extern "C" {
#endif

/** A session boundary: the generation moves, the result stays. */
void     nn_rec_boundary(void);
/** ...and the base a session counts its own publishes from, taken in the same
 *  critical section, so nothing can be accepted between the two. */
uint32_t nn_rec_boundary_base(void);
/** The model or its decoder changed: the result goes. */
void     nn_rec_invalidate(void);
/** The generation a publisher arms with. */
uint32_t nn_rec_gen(void);
/** A plugin's decode, negative values included; @return non-zero if taken. */
int      nn_rec_publish_external(int n, uint32_t gen);
/** An inference nothing decoded; @return non-zero if taken. */
int      nn_rec_publish_raw(uint32_t gen);
/** The result, all of it, in one critical section. */
void     nn_rec_snapshot(struct nn_det_snapshot *out);

#ifdef __cplusplus
}
#endif

#endif /* NN_REC_H */
