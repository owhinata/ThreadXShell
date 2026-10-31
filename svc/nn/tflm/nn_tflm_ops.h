/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    nn_tflm_ops.h
 * @brief   The operator set the tflm backend registers -- stated exactly once
 * (owhinata/wio-lite-ai#9 P2c; one header for wio-lite-ai and f746g-disco since
 * issue #161).
 *
 * [!] THIS FILE EXISTS SO THE BOARD AND THE PC CANNOT DISAGREE.  Two things consume the
 * list: svc/nn/tflm/nn_tflm.cc, which registers the operators into the
 * MicroMutableOpResolver, and each board's scripts/verify_tflite.cc, the host-side
 * checker that tells you BEFORE a transfer whether a model stays inside that set.  If
 * those two lists were written out separately they would drift, and a drifted checker
 * is worse than no checker: it reports "this model will run" about a firmware that
 * never registered the operator.  Both are built with the SAME profile define (each
 * board's cmake/tflite-micro.cmake passes it to the firmware library and to the host
 * compile alike).
 *
 * An X-macro rather than a table because the two users need different things from each
 * entry -- the firmware needs the resolver METHOD, the host tool needs the schema
 * ENUM -- and a macro is the only way to give each what it needs from one definition.
 *
 * Each entry is X(schema_enum_suffix, resolver_method):
 *   schema_enum_suffix  appended to tflite::BuiltinOperator_ (schema_generated.h)
 *   resolver_method     the MicroMutableOpResolver<N>::AddXxx() member
 *
 * Adding an operator here is not free: it is compiled kernel code in the board's
 * flash.  Which profile a board builds, and what each one measured there, is the
 * board's (its cmake/tflite-micro.cmake and README): wio-lite-ai defaults to
 * `blazeface` and cannot link `extended`; f746g-disco builds `extended`.
 */
#ifndef NN_TFLM_OPS_H
#define NN_TFLM_OPS_H

/*
 * The eight operators BlazeFace-front 128 int8 actually uses.
 */
#define NN_TFLM_OPS_BASE(X)             \
	X(QUANTIZE,           AddQuantize)          \
	X(CONV_2D,            AddConv2D)            \
	X(DEPTHWISE_CONV_2D,  AddDepthwiseConv2D)   \
	X(ADD,                AddAdd)               \
	X(PAD,                AddPad)               \
	X(MAX_POOL_2D,        AddMaxPool2D)         \
	X(RESHAPE,            AddReshape)           \
	X(DEQUANTIZE,         AddDequantize)

/*
 * Exactly what MLPerf Tiny v1.4 needs on top of the base set (owhinata/wio-lite-ai#55).
 *
 * [!] A MEASURED SET, NOT A GENEROUS ONE.  All five v1.4 models -- ic01 (ResNet), ic02
 * (larger ResNet), kws01 (DS-CNN), vww01 (MobileNet) and ad01 (deep autoencoder) --
 * were run through scripts/verify_tflite.cc against the base set, and the union of
 * everything they were missing is these three.
 *
 * [!] "ONE MORE OPERATOR" IS NOT A UNIT OF FLASH, so do not budget by counting them.
 * Adding MEAN and LOGISTIC to this profile was measured at ZERO bytes on wio-lite-ai:
 * their kernels are already in the image because TFLM's shared operator code reaches
 * them and --gc-sections cannot drop them.  What costs is a kernel nothing else
 * already pulled in.  Measure the profile you actually intend to ship.
 */
#define NN_TFLM_OPS_MLPERF(X)                   \
	X(FULLY_CONNECTED,    AddFullyConnected)    \
	X(AVERAGE_POOL_2D,    AddAveragePool2D)     \
	X(SOFTMAX,            AddSoftmax)

/*
 * The common-vision superset on top of `mlperf`, so a different int8 model can be
 * loaded without rebuilding the firmware.  base + mlperf + this = 23 operators, the
 * set f746g-disco has registered since its owhinata/stm32f746g-disco#89.
 */
#define NN_TFLM_OPS_EXTRA(X)                            \
	X(LOGISTIC,               AddLogistic)               \
	X(RELU,                   AddRelu)                   \
	X(RELU6,                  AddRelu6)                  \
	X(MUL,                    AddMul)                    \
	X(SUB,                    AddSub)                    \
	X(MEAN,                   AddMean)                   \
	X(CONCATENATION,          AddConcatenation)          \
	X(RESIZE_BILINEAR,        AddResizeBilinear)         \
	X(RESIZE_NEAREST_NEIGHBOR, AddResizeNearestNeighbor) \
	X(STRIDED_SLICE,          AddStridedSlice)           \
	X(LEAKY_RELU,             AddLeakyRelu)              \
	X(TRANSPOSE,              AddTranspose)

/*
 * THE PROFILE: exactly one complete list, NN_TFLM_OPS_ALL(X), chosen by one define.
 *
 *   (none)                         blazeface   base                      =  8
 *   NN_TFLM_OPS_MLPERF_PROFILE=1   mlperf      base + mlperf             = 11
 *   NN_TFLM_OPS_EXTENDED=1         extended    base + mlperf + extra     = 23
 *
 * The three existing profiles happen to nest, which is why NN_TFLM_OPS_EXTRA does NOT
 * repeat the MLPerf operators (registering one twice is a resolver error, and this is
 * the only place that could introduce one).  Nesting is not a property of the
 * mechanism, though: each branch below states its WHOLE list.  A profile that shares
 * nothing with the CPU kernels -- an Ethos-U one, whose model is a single custom
 * operator the NPU runs (X(CUSTOM, AddEthosU)) with no CPU kernel registered at all --
 * is one more branch with its own list and its own define; the resolver's size below
 * follows from whatever the branch lists.
 */
#if defined(NN_TFLM_OPS_EXTENDED) && defined(NN_TFLM_OPS_MLPERF_PROFILE)
#error "more than one NN_TFLM_OPS profile is defined"
#endif

#if defined(NN_TFLM_OPS_EXTENDED)
#define NN_TFLM_OPS_ALL(X)  NN_TFLM_OPS_BASE(X) NN_TFLM_OPS_MLPERF(X) NN_TFLM_OPS_EXTRA(X)
#elif defined(NN_TFLM_OPS_MLPERF_PROFILE)
#define NN_TFLM_OPS_ALL(X)  NN_TFLM_OPS_BASE(X) NN_TFLM_OPS_MLPERF(X)
#else
#define NN_TFLM_OPS_ALL(X)  NN_TFLM_OPS_BASE(X)
#endif

/* The resolver's size, counted from the selected list itself so that
 * MicroMutableOpResolver<N> cannot fall behind it: a compile-time constant
 * (svc/nn/tflm/nn_tflm_state.h sizes the resolver's storage with it). */
#define NN_TFLM_OPS_COUNT_ONE(e, m) +1
#define NN_TFLM_OPS_TOTAL   (0 NN_TFLM_OPS_ALL(NN_TFLM_OPS_COUNT_ONE))

#endif /* NN_TFLM_OPS_H */
