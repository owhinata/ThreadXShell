/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    npu_model_scan.h
 * @brief   Locate and vet an Ethos-U model's driver payload (issues #46, #45).
 *
 * C++ ONLY -- it speaks flatbuffer types, which is exactly why it is separate
 * from the C headers an adapter includes.  Two translation units include it:
 *
 *   svc/nn/ethosu/nn_ethosu.cc
 *       the backend's reload() refuses a model that fails this
 *   boards/grove-vision-ai-v2/scripts/verify_vela_model.cc
 *       the host gate, before the model is flashed
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
 *
 * [!] THE PAYLOADS ADD UP TO AT MOST @p model_bytes (issue #179).  Each
 * operator entry's payload is charged against the model's declared length
 * BEFORE it is walked, and the first one that would take the sum past it is
 * refused.  Operator entries may name one Operator table and tensors may name
 * one Buffer table -- the verifier accepts both -- so without the charge a
 * file within its 4,096 visits could have the walk read one large stream
 * thousands of times.  The bound is structural, not measured: an honest
 * model's payloads are disjoint runs of its own bytes.  So the whole scan reads
 * at most model_bytes / 4 payload words, and the driver's parse of every
 * payload at each Invoke() is bounded the same way.
 *
 * [!] AND NO INPUT AFTER 0 IS -1.  The ethos-u kernel's Eval() passes inputs
 * 1..n to GetEvalTensor() unchecked; the shared graph walk lets -1 through
 * (TFLM's optional tensor), so it is refused here.  The list's length is judged
 * first (NN_TFLM_MAX_OP_TENSORS, the walk's limit), because the firmware runs
 * this before that walk.
 *
 * @param model_bytes  the length the model was verified with (the declared
 *                     length, never "the rest of the window")
 */
const char *npu_model_payload_refusal(const tflite::Model *model,
                                      size_t model_bytes);

/**
 * Why a command stream of this model must not run, or nullptr when none
 * reaches outside its tensors (issue #184).
 *
 * For every ethos-u operator entry: the stream inside its payload
 * (npu_payload.h reports where), and one region per base pointer the kernel
 * hands the driver -- inputs 1.. then outputs, cut at 8, the kernel's order
 * (ethosu.cc Eval()) -- each the byte size of its tensor (the shape's elements
 * times the element size, INT4 half a byte rounded up: the constant's length
 * and at most the arena allocation) and writable only when the tensor has no
 * data.  npu_cmdstream_check() then decides; the sentence is
 * npu_cmdstream_refusal_text()'s, the same in the firmware and the host gate.
 *
 * [!] RUN IT AFTER npu_model_payload_refusal() AND THE SHARED WALK, both
 * accepting (nn_ethosu.cc's order; the host gate keeps it in both of its
 * orders).  The scan proves each payload is one final stream, charged against
 * the model's length (so this reads at most model_bytes / 4 words again); the
 * walk (nn_tflm_model_io_check()) proves every tensor index and buffer index
 * in range, each tensor's size an int and a constant's data exactly its
 * shape's bytes, which is what makes the sizes read here the ones TFLM uses.
 * What it reads it still bounds itself, so a misuse refuses rather than reads
 * out of range.
 *
 * @param shram_bytes  the NPU's SHRAM for the configuration the board compiles
 *                     models for (grove-vision-ai-v2: npu_hw.h's
 *                     NPU_SHRAM_BYTES)
 */
const char *npu_model_cmdstream_refusal(const tflite::Model *model,
                                        uint32_t shram_bytes);

#endif /* NPU_MODEL_SCAN_H */
