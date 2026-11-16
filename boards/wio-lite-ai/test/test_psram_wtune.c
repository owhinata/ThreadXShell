/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Issue #252 -- the sweep of `psram wtune` (src/psram_wtune.c), compiled
 * unchanged over a fake device: a 6 MB array the marker is written to and
 * read back from, a per-dummy "the write lands" table, a read-back that can
 * time out and a reset that can fail at a chosen dummy.
 *
 * What it asks:
 *   - refused (not up; an offset that is not a multiple of 4 or whose 8 bytes
 *     leave the non-cacheable pool): PSRAM_WTUNE_REFUSED, mask 0, and no step
 *     of the hardware called;
 *   - every dummy in turn, 4 markers each; the lowest that round-tripped is
 *     left applied, the original one when none did (and the result is OK with
 *     mask 0 -- the command decides what that means);
 *   - a read-back that times out fails only that dummy;
 *   - [!] a reset that does not bring the device back ends DEAD at once, with
 *     what passed before, and re-enters the original dummy -- never OK.
 */
#include "psram_wtune.h"

#include <stdio.h>
#include <string.h>

static uint8_t dev[PSRAM_NC_BYTES];
static uint32_t lands;           /* bit d: a write at dummy d reaches the device */
static uint32_t cur_d, last_enter;
static int calls, enters, recovers, dead_at = -1, timeout_at = -1;
static int bad_off;

static void f_enter(uint32_t d) { calls++; enters++; cur_d = d; last_enter = d; }
static void f_write(uint32_t off, uint32_t m0, uint32_t m1)
{
	calls++;
	if (off > PSRAM_NC_BYTES - 8u) {
		bad_off++;
		return;
	}
	if (lands & (1u << cur_d)) {
		memcpy(dev + off, &m0, 4);
		memcpy(dev + off + 4, &m1, 4);
	}
}
static int f_read(uint32_t off, uint8_t back[8])
{
	calls++;
	if ((int)cur_d == timeout_at)
		return 0;
	if (off > PSRAM_NC_BYTES - 8u) {
		bad_off++;
		return 0;
	}
	memcpy(back, dev + off, 8);
	return 1;
}
static int f_recover(void)
{
	calls++;
	recovers++;
	return (int)cur_d != dead_at;
}

static const struct psram_wtune_hw hw = { f_enter, f_write, f_read, f_recover };

static int failures;

static void check(int ok, const char *what)
{
	if (!ok) {
		printf("FAIL: %s (calls %d enters %d recovers %d last %lu)\n", what, calls,
		       enters, recovers, (unsigned long)last_enter);
		failures++;
	}
}

static void reset(void)
{
	memset(dev, 0, sizeof dev);
	lands = 0xFFFFu;
	cur_d = last_enter = 99u;
	calls = enters = recovers = bad_off = 0;
	dead_at = timeout_at = -1;
}

int main(void)
{
	static const uint32_t refused[] = { 0x5FFFFCu, 0x600000u, 0x7FFFF8u, 0x202u,
	                                    0x201u, 0xFFFFFFF8u };
	uint32_t mask;
	int rc;

	for (unsigned i = 0; i < sizeof refused / sizeof refused[0]; i++) {
		reset();
		mask = 7u;
		rc = psram_wtune_sweep(&hw, 1, 4u, 0u, refused[i], 4u, &mask);
		check(rc == PSRAM_WTUNE_REFUSED && mask == 0u && calls == 0,
		      "an offset outside the pool: refused, nothing called");
	}
	reset();
	rc = psram_wtune_sweep(&hw, 0, 4u, 0u, 0x200u, 4u, &mask);
	check(rc == PSRAM_WTUNE_REFUSED && calls == 0, "not up: refused, nothing called");

	reset();
	rc = psram_wtune_sweep(&hw, 1, 4u, 0x4D0000u, 0x5FFFF8u, 4u, &mask);
	check(rc == PSRAM_WTUNE_OK && mask == 0xFFFFu && last_enter == 0u &&
	      recovers == 16 && bad_off == 0,
	      "all land: OK, every dummy, the lowest applied, the last word in the pool");

	reset();
	lands = 0xFFF0u;                         /* 0..3 do not land */
	rc = psram_wtune_sweep(&hw, 1, 4u, 1u, 0x200u, 4u, &mask);
	check(rc == PSRAM_WTUNE_OK && mask == 0xFFF0u && last_enter == 4u,
	      "the lowest that round-tripped is left applied");

	reset();
	lands = 0u;
	rc = psram_wtune_sweep(&hw, 1, 5u, 2u, 0x200u, 4u, &mask);
	check(rc == PSRAM_WTUNE_OK && mask == 0u && last_enter == 5u,
	      "none round-tripped: OK with mask 0, the original re-entered");

	reset();
	timeout_at = 6;
	rc = psram_wtune_sweep(&hw, 1, 4u, 3u, 0x200u, 4u, &mask);
	check(rc == PSRAM_WTUNE_OK && mask == (0xFFFFu & ~(1u << 6)),
	      "a read-back timeout fails only that dummy");

	reset();
	lands = 0xFFF0u;
	dead_at = 7;
	rc = psram_wtune_sweep(&hw, 1, 9u, 4u, 0x200u, 4u, &mask);
	check(rc == PSRAM_WTUNE_DEAD && mask == 0x0070u && last_enter == 9u &&
	      recovers == 8, "the device dead: DEAD at once, what passed, the original back");

	reset();
	dead_at = 0;
	rc = psram_wtune_sweep(&hw, 1, 4u, 5u, 0x200u, 4u, &mask);
	check(rc == PSRAM_WTUNE_DEAD && mask == 0u && last_enter == 4u,
	      "dead at the first dummy: DEAD with nothing passed");

	printf("test_psram_wtune: %s\n", failures ? "FAILED" : "OK");
	return failures ? 1 : 0;
}
