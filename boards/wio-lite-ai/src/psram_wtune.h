/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    psram_wtune.h
 * @brief   The write-latency sweep of `psram wtune` without the hardware
 *          (issue #252), so the host tests reach every way it ends.
 *
 * src/psram.c's psram_wtune() hands the sweep its four hardware steps and
 * returns what psram_wtune_sweep() returns.  Everything that decides the
 * result is here: the refusal (not up, or an offset outside the
 * non-cacheable pool, psram_wtune_off_ok()), the per-dummy marker compare,
 * a reset that does not bring the device back (PSRAM_WTUNE_DEAD), and which
 * write dummy is left applied.  No HAL, no registers.
 */
#ifndef PSRAM_WTUNE_H
#define PSRAM_WTUNE_H

#include <stdint.h>

#include "psram.h"   /* PSRAM_WTUNE_*, psram_wtune_off_ok() */

struct psram_wtune_hw {
	/** Enter memory-mapped mode with this write dummy (the read one kept). */
	void (*mmap_enter)(uint32_t wr_dcyc);
	/** Write the 8-byte marker at the window offset through the window, then
	 *  drain it and leave memory-mapped mode for the read-back. */
	void (*write_marker)(uint32_t off, uint32_t m0, uint32_t m1);
	/** Read 8 bytes back indirectly; 0 on a timeout. */
	int  (*read_back)(uint32_t off, uint8_t back[8]);
	/** Check the device answers, with a Global Reset if not; 1 when it does. */
	int  (*recover)(void);
};

/**
 * The sweep (see psram_wtune() in psram.h for the results).  @p up: the
 * PSRAM is in memory-mapped mode; @p orig: the write dummy in use, re-entered
 * when none passes or the device dies; @p nonce: folded into every marker.
 * On PSRAM_WTUNE_REFUSED no step of @p hw is called.
 */
int psram_wtune_sweep(const struct psram_wtune_hw *hw, int up, uint32_t orig,
                      uint32_t nonce, uint32_t off, uint32_t reps,
                      uint32_t *out_mask);

#endif /* PSRAM_WTUNE_H */
