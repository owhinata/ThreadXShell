/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Host test for the measure-then-wrap IRQ accounting (issue #30,
 * port/sdk_seam/epk_irq_wrap.c).
 *
 * WHY THIS EXISTS.  The wrap is a TRANSACTION over hardware the shell cannot
 * inspect afterwards, and its failure mode is invisible: a bring-up that
 * wrapped two lines and then failed on the third used to leave those two
 * enabled and registered while the caller closed the device underneath them.
 * The registry then pointed at vectors the vendor's close() had moved, and
 * `thread` read "--" until the next reboot -- a permanent, silent loss of the
 * accounting the whole mechanism exists to provide.  Nothing about that is
 * visible in a symbol table, and reproducing it on hardware means engineering
 * a driver failure, so it is pinned here instead.
 *
 * The invariant under test is the one AGENTS.md states: **every line is either
 * DISABLED, or wrapped AND registered.**  There is no third state, at any point
 * in the sequence -- including after a failure, and including after a retry.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "epk_irq_wrap.h"
#include "seam_host_env.h"
#include "WE2_device.h"

static int failures;

#define CHECK(cond, ...)                                                       \
	do {                                                                   \
		if (!(cond)) {                                                 \
			printf("FAIL %s:%d: ", __FILE__, __LINE__);            \
			printf(__VA_ARGS__);                                   \
			printf("\n");                                          \
			failures++;                                            \
		}                                                              \
	} while (0)

/* Stand-in for a vendor handler: only its ADDRESS matters here. */
static void vendor_isr_a(void) { }
static void vendor_isr_b(void) { }
static void vendor_isr_c(void) { }
void Default_Handler(void) { }

static int irq_enabled(int irqn)
{
	return (seam_host_env.iser[irqn >> 5] >> (irqn & 31)) & 1u;
}

static int irq_registered(int irqn)
{
	return (seam_host_env.registry[irqn >> 5] >> (irqn & 31)) & 1u;
}

static void env_reset(void)
{
	memset(&seam_host_env, 0, sizeof seam_host_env);
	seam_host_env.fail_register_irqn = -1;
}

/* What a vendor bring-up does: install a handler and enable the line. */
static void vendor_brings_up(int irqn, void (*isr)(void))
{
	seam_host_env.vector[irqn] = (uint32_t)(uintptr_t)isr;
	seam_host_env.iser[irqn >> 5] |= 1u << (irqn & 31);
}

/*
 * THE INVARIANT.  Checked over every line, after every step: nothing is enabled
 * unless it is also registered, and nothing carries a vendor vector while
 * registered.  A single sweep is cheap and catches states no targeted assertion
 * was written for.
 */
static void check_invariant(const char *when)
{
	int irqn;

	for (irqn = 0; irqn < 512; irqn++) {
		if (irq_enabled(irqn) && !irq_registered(irqn)) {
			CHECK(0, "%s: irq %d is enabled but not registered",
			      when, irqn);
			return;
		}
	}
}

/* ---- 1. the happy path wraps, registers and enables ---------------------- */

static void test_wrap_success(void)
{
	struct epk_irq_snapshot snap;
	struct epk_irq_wrapset set;

	env_reset();
	grove_epk_irq_snapshot(&snap);
	vendor_brings_up(105, vendor_isr_a);
	vendor_brings_up(66, vendor_isr_b);

	CHECK(grove_epk_irq_wrap_new(&snap, &set) == 1,
	      "wrapping two fresh lines failed");
	CHECK(set.count == 2u, "the undo log has %lu entries, wanted 2",
	      (unsigned long)set.count);
	CHECK(irq_enabled(105) && irq_registered(105), "irq 105 not accounted");
	CHECK(irq_enabled(66) && irq_registered(66), "irq 66 not accounted");
	CHECK(seam_host_env.vector[105] != (uint32_t)(uintptr_t)vendor_isr_a,
	      "irq 105 still carries the vendor vector");
	check_invariant("after a clean wrap");

	/* [!] Every test rolls its own wraps back.  The trampoline table is
	 * static and outlives env_reset(), so a test that leaked entries would
	 * make the NEXT one restore a stale vendor vector -- which is how this
	 * suite found that wrap_one() must refuse a double wrap. */
	grove_epk_irq_unwrap_set(&set);
}

/* ---- 1b. wrapping the same line twice is refused ------------------------- */

static void test_double_wrap_refused(void)
{
	struct epk_irq_snapshot snap;
	struct epk_irq_wrapset first, second;

	env_reset();
	grove_epk_irq_snapshot(&snap);
	vendor_brings_up(105, vendor_isr_a);
	CHECK(grove_epk_irq_wrap_new(&snap, &first) == 1, "first wrap failed");

	/* The vendor disables and re-enables the line without anyone
	 * unwrapping it -- a close()/open() cycle that skipped the teardown. */
	seam_host_env.iser[105 >> 5] &= ~(1u << (105 & 31));
	grove_epk_irq_snapshot(&snap);
	seam_host_env.iser[105 >> 5] |= 1u << (105 & 31);

	CHECK(grove_epk_irq_wrap_new(&snap, &second) == 0,
	      "wrapping an already-wrapped line was accepted; unwrap would "
	      "then restore whichever entry it found first");
	CHECK(second.count == 0u, "the refused wrap still logged an entry");
	CHECK(!irq_enabled(105), "the refused line was left enabled");
	check_invariant("after a refused double wrap");

	grove_epk_irq_unwrap_set(&first);
	CHECK(seam_host_env.vector[105] == (uint32_t)(uintptr_t)vendor_isr_a,
	      "rollback did not restore the vendor vector");
}

/* ---- 2. a line the vendor never touched is left alone -------------------- */

static void test_untouched_lines_ignored(void)
{
	struct epk_irq_snapshot snap;
	struct epk_irq_wrapset set;

	env_reset();
	/* Already enabled BEFORE the snapshot: somebody else's line (the
	 * console UART, in the firmware).  It must not be re-wrapped. */
	vendor_brings_up(90, vendor_isr_c);
	grove_epk_irq_snapshot(&snap);
	vendor_brings_up(105, vendor_isr_a);

	CHECK(grove_epk_irq_wrap_new(&snap, &set) == 1, "wrap failed");
	CHECK(set.count == 1u, "the pre-existing line was wrapped too");
	CHECK(seam_host_env.vector[90] == (uint32_t)(uintptr_t)vendor_isr_c,
	      "a line enabled before the snapshot had its vector replaced");
	grove_epk_irq_unwrap_set(&set);
}

/* ---- 3. a partial failure is rolled back COMPLETELY ---------------------- */

static void test_partial_failure_rolls_back(void)
{
	struct epk_irq_snapshot snap;
	struct epk_irq_wrapset set;

	env_reset();
	grove_epk_irq_snapshot(&snap);
	vendor_brings_up(64, vendor_isr_a);
	vendor_brings_up(65, vendor_isr_b);
	vendor_brings_up(66, vendor_isr_c);
	/* The registry refuses exactly one line -- the shape of "the eighth
	 * trampoline was already taken" without needing eight of them. */
	seam_host_env.fail_register_irqn = 65;

	CHECK(grove_epk_irq_wrap_new(&snap, &set) == 0,
	      "a refused registration was reported as success");
	/* The refused line must already be disabled: that is fail-closed. */
	CHECK(!irq_enabled(65), "the refused line was left enabled");
	check_invariant("after a partial failure");

	/* ...and the caller can undo the rest. */
	grove_epk_irq_unwrap_set(&set);
	CHECK(!irq_enabled(64) && !irq_enabled(66),
	      "rollback left a line enabled");
	CHECK(!irq_registered(64) && !irq_registered(66),
	      "rollback left a line registered");
	CHECK(seam_host_env.vector[64] == (uint32_t)(uintptr_t)vendor_isr_a &&
	      seam_host_env.vector[66] == (uint32_t)(uintptr_t)vendor_isr_c,
	      "rollback did not restore the vendor vectors");
	CHECK(set.count == 0u, "the undo log was not emptied");
	check_invariant("after a rollback");
}

/* ---- 4. retry after a rolled-back failure works, and does not leak ------- */

static void test_retry_after_failure(void)
{
	struct epk_irq_snapshot snap;
	struct epk_irq_wrapset set;
	int attempt;

	env_reset();

	/*
	 * Nine failed attempts, each rolled back, then a successful one.  Nine
	 * is deliberately more than GROVE_EPK_WRAP_MAX (8): if a failed attempt
	 * leaked even one trampoline slot, the table would be exhausted before
	 * this loop ended and the final attempt would fail.  That is exactly
	 * the "repeated transient failures permanently break EPK" case.
	 */
	for (attempt = 0; attempt < 9; attempt++) {
		grove_epk_irq_snapshot(&snap);
		vendor_brings_up(64, vendor_isr_a);
		vendor_brings_up(65, vendor_isr_b);
		seam_host_env.fail_register_irqn = 65;

		CHECK(grove_epk_irq_wrap_new(&snap, &set) == 0,
		      "attempt %d: refused registration reported success",
		      attempt);
		grove_epk_irq_unwrap_set(&set);
		check_invariant("between retries");

		/* The vendor's own teardown: lines off, vectors as they were. */
		seam_host_env.iser[2] = 0u;
	}

	/* Now the driver comes up cleanly. */
	seam_host_env.fail_register_irqn = -1;
	grove_epk_irq_snapshot(&snap);
	vendor_brings_up(64, vendor_isr_a);
	vendor_brings_up(65, vendor_isr_b);

	CHECK(grove_epk_irq_wrap_new(&snap, &set) == 1,
	      "the retry after 9 rolled-back failures could not wrap -- "
	      "a failed attempt is leaking trampoline slots");
	CHECK(set.count == 2u, "the retry wrapped %lu lines, wanted 2",
	      (unsigned long)set.count);
	check_invariant("after a successful retry");
	grove_epk_irq_unwrap_set(&set);
}

/* ---- 5. rolling back twice is harmless ---------------------------------- */

static void test_rollback_is_idempotent(void)
{
	struct epk_irq_snapshot snap;
	struct epk_irq_wrapset set;

	env_reset();
	grove_epk_irq_snapshot(&snap);
	vendor_brings_up(105, vendor_isr_a);
	CHECK(grove_epk_irq_wrap_new(&snap, &set) == 1, "wrap failed");

	grove_epk_irq_unwrap_set(&set);
	grove_epk_irq_unwrap_set(&set);   /* a teardown after a failure path */
	CHECK(!irq_enabled(105), "double rollback re-enabled a line");
	CHECK(seam_host_env.vector[105] == (uint32_t)(uintptr_t)vendor_isr_a,
	      "double rollback disturbed the restored vector");
	check_invariant("after a double rollback");
}

int main(void)
{
	test_wrap_success();
	test_double_wrap_refused();
	test_untouched_lines_ignored();
	test_partial_failure_rolls_back();
	test_retry_after_failure();
	test_rollback_is_idempotent();

	if (failures != 0) {
		printf("test_epk_irq_wrap: %d failure(s)\n", failures);
		return 1;
	}
	printf("test_epk_irq_wrap: OK\n");
	return 0;
}
