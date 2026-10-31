/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    npu_cache.h
 * @brief   The CPU/NPU arena ownership protocol (issue #46): its state, the
 *          board primitives it runs on, and the two calls the backend makes
 *          (shared since issue #169).
 *
 * The protocol itself -- the whole arena at two points, the vendor's
 * invalidate hook left empty, DONE and OK both required, a checked reset before
 * any invalidate on failure, fail-stop otherwise -- is npu_cache.c, next to
 * this header, and holds no storage of its own.  What is a fact about a board
 * is the board's, bound at link time like nn_tflm_board_state:
 *
 *   npu_cache_board_state   the state object (defined by the board)
 *   npu_cache_clean()       one D-cache clean / invalidate over a range,
 *   npu_cache_invalidate()  rounded outward to whole lines (the board's
 *                           cache-line primitive)
 *   npu_cache_fail_stop()   log and halt, never return
 *
 * Including no board header and no tx_api.h, like everything under svc/.
 */
#ifndef NPU_CACHE_H
#define NPU_CACHE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * The protocol's state: the arena npu_open() handed over, and where the
 * handover stands.  `arena_cleaned` says the pre-launch clean ran;
 * `inference_armed` says the command stream was launched and the handover back
 * is still owed.  Zero is the boot state of every field (no arena, nothing
 * owed), so the board defines it without an initialiser.
 */
struct npu_cache_state {
	void             *arena_base;
	size_t            arena_size;
	volatile uint8_t  arena_cleaned;
	volatile uint8_t  inference_armed;
};

/** Defined by the board (one object, zero-initialised). */
extern struct npu_cache_state npu_cache_board_state;

/* --- what the board provides ------------------------------------------- */

/* One D-cache operation over a range.  Ranges are rounded outward to whole
 * cache lines, so the caller may pass a sub-range of the arena; NULL or zero
 * bytes does nothing. */
void npu_cache_clean(const void *p, size_t bytes);
void npu_cache_invalidate(void *p, size_t bytes);

/**
 * Stop, loudly and permanently.
 *
 * Reached only when the CPU is about to touch an arena the NPU may still own
 * and there is no way left to establish otherwise.  Returning would corrupt
 * silently and intermittently, which is strictly worse than halting.  How to
 * log and how to halt are the board's.
 */
void npu_cache_fail_stop(const char *why) __attribute__((noreturn));

/* --- what the protocol provides ---------------------------------------- */

/**
 * Hand the arena to the cache protocol, or take it back (NULL, 0).
 *
 * The protocol maintains the WHOLE arena at two points and never a sub-range --
 * see npu_cache.c for why -- so it has to know where the arena is.  This is the
 * arena npu_open() was actually given, not the static reservation, and it is
 * cleared on close and on every failure unwind.
 */
void npu_cache_set_arena(void *base, size_t bytes);

/**
 * Close out one Invoke().  Called after MicroInterpreter::Invoke() returns,
 * whatever it returned.
 *
 * Two jobs.  It clears the "arena was cleaned" state left by a launch that
 * never happened -- a payload the driver rejected before it reached
 * ethosu_inference_begin().  And it is where an inference that launched but
 * never completed the handover is caught: that would mean the CPU is about to
 * touch an arena the NPU may still own, so it fail-stops rather than return.
 */
void npu_cache_after_invoke(void);

#ifdef __cplusplus
}
#endif

#endif /* NPU_CACHE_H */
