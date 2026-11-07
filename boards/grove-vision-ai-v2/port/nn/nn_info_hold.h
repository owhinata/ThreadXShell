/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    nn_info_hold.h
 * @brief   Where this board's `nn info` model section comes from, and which
 *          state of the load order it belongs to (issues #99, #199).
 *
 * `nn info` tries the gate.  Taken, it copies everything under it.  Held by a
 * stream, it copies the identity in the interrupt-masked section that also
 * reads the owner, because no load can run while a stream holds the gate.
 * Held by anything else, the model section is BUSY.  That much is #99's and is
 * unchanged; what issue #199 adds is the copy's VALUE of the load order's
 * transition counter (svc/nn/nn_svc.h's nn_svc_info::seq), which the lines
 * read after `nn info` returns are checked against.
 *
 * [!] A VALUE ONLY WHERE THE HOLDER KEEPS THE COUNTER STILL.  A load or an
 * unload claims this same gate as NN_OWNER_SWAP and moves the counter inside
 * that hold -- so for a SWAP holder the counter can be even BEFORE it goes odd
 * and even AGAIN after the model has changed, and an even reading says nothing.
 * The holders that cannot be a load are this call (the gate taken), a stream
 * and an ordinary operation (`nn run`, `nn bench`, `nn out`, another `nn
 * info`): while one of them holds the gate a load cannot take it.  Any other
 * owner -- SWAP, NONE with the gate refused (an invariant failure), a value no
 * enum names -- carries no value.  An odd counter carries none either: it is no
 * one state.
 *
 * WHY A PURE FUNCTION IN ITS OWN FILE.  port/nn/nn_svc_grove.c is not built on
 * the host, and none of these holders can be lined up from one console, so the
 * decision is separated from the masked section that gathers its inputs and a
 * host test walks it (test/test_nn_info_hold.c).  The masked section itself is
 * the adapter's.
 *
 * Pure: no lock, no storage, no tx_api.h.
 */
#ifndef NN_INFO_HOLD_H
#define NN_INFO_HOLD_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Where the model section's answer comes from. */
enum nn_info_from {
	/** This call took the gate: every section, copied under it. */
	NN_INFO_FROM_GATE = 0,
	/** A stream holds the gate: identity, `used` and the tensors, copied in
	 *  the section that read the owner. */
	NN_INFO_FROM_STREAM,
	/** Something else holds it: identity, runtime and tensors are BUSY. */
	NN_INFO_FROM_BUSY,
};

/** The decision.  @ref seq is meaningful only when @ref seq_known. */
struct nn_info_hold {
	uint8_t  from;        /**< enum nn_info_from                          */
	uint8_t  seq_known;   /**< the copy belongs to the state @ref seq      */
	uint32_t seq;
};

/**
 * Decide from what the adapter read.
 *
 * @param gate_taken  non-zero when this `nn info` took the gate; @p owner is
 *                    then not consulted
 * @param owner       enum nn_owner, read in the same masked section as @p seq
 *                    when the gate was refused
 * @param seq         the transition counter, read under the gate when it was
 *                    taken, else in that masked section
 */
void nn_info_hold_of(int gate_taken, unsigned owner, uint32_t seq,
                     struct nn_info_hold *out);

#ifdef __cplusplus
}
#endif

#endif /* NN_INFO_HOLD_H */
