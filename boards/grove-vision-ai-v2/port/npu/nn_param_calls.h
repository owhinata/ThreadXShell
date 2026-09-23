/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    nn_param_calls.h
 * @brief   The `nn` gate's holder, and how many threshold calls are inside the
 *          plugin right now (issue #122).
 *
 * `nn thresh` does not take the gate: a stream holds it for its whole life, and
 * a threshold is what an operator adjusts while watching one.  So it did not
 * take anything, and since a load can replace an open model (issue #122 stage
 * 4) one command on another console could copy a new plugin over the code a
 * threshold call was executing.  Unpublishing a slot table does not revoke a
 * function pointer somebody already holds.
 *
 * The answer is a count, not the gate: a threshold call is COUNTED IN while it
 * runs the plugin's param function (refused only while an operation that can
 * replace or remove the plugin holds the gate), and such an operation is
 * refused its claim while the count is not zero.  Nobody waits -- both refusals
 * are BUSY.
 *
 * [!] EACH FUNCTION HERE IS ONE WHOLE TRANSITION -- the test and the change --
 * and the caller runs it inside ONE critical section.  Testing in one section
 * and changing in another would let a threshold call and a swap each see the
 * other absent and both proceed, which is the race this exists to close.  That
 * the caller does so is held by its short wrappers in nn_svc_grove.c, not by
 * this file.
 *
 * WHAT IT DOES NOT CLOSE: the camera producer's decode on a running stream and
 * a threshold call can still be inside the same plugin at once (issue #122 P5,
 * Phase 3a).  Neither of them replaces the plugin.
 */
#ifndef NN_PARAM_CALLS_H
#define NN_PARAM_CALLS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** WHO holds the gate (issue #99).  Not @ref nn_claim, which is the caller's
 *  cleanup authority. */
enum nn_owner {
	NN_OWNER_NONE = 0,
	NN_OWNER_OP,      /**< one operation, which releases before it returns */
	NN_OWNER_STREAM,  /**< a running stream, until its stop               */
	NN_OWNER_SWAP,    /**< an operation that replaces or removes the plugin
	                   *   -- a load or an unload (issue #122)           */
};

/** How many threshold calls can be counted in at once.  Far above the threads
 *  that could make one; reaching it is refused rather than wrapped to zero. */
#define NN_PARAM_CALLS_MAX 0xFFFFu

/**
 * Claim the gate for @p who.  Refused when it is held -- and, for
 * NN_OWNER_SWAP, while any threshold call is inside the plugin.
 *
 * @return 1 claimed (busy and owner set), 0 refused (nothing changed)
 */
int nn_gate_claim(uint8_t *busy, uint8_t *owner, uint16_t calls, uint8_t who);

/**
 * Count a threshold call in.  Refused while an NN_OWNER_SWAP holds the gate;
 * any other holder -- a stream, `nn run` -- leaves the plugin where it is and
 * does not refuse.
 *
 * @return 1 counted in, 0 refused (nothing changed)
 */
int nn_param_calls_enter(uint8_t busy, uint8_t owner, uint16_t *calls);

/** Count one out.  Never below zero. */
void nn_param_calls_leave(uint16_t *calls);

#ifdef __cplusplus
}
#endif

#endif /* NN_PARAM_CALLS_H */
