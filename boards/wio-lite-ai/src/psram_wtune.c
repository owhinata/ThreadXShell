/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    psram_wtune.c
 * @brief   The write-latency sweep of `psram wtune`, its hardware steps passed
 *          in (issue #252; moved out of psram.c unchanged otherwise).
 *
 * For each write dummy 0..15, @p reps times: enter memory-mapped mode with
 * it, write the marker through the window, read it back indirectly and
 * compare; then check the device still answers (a reset if not) -- a dummy
 * that wedged it must not poison the rest.  If it stays dead, the original
 * dummy is re-entered and the sweep ends DEAD with what passed so far, so
 * the caller does not take "16 fails" for a result.  Otherwise the lowest
 * passing dummy is left applied (the original one if none passed).
 */
#include "psram_wtune.h"

int psram_wtune_sweep(const struct psram_wtune_hw *hw, int up, uint32_t orig,
                      uint32_t nonce, uint32_t off, uint32_t reps,
                      uint32_t *out_mask)
{
	uint32_t d, r, i, mask = 0u, first = 0xFFFFFFFFu;

	*out_mask = 0u;
	/* [!] Never past the non-cacheable pool, whatever the caller. */
	if (!up || !psram_wtune_off_ok(off))
		return PSRAM_WTUNE_REFUSED;

	for (d = 0u; d <= 15u; d++) {
		uint32_t ok = 1u;

		for (r = 0u; r < reps && ok; r++) {
			uint32_t m0 = (0x5AC30000u | (d << 8) | r) ^ nonce;
			uint32_t m1 = ~m0;
			uint8_t back[8];
			uint32_t g0 = 0u, g1 = 0u;

			hw->mmap_enter(d);
			hw->write_marker(off, m0, m1);
			if (!hw->read_back(off, back)) {
				ok = 0u;                /* timed out; abort already done */
				continue;
			}
			for (i = 0u; i < 4u; i++) {
				g0 |= (uint32_t)back[i] << (8u * i);
				g1 |= (uint32_t)back[4u + i] << (8u * i);
			}
			if (g0 != m0 || g1 != m1)
				ok = 0u;
		}
		if (!hw->recover()) {
			hw->mmap_enter(orig);
			*out_mask = mask;
			return PSRAM_WTUNE_DEAD;
		}
		if (ok) {
			mask |= 1u << d;
			if (first == 0xFFFFFFFFu)
				first = d;
		}
	}
	hw->mmap_enter((first != 0xFFFFFFFFu) ? first : orig);
	*out_mask = mask;
	return PSRAM_WTUNE_OK;
}
