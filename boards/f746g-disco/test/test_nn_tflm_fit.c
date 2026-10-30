/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Host test for port/nn/tflm/nn_tflm_fit.h -- the tflm backend's decision on
 * which model range a staging slot may adopt (issue #159).  The backend
 * (nn_tflm.cc) calls this very header; test_nn_model_container.c's stub
 * backend only restates the contract, so this is where the rule is tested.
 *
 * The boundaries: a range ending exactly at the slot's end, one byte past it,
 * every misalignment up to 15, a pointer at the end or before the base, an
 * empty range, a length that would wrap, and a slot at the top of the address
 * space.
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "nn_tflm_fit.h"

#define CAP   (512u * 1024u)
#define ALIGN 16u

int main(void)
{
	const uintptr_t base = 0xC0600600u;
	uint32_t off;

	/* The slot base, whole slot: the bare model's case. */
	assert(nn_tflm_fit(base, CAP, base, CAP, ALIGN) == 1);
	/* In place: an aligned offset, ending exactly at the slot's end. */
	assert(nn_tflm_fit(base, CAP, base + 4304u, CAP - 4304u, ALIGN) == 1);
	/* One byte past the end. */
	assert(nn_tflm_fit(base, CAP, base + 4304u, CAP - 4304u + 1u, ALIGN) == 0);
	assert(nn_tflm_fit(base, CAP, base, CAP + 1u, ALIGN) == 0);
	/* Every misalignment of 1..15. */
	for (off = 1u; off < ALIGN; off++)
		assert(nn_tflm_fit(base, CAP, base + 4304u + off, 16u, ALIGN) == 0);
	assert(nn_tflm_fit(base, CAP, base + 4304u + ALIGN, 16u, ALIGN) == 1);
	/* Outside: before the base, at the end, beyond it. */
	assert(nn_tflm_fit(base, CAP, base - ALIGN, 16u, ALIGN) == 0);
	assert(nn_tflm_fit(base, CAP, base + CAP, 16u, ALIGN) == 0);
	assert(nn_tflm_fit(base, CAP, base + CAP + ALIGN, 16u, ALIGN) == 0);
	/* The last aligned 16 bytes fit; an empty range is not a model. */
	assert(nn_tflm_fit(base, CAP, base + CAP - 16u, 16u, ALIGN) == 1);
	assert(nn_tflm_fit(base, CAP, base + 4304u, 0u, ALIGN) == 0);
	/* A length that would wrap the sum p + len is still refused. */
	assert(nn_tflm_fit(base, CAP, base + 4304u, 0xFFFFFFF0u, ALIGN) == 0);
	/* A slot at the very top of a 32-bit space: no overflow in the checks. */
	assert(nn_tflm_fit(0xFFF80000u, CAP, 0xFFFFFFF0u, 16u, ALIGN) == 1);
	assert(nn_tflm_fit(0xFFF80000u, CAP, 0xFFFFFFF0u, 17u, ALIGN) == 0);
	/* Which slot a pointer claims. */
	assert(nn_tflm_in_slot(base, CAP, base) == 1);
	assert(nn_tflm_in_slot(base, CAP, base + CAP - 1u) == 1);
	assert(nn_tflm_in_slot(base, CAP, base + CAP) == 0);
	assert(nn_tflm_in_slot(base, CAP, base - 1u) == 0);

	printf("test_nn_tflm_fit: OK\n");
	return 0;
}
