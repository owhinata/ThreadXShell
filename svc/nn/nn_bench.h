/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    nn_bench.h
 * @brief   `nn bench` once for every board: hold -> fill -> N timed runs ->
 *          let go, in ONE hold (issue #163).
 *
 * Until issue #163 each board had two calls, a prepare (hold, fill the inputs,
 * let go) and a run (hold again, invoke N times, let go), and nothing held the
 * model between them: a load on another console could replace the model the
 * prepare had just filled.  The procedure is now this one file and the board
 * passes only its facts through a const @ref nn_bench_ops: how it takes and
 * gives its hold, and how it tells time.
 *
 * [!] THE SHARED SIDE NEVER WAITS AND HOLDS NO LOCK.  Every hold is the board's
 * (svc/README.md, "Why the lock and the threads are the board's"); this file
 * calls the hooks in a fixed order and keeps its numbers on the caller's stack.
 * It owns no mutable storage (the build's audit checks that per board).
 */
#ifndef NN_BENCH_H
#define NN_BENCH_H

#include <stdint.h>

#include "nn.h"
#include "nn_svc.h"

/** The byte every input is filled with, on every board (issue #163; f746
 *  filled 0x00 before).  The value does not matter, that there is one does. */
#define NN_BENCH_FILL 0x5Au

/**
 * The board's time-keeping for one bench, on the CALLER's stack.  Zeroed by
 * nn_bench() before the first hook; what the two words mean is the board's
 * (a clock read once, a tick at the start of a run).
 */
struct nn_bench_clock {
	uint32_t mhz;
	uint32_t t0;
};

/**
 * What a board tells the shared bench.  A const object in the board's own
 * translation unit.
 *
 * [!] acquire OWNS ITS FAILURES.  On success it returns the model with the
 * board's hold taken and the model asked for under that hold; on failure it
 * writes the status and the words into @p res, gives back whatever it had taken
 * and returns NULL.  nn_bench() never calls release after a failed acquire, and
 * calls it exactly once on every exit after a successful one.
 */
struct nn_bench_ops {
	struct nn_model *(*acquire)(struct nn_op_result *res);
	void (*release)(void);
	/** Optional: once, under the hold, before the first run (a clock the
	 *  board reads once for the whole bench). */
	void (*clock_start)(struct nn_bench_clock *c);
	/** Optional: right before each nn_run(), after the cancel question. */
	void (*run_begin)(struct nn_bench_clock *c);
	/** After each SUCCESSFUL nn_run(): that run in microseconds. */
	uint32_t (*run_end_us)(struct nn_bench_clock *c, const struct nn_model *m);
	/** Optional: the clock the conversion assumed, asked once after the last
	 *  run of a bench that succeeded; NULL reports 0 (real time counted
	 *  directly). */
	uint32_t (*clock_mhz)(const struct nn_bench_clock *c);
};

/**
 * Hold, fill every input with NN_BENCH_FILL, invoke @p iters times -- asking
 * @p cancel once before each run -- and let go.  The result's disposition is
 * NN_CLAIM_NONE on every path: nothing is left held.
 *
 * An input whose tensor or data pointer is NULL is skipped, not an error: the
 * fill is for comparability and an input with no storage has nothing to fill.
 */
void nn_bench(const struct nn_bench_ops *ops, uint32_t iters,
              struct nn_bench_stats *out, nn_svc_cancel_fn cancel, void *ctx,
              struct nn_op_result *res);

#endif /* NN_BENCH_H */
