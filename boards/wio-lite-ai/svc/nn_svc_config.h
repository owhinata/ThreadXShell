/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    nn_svc_config.h
 * @brief   What the shared `nn` command has on Wio Lite AI (issue #50).
 *
 * The board says what it HAS; the shared command registers a subcommand only
 * under its macro.  These are properties, never a board identity test.
 */
#ifndef NN_SVC_CONFIG_H
#define NN_SVC_CONFIG_H

/*
 * [!] TWO CONDITIONS, AND BOTH ARE PROPERTIES RATHER THAN THE BOARD.  A model
 * can be pointed somewhere new only if a backend can interpret one at run time
 * AND there is an asset store to read it out of.  The `null` backend has no
 * loader, and a build without the external NOR has nowhere to load FROM, so in
 * either case `nn model load` would be a subcommand that always refuses.
 */
#if defined(CONFIG_NN_BACKEND_TFLM) && BSP_ENABLE_KV
#define NN_SVC_HAS_MODEL_LOAD  1
#endif

/** One-shot capture and infer, from the camera's band stream. */
#define NN_SVC_HAS_CAMERA      1

/** Repeat-invoke timing.  This board already had it as `ai bench`. */
#define NN_SVC_HAS_BENCH       1

/** Float input normalisation: this board's inputs can be float, so the range is
 *  a real choice here. */
#define NN_SVC_HAS_NORM        1

/** Boxes drawn on the LTDC preview.  This board is the only one where the
 *  overlay can be turned OFF independently -- the other panel board draws
 *  whenever its preview runs. */
#define NN_SVC_HAS_OVERLAY     1

/** A background inference worker with stop and stats.  The handler stays this
 *  board's own for now -- see cmds/cmd_nn_board.c and issue #50's deferral. */
#define NN_SVC_HAS_STREAM      1

/* NOT `preview`: this board's live inference is the background worker above,
   and the overlay rides on the camera's own LTDC preview rather than on a
   blocking command of its own. */

#endif /* NN_SVC_CONFIG_H */
