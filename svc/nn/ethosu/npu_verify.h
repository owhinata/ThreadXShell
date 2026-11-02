/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    npu_verify.h
 * @brief   Grove's entry to the bounded FlatBuffer check, shared by the
 *          firmware and the host gate (issue #93; the limits are
 *          nn_tflm_model_check.h's since issue #175).
 *
 * ONE DECLARATION, TWO CALLERS, for the same reason npu_model_scan.h is shared:
 * scripts/verify_vela_model.cc exists to reach the board's verdict on the
 * developer's machine, and it can only do that if it runs the board's check
 * rather than one that looks like it.  The host tool used to construct a
 * flatbuffers::Verifier with the DEFAULT limits -- depth 64, a million tables --
 * so a model that verified there could still be refused by the firmware's
 * tighter ones.  A gate that passes what the device rejects is
 * worse than no gate: it moves the failure to the serial line, after the erase.
 *
 * THE LIMITS ARE NOT DECLARED HERE ANY MORE (issue #175).  The verifier, its
 * limits (depth 16, 4096 tables, nested-flatbuffer check off) and the reason
 * for each are svc/nn/tflm/nn_tflm_model_check.h's, which the tflm backend and
 * wio-lite-ai's and f746g-disco's host gates call too, so every board verifies
 * a model with the same bounds.  What this file keeps is Grove's own: the
 * length floor below, and the one entry point the firmware and the host gate
 * both call.  (This header used to justify depth 16 as a stack bound -- "the
 * default 64 would be ~4.1 KB, the whole stack" -- on the assumption that depth
 * followed the data.  It does not: the schema bounds it at 6; see that header.)
 *
 * MEASURED, THEN GIVEN ROOM.  The classification model this port ships against
 * needs depth 4 and 19 tables -- that is what a Vela model looks like, with the
 * whole graph folded into one `ethos-u` custom operator and only a handful of
 * tensors, buffers and metadata left to verify.  The limits are not those
 * measurements: a model that is legitimate but larger, or one Vela did not
 * fully offload, has to reach the op resolver and be refused THERE with
 * "operator set not supported", rather than be reported here as malformed.
 */
#ifndef NPU_VERIFY_H
#define NPU_VERIFY_H

#include <stddef.h>
#include <stdint.h>

#include "nn_tflm_model_check.h"   /* the bounded verifier and its limits */

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

/**
 * @brief  Does @p len bytes at @p buf hold a flatbuffer that is safe to follow?
 *
 * Nothing here says the model MEANS anything -- that it has one subgraph, that
 * every operator is on Ethos-U, that the arena will hold it.  It says that
 * every offset in the buffer lands inside the buffer, which is what has to be
 * true before any of those questions can be asked at all: tflite::GetModel() is
 * a cast, and the first accessor after it is already following an offset.
 *
 * The custom operator's payload is opaque bytes to the verifier, which
 * npu_model_scan walks itself.
 */
static inline bool npu_verify_model_buffer(const uint8_t *buf, size_t len)
{
	if (buf == nullptr || len < NPU_MODEL_MIN_BYTES)
		return false;

	return nn_tflm_verify_model_buffer(buf, len);
}

#endif /* NPU_VERIFY_H */
