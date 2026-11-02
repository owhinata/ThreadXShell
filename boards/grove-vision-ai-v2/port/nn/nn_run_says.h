/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    nn_run_says.h
 * @brief   What a `nn run` whose one frame ended without a result says on this
 *          board: the status and the sentence for each NN_OV_SHOT_* (issue
 *          #174).
 *
 * [!] WORDS ONLY.  This board's one-shot keeps its own state (nn_ov_shot,
 * nn_camera.c): one byte, armed while the worker is parked, written once by the
 * producer or the worker -- one frame by construction, so svc/nn/nn_shot.c's
 * run numbers and re-arm answer protect nothing here (issue #174 D8).  What is
 * shared is the WORDS: an ending the shared table names (prep, invoke, lease
 * not had in time, decode reached unleased) reads exactly as it does on
 * wio-lite-ai and f746g-disco, from nn_shot_says().  What only this board has
 * keeps its own sentence:
 *
 *   NO_OUTPUTS  "output <n> of the model is unreadable" -- it knows WHICH
 *   PUBLISHED   the worker published, the record did not take it
 *   STOPPED     a stop came first (and anything undocumented), with the code
 *
 * [!] THE STATUS IS UNCHANGED BY THE MOVE: LEASE_TIMEOUT is BUSY (nothing was
 * decoded and the same run may well be decoded next time), every other ending
 * HW.  shell/test's sibling, boards/grove-vision-ai-v2/test/test_nn_run_says.c,
 * holds each value to it.
 *
 * Pure: no lock, no storage, no tx_api.h.
 */
#ifndef NN_RUN_SAYS_H
#define NN_RUN_SAYS_H

#include "nn_svc.h"   /* struct nn_op_result */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Write the sentence for one-shot ending @p shot (NN_OV_SHOT_*) into
 * @p res->detail and return its status.  @p out_index is the unreadable
 * output's index (nn_camera_shot_index()), read only for NO_OUTPUTS.  The
 * claim is not touched: it is the teardown's.
 */
int nn_run_no_result_says(int shot, unsigned out_index,
                          struct nn_op_result *res);

#ifdef __cplusplus
}
#endif

#endif /* NN_RUN_SAYS_H */
