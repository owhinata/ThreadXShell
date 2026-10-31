/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    nn_tflm_fit.h
 * @brief   Which staging slot may adopt a model range -- the tflm backend's one
 *          decision about where a model lies, as a pure function (issue #159;
 *          shared by wio-lite-ai and f746g-disco, with wio-lite-ai's live-slot
 *          refusal, since issue #161).
 *
 * svc/nn/tflm/nn_tflm.cc accepts a model pointer by CONTAINMENT, not equality: a
 * plugin container's model section is handed over in place, at an offset inside the
 * slot the container was read into (wio-lite-ai since its issue #108, f746g-disco
 * since #159).  What equality used to guarantee is still required, and this is where
 * it is stated: the whole range lies inside one slot -- TFLM keeps pointers INTO the
 * model for as long as the interpreter lives -- and it starts on the alignment the
 * flatbuffer was laid out against.
 *
 * [!] AND NEVER THE LIVE SLOT.  load_region() hands out the slot the running model is
 * NOT in.  A range inside the live one means the caller used an answer from before
 * some other swap -- and by then it has already written its download over the
 * flatbuffer the interpreter is reading, so the model is gone either way.  Refusing
 * says so, rather than rebuilding from bytes that are half one model and half
 * another (the transactional restore would otherwise "succeed" on corrupt input).
 * The caller-side rule that prevents it is nn.h's: hold the NN session across
 * nn_model_load_region() AND nn_model_reload().
 *
 * [!] ONE COPY, USED BY THE BACKEND AND BY ITS HOST TEST
 * (shell/test/test_nn_tflm_fit.c).  A test that restated the rule in a stub would
 * test the stub.  No tx / HAL / TFLM dependency -- only nn.h's codes -- so the host
 * compiles this very header.  Arithmetic is on differences against the side known to
 * be larger, so nothing overflows near the top of the address space.
 */
#ifndef NN_TFLM_FIT_H
#define NN_TFLM_FIT_H

#include <stdint.h>

#include "nn.h"   /* NN_MODEL_ERR_*: the reasons are the public ones */

/** Whether @p p points into the slot [base, base + cap) at all -- which slot a
 *  range claims, before the rest of the rule decides whether it can hold it. */
static inline int nn_tflm_in_slot(uintptr_t base, uint32_t cap, uintptr_t p)
{
	return p >= base && p - base < (uintptr_t)cap;
}

/**
 * Which of @p n slots, each [bases[i], bases[i] + cap), adopts [p, p + len).
 *
 * @param align  a power of two (NN_MODEL_ALIGN)
 * @param live   the index of the slot the LIVE model is in, or -1 when none is
 * @return the slot's index (>= 0), or why none adopts it, checked in this order:
 *         NN_MODEL_ERR_SLOT   p is in no slot, or the range runs off its end
 *         NN_MODEL_ERR_ALIGN  p is not on @p align
 *         NN_MODEL_ERR_SLOT   it is the live slot
 *         NN_MODEL_ERR_EMPTY  an empty range is not a model
 */
static inline int nn_tflm_pick_slot(const uintptr_t *bases, unsigned n, uint32_t cap,
                                    uintptr_t p, uint32_t len, uint32_t align, int live)
{
	int slot = -1;
	unsigned i;

	for (i = 0u; i < n; i++) {
		if (nn_tflm_in_slot(bases[i], cap, p)) {
			slot = (int)i;
			break;
		}
	}
	if (slot < 0)
		return NN_MODEL_ERR_SLOT;            /* not in any slot */
	if (len > cap - (uint32_t)(p - bases[slot]))
		return NN_MODEL_ERR_SLOT;            /* runs off the end of the slot */
	if ((p & (uintptr_t)(align - 1u)) != 0u)
		return NN_MODEL_ERR_ALIGN;           /* misaligned */
	if (slot == live)
		return NN_MODEL_ERR_SLOT;            /* the live model's slot */
	if (len == 0u)
		return NN_MODEL_ERR_EMPTY;
	return slot;
}

/**
 * The one-slot form of the rule with no live model: 1 if [p, p + len) lies wholly
 * inside [base, base + cap), is not empty and p is aligned, else 0.
 */
static inline int nn_tflm_fit(uintptr_t base, uint32_t cap, uintptr_t p,
                              uint32_t len, uint32_t align)
{
	return nn_tflm_pick_slot(&base, 1u, cap, p, len, align, -1) == 0;
}

#endif /* NN_TFLM_FIT_H */
