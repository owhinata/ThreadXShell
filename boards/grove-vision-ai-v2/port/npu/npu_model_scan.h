/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    npu_model_scan.h
 * @brief   Locate and vet an Ethos-U model's driver payload (issues #46, #45).
 *
 * C++ ONLY -- it speaks flatbuffer types, which is exactly why it is separate
 * from npu.h.  Two translation units include it:
 *
 *   port/npu/npu_tflm.cc          npu_open() refuses a model that fails this
 *   scripts/verify_vela_model.cc  the host gate, before the model is flashed
 *
 * [!] THE SHARING IS THE POINT, AND IT IS NOT ONLY ABOUT THE WALK.
 * npu_payload.c already gave both sides one implementation of "is this byte
 * sequence a single final command stream".  What they used to have TWO of was
 * the harder half: WHICH BYTES to walk.  That half is where this port has
 * already been wrong once -- the payload is the operator's INPUT TENSOR 0, not
 * its custom_options, and a checker that walked the marker would have agreed
 * with itself perfectly while proving nothing.  A host gate that reimplements
 * the locator can pass a model the board then refuses, or worse, pass one the
 * board accepts for different reasons.
 */
#ifndef NPU_MODEL_SCAN_H
#define NPU_MODEL_SCAN_H

#include <stddef.h>
#include <stdint.h>

#include "tensorflow/lite/schema/schema_generated.h"

/**
 * The bytes the driver will be handed for @p op, or nullptr with @p why set.
 *
 * [!] IT IS INPUT TENSOR 0.  The ethos-u kernel reads custom_initial_data only
 * to check a CO_TYPE marker; what it passes the driver is `cms_data` from input
 * tensor 0's buffer and `cms_data_size` from that tensor's `bytes`
 * (ethosu.cc, Prepare/Eval).
 *
 * `bytes` is TFLM's dims-derived size rather than the buffer length, so both
 * are pinned: a 1-D one-byte-element tensor whose extent equals its constant
 * buffer.  Only then does the returned span cover exactly what the driver
 * parses.
 *
 * A VARIABLE tensor is refused: MicroAllocator::AllocateVariables() overwrites
 * eval_tensors[i].data.data with an arena allocation even when the tensor has a
 * serialized buffer, so Eval() would hand the driver arena contents -- writable
 * at run time by another operator -- while this walk inspected flash.
 *
 * @param why  set to a static description on refusal; untouched on success
 */
const uint8_t *npu_model_command_stream(const tflite::Model *model,
                                        const tflite::SubGraph *sub,
                                        const tflite::Operator *op,
                                        size_t *bytes, const char **why);

/**
 * Why this model must not be opened, or nullptr when it may be.
 *
 * Requires exactly one subgraph, every operator to be the `ethos-u` custom
 * operator, and every one of their payloads to be a single final COMMAND_STREAM
 * (npu_payload.h explains why that shape and no other).
 *
 * Rejecting a non-ethos-u operator is not redundant with the one-operator
 * resolver: AllocateTensors would refuse it too, but this runs first and says
 * which model is wrong rather than which operator is missing.
 */
const char *npu_model_payload_refusal(const tflite::Model *model);

#endif /* NPU_MODEL_SCAN_H */
