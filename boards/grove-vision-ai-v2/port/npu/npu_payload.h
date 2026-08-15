/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    npu_payload.h
 * @brief   Ethos-U command-stream payload check (issue #46).
 *
 * Plain C over plain bytes: no TFLite type reaches this, which is what lets the
 * host test compile the real implementation rather than a copy of it.  Finding
 * the payload inside the model is npu_tflm.cc's job; deciding whether it is
 * safe to launch is this one's.
 */
#ifndef NPU_PAYLOAD_H
#define NPU_PAYLOAD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * True when @p data is exactly one COMMAND_STREAM action, as the LAST action.
 *
 * WHY THAT SHAPE AND NOTHING ELSE.  ethosu_invoke_async() walks driver actions
 * in order, and the command-stream action LAUNCHES the NPU in the middle of
 * that walk.  If a later action is malformed the parser jumps to its error exit
 * and returns without calling ethosu_wait() -- so ethosu_inference_end() never
 * runs, the arena is never handed back to the CPU, and TFLM writes it while the
 * NPU may still be running.  The SDK is read-only, so the window cannot be
 * closed there.  It closes here instead: with nothing left to parse after the
 * launch, there is nothing left to fail on.
 *
 * @param data   the payload the driver will be handed -- which is the ethos-u
 *               operator's INPUT TENSOR 0, not its custom_options (that holds
 *               only a CO_TYPE marker)
 * @param bytes  exactly the length the driver will be given
 *
 * Rejects rather than assumes on every malformed case: bad length, wrong
 * fourcc, an unknown action, a length field that runs off the end, or any
 * action at all after the command stream.
 */
bool npu_payload_is_single_final_command_stream(const uint8_t *data,
                                               size_t bytes);

#ifdef __cplusplus
}
#endif

#endif /* NPU_PAYLOAD_H */
