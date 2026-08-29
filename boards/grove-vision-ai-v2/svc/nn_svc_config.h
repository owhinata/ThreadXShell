/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    nn_svc_config.h
 * @brief   What the shared `nn` command has on Grove Vision AI V2 (issue #50).
 *
 * The board says what it HAS; the shared command registers a subcommand only
 * under its macro, so `help` on this board is an inventory of this board.  These
 * are properties, never a board identity test -- see shell/cmds/cmd_nn.c.
 */
#ifndef NN_SVC_CONFIG_H
#define NN_SVC_CONFIG_H

/** A model is opened from the asset store by name, or from a raw address with a
 *  length.  Always available: this board has no compile-time model at all. */
#define NN_SVC_HAS_MODEL_LOAD  1

/** One-shot capture and infer.  The camera and the NPU are both here. */
#define NN_SVC_HAS_CAMERA      1

/** Repeat-invoke timing.  New on this board with issue #50 -- the other two had
 *  it and this one did not, which was a gap rather than a decision. */
#define NN_SVC_HAS_BENCH       1

/*
 * NOT float input normalisation.  This board's path is int8 throughout and
 * fills the input as (pixel - 128), so there is no float range to choose
 * between -- `nn norm` would be a setting that does nothing.  Absent rather
 * than stubbed: `help` listing a control that cannot control anything is worse
 * than not listing it.
 */

/**
 * Live inference, as `nn stream start/stop/stats` (issue #99).
 *
 * This board's live inference used to be `nn preview`, which BLOCKED and drew
 * on the panel while the other two had a background worker.  Issue #99 moved
 * all three onto one grammar over neutral start/poll/stop hooks and removed
 * `preview`; the panel drawing is unchanged, it simply outlives the command
 * now.  On a board with ONE console that is the bigger half of the change --
 * `camera stats` and `nn info` during a run were previously unreachable.
 *
 * NOT NN_SVC_HAS_STREAM_TEST: there is no sensor test pattern here, and a
 * `--test` that always refused would be an option listed in `help` that cannot
 * do anything.
 */
#define NN_SVC_HAS_STREAM      1

#endif /* NN_SVC_CONFIG_H */
