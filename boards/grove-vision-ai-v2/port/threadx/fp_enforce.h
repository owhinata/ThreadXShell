/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    fp_enforce.h
 * @brief   The floating-point context precondition, and its verdict (issue #42).
 *
 * WHAT THIS IS FOR.  `FPCCR.ASPEN` is what makes the PE set `CONTROL.FPCA` when
 * a thread executes a floating-point OR MVE instruction (Armv8-M ARM, rule
 * RZWQX), and `CONTROL.FPCA` is what makes the exception frame carry the
 * caller-saved vector state -- S0-S15, FPSCR and, on a part with MVE, VPR at
 * offset 0x44.  The ThreadX Cortex-M55 port then adds the callee-saved half
 * (`{s16-s31}`), but ONLY when the EXC_RETURN says that frame exists.
 *
 * So with ASPEN clear the whole chain silently stops: no frame, no software
 * save, and vector state that crosses a preemption is whatever the other thread
 * left behind.  That is not hypothetical here -- the prebuilt Himax driver
 * archives execute MVE (issue #66 counted 80 vector load/stores), so this image
 * has depended on ASPEN since long before this project's own code was allowed
 * anywhere near a vector register.
 *
 * This app does not write FPCCR anywhere else and neither does the SDK's
 * SystemInit(), so the value is inherited from the bootloader.  Inherited state
 * is exactly what this port reads back rather than trusts (the same rule as
 * SystemCoreClock and the WFI preconditions), and the read-back is the
 * enforcement -- the write alone proves nothing.
 *
 * The DECISION lives here, apart from the halt, so it can be tested: a boot
 * where ASPEN refuses to set cannot be produced on hardware, and a check nobody
 * has seen fail is a check nobody should believe (issue #66 is this repository's
 * own example of that).  test/test_fp_enforce.c walks the table.
 */
#ifndef FP_ENFORCE_H
#define FP_ENFORCE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* FPCCR bits this port reasons about, from the Armv8-M ARM.  The port TU
 * static-asserts these against CMSIS's own masks, so a rename upstream cannot
 * leave this header quietly describing a different register. */
#define FP_FPCCR_ASPEN   (1u << 31)  /**< automatic FP context creation       */
#define FP_FPCCR_LSPEN   (1u << 30)  /**< lazy state preservation enabled     */
#define FP_FPCCR_LSPACT  (1u << 0)   /**< a lazy save is OUTSTANDING          */
#define FP_FPCCR_TS      (1u << 26)  /**< FP state is Treated as Secure       */

enum fp_enforce_verdict {
	FP_ENFORCE_OK = 0,          /**< ASPEN is set and nothing is pending  */
	FP_ENFORCE_LSPACT,          /**< a lazy save was already active       */
	FP_ENFORCE_ASPEN_REFUSED,   /**< the write did not take               */
	FP_ENFORCE_TS_REFUSED,      /**< TS would not clear (issue #103)      */
};

/**
 * @brief  Judge the FPCCR values read either side of the enforcing write.
 *
 * @param before  FPCCR as inherited, sampled BEFORE the write
 * @param after   FPCCR read back after the write, with a barrier between
 *
 * [!] LSPACT IS JUDGED ON `before`, AND IT IS REJECTED RATHER THAN CLEARED.
 * The bit says a lazy floating-point save is outstanding for a stack frame this
 * application does not own -- clearing it would discard the architecture's own
 * bookkeeping about somebody else's memory, and continuing would leave every
 * later context switch reasoning about a state nobody wrote down.  A handover
 * into Thread mode has no business carrying one; a bootloader that does is an
 * incompatibility to diagnose, not a condition to absorb.
 *
 * LSPEN is deliberately NOT judged: lazy and eager stacking are both correct
 * here (under lazy, the port's own `{s16-s31}` save is the FP instruction that
 * materialises the deferred frame first), and which one is in force is the
 * bootloader's choice to make.
 *
 * [!] TS IS ENFORCED TO ZERO, AND THAT IS A DIFFERENT KIND OF ACT FROM
 * REFUSING LSPACT (issue #103).  Both bits arrive inherited, and the difference
 * is what they describe:
 *
 *   LSPACT is BOOKKEEPING about somebody else's outstanding frame.  Nothing
 *   here can make that frame consistent, so the only honest response is to
 *   refuse.
 *
 *   TS is a POLICY bit about how FUTURE floating-point context is treated, and
 *   in this application the policy is ours to set: the whole image is Secure
 *   with the SAU disabled, so there is no Non-secure world for TS to protect FP
 *   state from.
 *
 * What TS costs when set is stack.  With TS == 1 an exception taken from Secure
 * state stacks `s16-s31` on top of the standard extended frame -- 64 B more,
 * turning a 108 B worst case into 172 B on the stack of whatever thread was
 * interrupted.  Issue #103 has to state an exception reserve for threads that
 * call into a loaded plugin, and a reserve derived from a bit the bootloader
 * happens to leave clear is not a reserve.  So it is written, read back, and
 * the boot is refused if it did not take.
 */
enum fp_enforce_verdict fp_enforce_judge(uint32_t before, uint32_t after);

/** @return a short description of a verdict (never NULL). */
const char *fp_enforce_strerror(enum fp_enforce_verdict v);

#ifdef __cplusplus
}
#endif

#endif /* FP_ENFORCE_H */
