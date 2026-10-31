/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Host test for svc/nn/tflm/nn_tflm_fit.h -- the tflm backend's decision on
 * which staging slot may adopt a model range (issue #159; one backend for
 * wio-lite-ai and f746g-disco, with the live-slot refusal, since issue #161).
 * The backend (svc/nn/tflm/nn_tflm.cc) calls this very header; the stub
 * backends of the nn host tests only restate the contract, so this is where the
 * rule is tested.
 *
 * The boundaries of one slot (nn_tflm_fit(), the one-slot form): a range ending
 * exactly at the slot's end, one byte past it, every misalignment up to 15, a
 * pointer at the end or before the base, an empty range, a length that would
 * wrap, and a slot at the top of the address space.  Then the two-slot choice
 * (nn_tflm_pick_slot()): which slot, each refusal's code in its order, and
 * never the live slot.
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "nn.h"
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

	/* Two slots, as the backend has them (issue #161): the slot a range lies in,
	 * by containment -- the base, or an aligned offset inside it. */
	{
		const uintptr_t slots[2] = { base, base + CAP };
		const uintptr_t s1 = slots[1];

		assert(nn_tflm_pick_slot(slots, 2u, CAP, base, CAP, ALIGN, -1) == 0);
		assert(nn_tflm_pick_slot(slots, 2u, CAP, s1, CAP, ALIGN, -1) == 1);
		assert(nn_tflm_pick_slot(slots, 2u, CAP, s1 + 4304u, CAP - 4304u, ALIGN, -1) == 1);
		/* [!] Never the live slot -- base or offset, whatever else is right. */
		assert(nn_tflm_pick_slot(slots, 2u, CAP, base, 16u, ALIGN, 0) == NN_MODEL_ERR_SLOT);
		assert(nn_tflm_pick_slot(slots, 2u, CAP, base + 4304u, 16u, ALIGN, 0) ==
		       NN_MODEL_ERR_SLOT);
		assert(nn_tflm_pick_slot(slots, 2u, CAP, s1, 16u, ALIGN, 1) == NN_MODEL_ERR_SLOT);
		/* ... while the other slot is still adopted. */
		assert(nn_tflm_pick_slot(slots, 2u, CAP, s1, 16u, ALIGN, 0) == 1);
		assert(nn_tflm_pick_slot(slots, 2u, CAP, base, 16u, ALIGN, 1) == 0);
		/* A range running from slot 0 into slot 1 is in no ONE slot. */
		assert(nn_tflm_pick_slot(slots, 2u, CAP, s1 - 16u, 32u, ALIGN, -1) ==
		       NN_MODEL_ERR_SLOT);
		/* Outside both. */
		assert(nn_tflm_pick_slot(slots, 2u, CAP, base - ALIGN, 16u, ALIGN, -1) ==
		       NN_MODEL_ERR_SLOT);
		assert(nn_tflm_pick_slot(slots, 2u, CAP, s1 + CAP, 16u, ALIGN, -1) ==
		       NN_MODEL_ERR_SLOT);
		/* Each reason has its own code, checked in the documented order:
		 * off the end before alignment, alignment before the live slot, the
		 * live slot before the empty range. */
		assert(nn_tflm_pick_slot(slots, 2u, CAP, base + 1u, 16u, ALIGN, -1) ==
		       NN_MODEL_ERR_ALIGN);
		assert(nn_tflm_pick_slot(slots, 2u, CAP, base + 1u, CAP, ALIGN, -1) ==
		       NN_MODEL_ERR_SLOT);
		assert(nn_tflm_pick_slot(slots, 2u, CAP, base + 1u, 16u, ALIGN, 0) ==
		       NN_MODEL_ERR_ALIGN);
		assert(nn_tflm_pick_slot(slots, 2u, CAP, base, 0u, ALIGN, 0) == NN_MODEL_ERR_SLOT);
		assert(nn_tflm_pick_slot(slots, 2u, CAP, base, 0u, ALIGN, 1) == NN_MODEL_ERR_EMPTY);
		assert(nn_tflm_pick_slot(slots, 2u, CAP, base, 0u, ALIGN, -1) == NN_MODEL_ERR_EMPTY);
	}

	printf("test_nn_tflm_fit: OK\n");
	return 0;
}
