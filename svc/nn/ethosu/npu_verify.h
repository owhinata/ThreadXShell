/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    npu_verify.h
 * @brief   The bounded FlatBuffer check, shared by the firmware and the host
 *          gate (issue #93).
 *
 * ONE DECLARATION, TWO CALLERS, for the same reason npu_model_scan.h is shared:
 * scripts/verify_vela_model.cc exists to reach the board's verdict on the
 * developer's machine, and it can only do that if it runs the board's check
 * rather than one that looks like it.  The host tool used to construct a
 * flatbuffers::Verifier with the DEFAULT limits -- depth 64, a million tables --
 * so a model that verified there could still be refused by the firmware, which
 * cannot afford those numbers.  A gate that passes what the device rejects is
 * worse than no gate: it moves the failure to the serial line, after the erase.
 *
 * WHY THE LIMITS ARE NOT THE DEFAULTS.  The generated table verifiers RECURSE,
 * so max_depth is a bound on stack, and the firmware runs this on a shell
 * thread whose stack is CLI_INSTANCE_STACK_SIZE (4,096 B) -- not on a stack
 * sized for a parser.  -fstack-usage over the firmware's translation unit puts
 * every frame in the recursive cycle at 24 B or less, so a level of nesting
 * costs about 64 B: 16 levels is ~1.0 KB and the default 64 would be ~4.1 KB,
 * which is the whole stack.  max_tables is a bound on WORK, and a million is
 * not a bound on anything a malformed buffer could ask for here.
 *
 * MEASURED, THEN GIVEN ROOM.  The classification model this port ships against
 * needs depth 4 and 19 tables -- that is what a Vela model looks like, with the
 * whole graph folded into one `ethos-u` custom operator and only a handful of
 * tensors, buffers and metadata left to verify.  The constants below are not
 * those measurements: a model that is legitimate but larger, or one Vela did
 * not fully offload, has to reach the op resolver and be refused THERE with
 * "operator set not supported", rather than be reported here as malformed.
 * Reporting the wrong reason is the failure this file exists to avoid, and the
 * table count costs nothing but a counter.  Both are still orders of magnitude
 * under the defaults.
 */
#ifndef NPU_VERIFY_H
#define NPU_VERIFY_H

#include <stddef.h>
#include <stdint.h>

#include "flatbuffers/verifier.h"
#include "tensorflow/lite/schema/schema_generated.h"

/**
 * Smallest length worth inspecting: the root offset, plus the 4-byte file
 * identifier that sits at offset 4.
 *
 * [!] IT IS A FLOOR ON THE DECLARED LENGTH and not only on the room behind an
 * address.  The identifier is read from `buf + 4`, so a caller that declared
 * fewer bytes than this would have the identifier check reading outside the
 * very bounds it declared -- before the verifier, the thing that would
 * otherwise catch it, has been constructed.
 */
#define NPU_MODEL_MIN_BYTES   16u

/** Nesting levels, and tables verified.  See the file header. */
#define NPU_VERIFY_MAX_DEPTH   16u    /* measured 4  */
#define NPU_VERIFY_MAX_TABLES  4096u  /* measured 19 */

/**
 * @brief  Does @p len bytes at @p buf hold a flatbuffer that is safe to follow?
 *
 * Nothing here says the model MEANS anything -- that it has one subgraph, that
 * every operator is on Ethos-U, that the arena will hold it.  It says that
 * every offset in the buffer lands inside the buffer, which is what has to be
 * true before any of those questions can be asked at all: tflite::GetModel() is
 * a cast, and the first accessor after it is already following an offset.
 *
 * check_nested_flatbuffers stays off: a nested buffer would be verified as a
 * buffer in its own right, and nothing in a Vela model has one.  The custom
 * operator's payload is opaque bytes, which npu_model_scan walks itself.
 */
static inline bool npu_verify_model_buffer(const uint8_t *buf, size_t len)
{
	flatbuffers::Verifier::Options opts;

	if (buf == nullptr || len < NPU_MODEL_MIN_BYTES)
		return false;

	opts.max_depth                = NPU_VERIFY_MAX_DEPTH;
	opts.max_tables               = NPU_VERIFY_MAX_TABLES;
	opts.check_nested_flatbuffers = false;

	flatbuffers::Verifier v(buf, len, opts);

	return tflite::VerifyModelBuffer(v);
}

#endif /* NPU_VERIFY_H */
