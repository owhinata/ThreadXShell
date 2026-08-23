/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    npu_hw.h
 * @brief   Ethos-U55 bring-up and cache maintenance, port-internal (issue #44).
 *
 * Not part of npu.h: the shell command only ever calls npu_open/invoke/close,
 * while these are the pieces those depend on and that only port/npu/ arranges.
 */
#ifndef NPU_HW_H
#define NPU_HW_H

#include <stddef.h>
#include <stdint.h>

/*
 * Finite inference timeout, in ThreadX ticks (1 ms).  The driver header would
 * otherwise define ETHOSU_SEMAPHORE_WAIT_INFERENCE as "wait forever", and a
 * lost NPU interrupt would then suspend the calling shell job permanently.
 *
 * [!] THIS LINE IS THE BUILD'S SOURCE OF TRUTH.  board.cmake PARSES it and
 * passes the value to the ethos-u driver as ETHOSU_SEMAPHORE_WAIT_INFERENCE;
 * configure fails if the line cannot be found.  It used to be written here as
 * documentation AND separately as a literal in board.cmake, where only the
 * literal was live -- so this constant was dead, and would have drifted the
 * first time anyone tuned it.
 *
 * One second, down from five since issue #48.  Two things changed: inference
 * now runs on the camera producer thread, where the wait sits inside the
 * window camera_stream_stop() has to join; and the ethos-u driver takes this
 * semaphore TWICE on its timeout/interrupt race path, so the budget is 2x this
 * number.  One second is still about 11x the worst measured inference (92 ms
 * for classification, 13 ms for the detector) -- generous enough that a model
 * or clock change will not trip it, tight enough that two of them plus the
 * panel's DMA timeout stay well inside CAM_STOP_JOIN_TICKS.
 */
#define NPU_INFERENCE_TIMEOUT_TICKS 1000u

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Power up, secure, reset and register the NPU, and account its interrupts.
 *
 * FAIL CLOSED.  Every step is read back where the SCU offers a getter, and any
 * mismatch aborts the whole bring-up with the hardware returned to where it
 * started -- no half-configured NPU, no interrupt left enabled but unaccounted.
 * The caller loses inference and keeps a working shell.
 *
 * @return 0 on success, negative on failure (see npu_hw_fail_reason()).
 */
int npu_hw_init(void);

/** Undo npu_hw_init(): stop the NPU, unwrap and disable its interrupts. */
void npu_hw_deinit(void);

/** One line saying why the last npu_hw_init() refused; NULL if it succeeded. */
const char *npu_hw_fail_reason(void);

/** True once npu_hw_init() has succeeded and npu_hw_deinit() has not run. */
int npu_hw_ready(void);


/**
 * The interrupt lines this bring-up wrapped for the execution profile kit.
 *
 * Reported so the "no line enabled but unaccounted" rule is VISIBLE and not
 * merely structural.  It is already structural -- npu_hw_init() refuses if the
 * wrap fails -- but a rule nobody can see the state of is one nobody checks.
 *
 * @param out    filled with up to @p max line numbers
 * @return how many lines are wrapped (0 when the NPU is down)
 */
unsigned npu_hw_wrapped_irqs(int *out, unsigned max);

/* [!] There is no violation-status read.  The SDK's SCU surface exposes
 * hx_drv_scu_set_u55_m{0,1}_msc_irq_clear() and the configuration getters, but
 * nothing that reports whether a master was filtered.  So the MSC is configured
 * for SCU_MSC_RESP_ERR instead of the RAZ/WI default: a blocked NPU access
 * raises a bus error rather than reading as zeros, which surfaces as a fault
 * this port already records to dmesg.  Loud beats silent -- zeros would reach
 * TFLM as a model or an arena full of nothing and be interpreted as data. */

/* Cache maintenance (npu_cache.c).  Ranges are rounded outward to whole cache
 * lines, so the caller may pass a sub-range of the arena. */
void npu_cache_clean(const void *p, size_t bytes);
void npu_cache_invalidate(void *p, size_t bytes);

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

#endif /* NPU_HW_H */
