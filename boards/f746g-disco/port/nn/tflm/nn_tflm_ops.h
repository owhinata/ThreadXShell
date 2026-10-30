/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    nn_tflm_ops.h
 * @brief   The operator set this board's tflm backend registers -- stated
 *          exactly once (issue #159).
 *
 * [!] THIS FILE EXISTS SO THE BOARD AND THE PC CANNOT DISAGREE.  Two things
 * consume the list: nn_tflm.cc, which registers the operators into its
 * MicroMutableOpResolver, and scripts/verify_tflite.cc, the host-side checker
 * the asset build runs as its model gate.  Written out separately they would
 * drift, and a drifted checker reports "this model will run" about a firmware
 * that never registered the operator.  (wio-lite-ai has the same arrangement
 * with its own, narrower, list.)
 *
 * An X-macro because the two users need different things from each entry: the
 * firmware the resolver METHOD, the host tool the schema ENUM.
 *
 *   X(schema_enum_suffix, resolver_method)
 *     schema_enum_suffix  appended to tflite::BuiltinOperator_
 *     resolver_method     the MicroMutableOpResolver<N>::AddXxx() member
 *
 * The broad common-vision superset this board has always registered (23, in
 * the order nn_tflm.cc registered them before #159): BlazeFace uses eight; the
 * rest let an SD-loaded int8 classifier or detector run without a rebuild.
 * Unregistered kernels are dropped by --gc-sections, so only these cost Flash.
 */
#ifndef NN_TFLM_OPS_H
#define NN_TFLM_OPS_H

#define NN_TFLM_OPS_ALL(X)                                   \
	X(CONV_2D,                 AddConv2D)                \
	X(DEPTHWISE_CONV_2D,       AddDepthwiseConv2D)       \
	X(FULLY_CONNECTED,         AddFullyConnected)        \
	X(AVERAGE_POOL_2D,         AddAveragePool2D)         \
	X(MAX_POOL_2D,             AddMaxPool2D)             \
	X(SOFTMAX,                 AddSoftmax)               \
	X(LOGISTIC,                AddLogistic)              \
	X(RELU,                    AddRelu)                  \
	X(RELU6,                   AddRelu6)                 \
	X(RESHAPE,                 AddReshape)               \
	X(QUANTIZE,                AddQuantize)              \
	X(DEQUANTIZE,              AddDequantize)            \
	X(PAD,                     AddPad)                   \
	X(ADD,                     AddAdd)                   \
	X(MUL,                     AddMul)                   \
	X(SUB,                     AddSub)                   \
	X(MEAN,                    AddMean)                  \
	X(CONCATENATION,           AddConcatenation)         \
	X(RESIZE_BILINEAR,         AddResizeBilinear)        \
	X(RESIZE_NEAREST_NEIGHBOR, AddResizeNearestNeighbor) \
	X(STRIDED_SLICE,           AddStridedSlice)          \
	X(LEAKY_RELU,              AddLeakyRelu)             \
	X(TRANSPOSE,               AddTranspose)

/* Counted from the list itself, so the resolver cannot be sized behind it. */
#define NN_TFLM_OPS_COUNT_ONE(e, m) +1
#define NN_TFLM_OPS_TOTAL (0 NN_TFLM_OPS_ALL(NN_TFLM_OPS_COUNT_ONE))

#endif /* NN_TFLM_OPS_H */
