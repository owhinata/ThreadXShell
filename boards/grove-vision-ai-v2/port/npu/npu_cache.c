/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    npu_cache.c
 * @brief   The CPU/NPU arena ownership protocol, owned by this port (issue #46).
 *
 * The Ethos-U55 is an AXI bus master and is NOT coherent with the CM55's
 * D-cache, so every handover of the arena needs explicit maintenance.  Issue
 * #44 put that maintenance in the shell command, per range.  That does not
 * work, for three independent reasons found while planning issue #45:
 *
 *   - TFLM aligns arena buffers to 16 bytes (micro_arena_constants.h) and this
 *     core's cache line is 32.  A per-range operation therefore rounds outward
 *     across a NEIGHBOUR's half-line: an invalidate can discard what the CPU
 *     wrote there, and a clean can write a stale half back over what the NPU
 *     wrote.  Measuring the model's own I/O does not close this, because the
 *     boundaries that matter are internal -- the NPU writes intermediate
 *     feature maps all through the arena while the CPU writes the ethos-u
 *     kernel's base_addrs scratch (RequestScratchBufferInArena) and TFLM's
 *     persistent allocator at the tail.
 *   - Maintenance done by the CALLER after Invoke() returns is always too late.
 *     TFLM calls ResetTempAllocations() immediately after the kernel returns
 *     and BEFORE it checks the kernel's status (micro_interpreter_graph.cc),
 *     which writes the arena-resident allocator's next_temp_.
 *   - Only ONE of the two vendor hooks was mistimed.  ethosu_flush_dcache() is
 *     called after Eval() has built its base-address arrays and immediately
 *     before the power request and the command-stream launch -- exactly the
 *     last-writer boundary we want.  It was neutered along with the other one.
 *
 * So this file stops maintaining ranges and maintains the WHOLE ARENA at two
 * points, which makes alignment structurally irrelevant: the arena is 32-byte
 * aligned and a whole number of lines by _Static_assert (npu_arena.c), so no
 * partial line can ever be involved.
 *
 *   flush hook (pre-launch)     CPU -> NPU   clean the whole arena
 *   inference_begin             .            arm
 *   ... NPU runs ...            NPU          calling thread suspended on the
 *                                            driver semaphore; every other
 *                                            thread excluded by cmd_nn.c's
 *                                            ownership gate
 *   inference_end, DONE && OK   NPU -> CPU   invalidate the whole arena
 *   inference_end, otherwise    NPU -> CPU   soft reset, CHECKED, then
 *                                            invalidate -- or fail-stop
 *   after Invoke() returns      CPU          nothing
 *
 * ethosu_invalidate_dcache() stays a no-op because its call site is still
 * wrong: ethosu_wait() runs it BEFORE taking the completion semaphore, i.e.
 * while the NPU may still be writing.  The asymmetry is the point.
 *
 * WHY inference_begin/end AND NOT THE SEMAPHORE HOOK.  npu_rtos.c owns
 * ethosu_semaphore_take() and a successful take does mean completion, but that
 * hook is not inference-specific (driver reservation takes the same path) and
 * it is never reached when power acquisition fails or the inference times out,
 * so a flag raised beside it would be left standing.  ethosu_inference_begin()
 * and ethosu_inference_end() are declared weak by the driver for exactly this
 * purpose, receive the driver, and sit at the right instants -- begin is
 * reachable only after the power request succeeded, and the driver's own
 * comment says end is "always called even in case of timeout".
 *
 * DONE IS NOT SUCCESS.  The interrupt handler sets state = DONE and then sets
 * result from ethosu_dev_handle_interrupt(), so a fault also lands as DONE;
 * and result INITIALISES to OK, so it alone does not prove the job ran.  Both
 * are required.
 *
 * RESET IS NOT ASSUMED.  On timeout or NPU error the driver calls
 * (void)ethosu_soft_reset(drv) and discards the result.  This file resets
 * first and checks, because "the NPU has stopped writing" is the precondition
 * for invalidating at all.  Resetting twice is redundant; resetting never is
 * not.
 *
 * The one path this protocol cannot cover from here is a payload whose driver
 * actions continue AFTER the command stream is launched: ethosu_invoke_async()
 * would then be able to fail and return without ever reaching
 * ethosu_inference_end().  That is closed at the other end, in npu_tflm.cc,
 * which refuses to open a model whose payload is not exactly one final
 * COMMAND_STREAM.  npu_cache_after_invoke() is the backstop if it ever is.
 */
#include "npu.h"
#include "npu_hw.h"

#include "ethosu_driver.h"   /* struct ethosu_driver, ethosu_soft_reset() */
#include "WE2_device.h"      /* __disable_irq                             */
#include "WE2_core.h"        /* hx_{Clean,Invalidate}DCache_by_Addr        */
#include "log.h"             /* LOG_ERR                                   */

#define CACHE_LINE 32u

/* The arena the protocol maintains, handed over by npu_open(). */
static void  *arena_base;
static size_t arena_size;

/* Protocol state.  `cleaned` says the pre-launch clean ran; `armed` says the
 * command stream was launched and the handover back is still owed. */
static volatile uint8_t arena_cleaned;
static volatile uint8_t inference_armed;

/*
 * Stop, loudly and permanently.
 *
 * Reached only when the CPU is about to touch an arena the NPU may still own
 * and there is no way left to establish otherwise.  Returning would corrupt
 * silently and intermittently, which is strictly worse than halting: the same
 * judgement tx_glue.c makes when the WFI preconditions do not hold.
 */
static void npu_cache_fail_stop(const char *why)
{
	LOG_ERR("npu cache: %s -- halting", why);
	__disable_irq();
	for (;;)
		;
}

static void arena_clean_all(void)
{
	npu_cache_clean(arena_base, arena_size);
}

static void arena_invalidate_all(void)
{
	npu_cache_invalidate(arena_base, arena_size);
}

void npu_cache_set_arena(void *base, size_t bytes)
{
	arena_base      = base;
	arena_size      = bytes;
	arena_cleaned   = 0u;
	inference_armed = 0u;
}

void npu_cache_after_invoke(void)
{
	/* Armed here means the command stream was launched and
	 * ethosu_inference_end() never ran, so nothing has established that the
	 * NPU stopped writing -- and TFLM has ALREADY written the arena on its way
	 * out of Invoke().  npu_tflm.cc's payload check exists to make this
	 * unreachable; if it is reached, the check is wrong. */
	if (inference_armed)
		npu_cache_fail_stop("inference launched but never handed the arena back");

	/* Not armed: either no launch happened (the driver rejected the payload
	 * before ethosu_inference_begin()) or the handover completed.  Either way
	 * the next inference must do its own clean. */
	arena_cleaned = 0u;
}

/* --- the vendor hooks ---------------------------------------------------- */

/*
 * Override the SDK's non-empty weak hooks.  Strong definitions here win at link
 * time; check the build's symbol table if that ever seems not to happen,
 * because a silent revert to the vendor versions restores BOTH the early
 * invalidate and the per-range clean.
 */
void ethosu_flush_dcache(uint32_t *p, size_t bytes)
{
	/* The driver's arguments are deliberately ignored.  Its default flush mask
	 * selects base pointer 1 -- Vela's scratch tensor, with a length derived
	 * from tensor dimensions -- which is not the arena.  Cleaning the whole
	 * arena instead is what makes the post-completion invalidate safe by
	 * construction rather than by an argument about where Vela put things. */
	(void)p;
	(void)bytes;

	if (arena_base == NULL)
		npu_cache_fail_stop("flush with no arena");

	arena_clean_all();
	arena_cleaned = 1u;
}

void ethosu_invalidate_dcache(uint32_t *p, size_t bytes)
{
	(void)p;
	(void)bytes;
	/* Deliberately nothing: this one IS called at the wrong time -- before the
	 * completion semaphore, while the NPU may still be writing.  The handover
	 * happens in ethosu_inference_end() below. */
}

/* --- the handover -------------------------------------------------------- */

void ethosu_inference_begin(struct ethosu_driver *drv, void *user_arg)
{
	(void)drv;
	(void)user_arg;

	/* [!] A SECOND launch while one is still owed must not be absorbed.
	 * The driver runs handle_command_stream() once per COMMAND_STREAM action
	 * -- cleaning, arming and launching each time -- but calls ethosu_wait()
	 * only once, after the whole walk.  So two streams in one payload would
	 * arm twice and disarm once, and the boolean backstop in
	 * npu_cache_after_invoke() would see 0 and pass.  npu_tflm.cc refuses such
	 * a payload at open, and this is the second lock on the same door: a
	 * counter would silently tolerate the case, so it is fatal instead. */
	if (inference_armed)
		npu_cache_fail_stop("second launch while an inference is still owed");

	/* Unreachable unless the flush hook was bypassed, which would mean the
	 * NPU is about to read an arena the CPU still holds dirty. */
	if (!arena_cleaned)
		npu_cache_fail_stop("launch without the pre-launch clean");

	inference_armed = 1u;
}

void ethosu_inference_end(struct ethosu_driver *drv, void *user_arg)
{
	(void)user_arg;

	if (drv == NULL)
		npu_cache_fail_stop("inference_end without a driver");
	if (!inference_armed)
		npu_cache_fail_stop("inference_end without a launch");

	if (drv->job.state == ETHOSU_JOB_DONE &&
	    drv->job.result == ETHOSU_JOB_RESULT_OK) {
		/* Confirmed complete: the interrupt handler wrote both fields before
		 * giving the semaphore. */
		arena_invalidate_all();
	} else {
		/* Timed out, or the NPU reported an error.  Either way it may still
		 * have bus activity in flight, so stop it and CHECK before touching
		 * the arena.  The driver resets again after this returns; that is
		 * redundant, not harmful. */
		if (ethosu_soft_reset(drv) != 0)
			npu_cache_fail_stop("NPU reset failed after a failed inference");
		arena_invalidate_all();
	}

	inference_armed = 0u;
	arena_cleaned   = 0u;
}

/* Round a range outward to whole cache lines. */
static void line_span(const void *p, size_t bytes, uint32_t *start,
                      int32_t *len)
{
	uint32_t lo = (uint32_t)(uintptr_t)p & ~(CACHE_LINE - 1u);
	uint32_t hi = ((uint32_t)(uintptr_t)p + (uint32_t)bytes + CACHE_LINE - 1u)
	              & ~(CACHE_LINE - 1u);

	*start = lo;
	*len   = (int32_t)(hi - lo);
}

void npu_cache_clean(const void *p, size_t bytes)
{
	uint32_t start;
	int32_t  len;

	if (p == NULL || bytes == 0u)
		return;
	line_span(p, bytes, &start, &len);
	hx_CleanDCache_by_Addr((volatile void *)start, len);
}

void npu_cache_invalidate(void *p, size_t bytes)
{
	uint32_t start;
	int32_t  len;

	if (p == NULL || bytes == 0u)
		return;
	line_span(p, bytes, &start, &len);
	hx_InvalidateDCache_by_Addr((volatile void *)start, len);
}
