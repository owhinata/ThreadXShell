/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    nn_tflm_fit.h
 * @brief   Whether a model range may be adopted from one staging slot -- the
 *          tflm backend's one decision, as a pure function (issue #159).
 *
 * nn_tflm.cc accepts a model pointer by CONTAINMENT, not equality: a plugin
 * container's model section is handed over in place, at an offset inside the
 * slot the container was read into.  What equality used to guarantee is still
 * required, and this is where it is stated: the whole range lies inside the
 * slot -- TFLM keeps pointers INTO the model for as long as the interpreter
 * lives -- and it starts on the alignment the flatbuffer was laid out against.
 *
 * [!] ONE COPY, USED BY THE BACKEND AND BY ITS HOST TEST
 * (test/test_nn_tflm_fit.c).  A test that restated the rule in a stub would
 * test the stub.  No tx / HAL / TFLM dependency, so the host compiles this very
 * header.  Arithmetic is on differences against the side known to be larger,
 * so nothing overflows near the top of the address space.
 */
#ifndef NN_TFLM_FIT_H
#define NN_TFLM_FIT_H

#include <stdint.h>

/**
 * @param base, cap  the slot: [base, base + cap)
 * @param p, len     the model range the caller wants adopted
 * @param align      a power of two (NN_MODEL_ALIGN)
 * @return 1 if [p, p + len) lies wholly inside the slot and p is aligned,
 *         else 0 (an empty range is not a model: refused)
 */
static inline int nn_tflm_fit(uintptr_t base, uint32_t cap, uintptr_t p,
                              uint32_t len, uint32_t align)
{
	if (p < base || p - base >= (uintptr_t)cap)
		return 0;                     /* not in this slot */
	if (len == 0u || len > cap - (uint32_t)(p - base))
		return 0;                     /* runs off the end of the slot */
	if ((p & (uintptr_t)(align - 1u)) != 0u)
		return 0;                     /* misaligned */
	return 1;
}

/** Whether @p p points into the slot at all -- which slot a range claims,
 *  before nn_tflm_fit() decides whether that slot can hold it. */
static inline int nn_tflm_in_slot(uintptr_t base, uint32_t cap, uintptr_t p)
{
	return p >= base && p - base < (uintptr_t)cap;
}

#endif /* NN_TFLM_FIT_H */
