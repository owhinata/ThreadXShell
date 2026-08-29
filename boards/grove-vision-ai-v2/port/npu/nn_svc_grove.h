/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    nn_svc_grove.h
 * @brief   What this board's own `nn preview` needs from the nn_svc adapter
 *          (issue #50).
 *
 * The shared command speaks svc/nn_svc.h and knows nothing about a panel.  This
 * board's `preview` is a subcommand the shared table registers but does NOT
 * implement: it blocks, draws, and has a teardown whose failure modes are its
 * own (issues #48, #57, #63, #65).  Its handler therefore stays in this board's
 * cmds/, which is shell-layer code and may hold a shell instance -- and it
 * reaches the state and the claim through these queries, because that state now
 * lives in the port where the model lifecycle is.
 *
 * The dependency stays one way: the port declares this, cmds/ uses it, and
 * nothing in the port ever looks the other way.
 *
 * [!] TEMPORARY BY CONSTRUCTION.  Issue #50 deliberately did not reconcile this
 * board's blocking `preview` with the other two boards' background `stream`,
 * because that is the camera-producer and panel-guard ordering those issues
 * settled rather than a formatting difference.  The follow-up moves every board
 * to `nn stream start/stop/stats` over neutral hooks and REMOVES this header.
 */
#ifndef NN_SVC_GROVE_H
#define NN_SVC_GROVE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Take the transient claim that covers every `nn` job on this board.
 *
 * Single-instance rather than a mutex: two consoles can both reach `nn`, and
 * what must be prevented is two jobs inside the NPU at once.
 *
 * @return non-zero if it was taken.
 */
int nn_svc_grove_acquire(void);

/**
 * Give the claim back.
 *
 * [!] `nn preview` DOES NOT ALWAYS CALL THIS, and that is the design rather
 * than an omission.  When a stop or a panel drain is unconfirmed, a thread may
 * still be inside the sink reading detections, and releasing here would let a
 * later unload dismantle the interpreter underneath it.  Holding the claim
 * until reboot is the correct trade.
 */
void nn_svc_grove_release(void);

/** Is a model active?  Read under the claim. */
int nn_svc_grove_model_open(void);

/**
 * Would this model actually annotate frames?
 *
 * Settled BEFORE a preview starts, where refusing costs nothing and can say
 * why: a preview that starts and then fails on every frame is a panel showing a
 * live picture with no boxes and no explanation, which is the exact failure the
 * command exists to make visible.
 *
 * @param why  filled with the reason when the answer is no.  A CALLER-OWNED
 *             buffer, not a pointer into this port: two consoles must not be
 *             able to overwrite each other's explanation.
 * @return 0 when a decode would find its tensors
 */
int nn_svc_grove_detector_ready(char *why, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* NN_SVC_GROVE_H */
