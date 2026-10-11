/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Host test for the OCTOSPI1 guard word (issue #142, src/psram_guard.c).
 *
 * WHY THIS EXISTS.  The guard decides who may touch OCTOSPI1 while another
 * command does, and the pairings that matter -- a `psram clk` against a
 * running `nn stream`, two users at once, a refused gate giving the mark back,
 * a release of the wrong kind -- are windows between two consoles that cannot
 * be typed on purpose.  This walks the contention table issue #142's plan
 * fixes as the acceptance condition:
 *
 *     user (lcd on, nn stream, capture...) x user         run together
 *     reconfigurer (psram clk, membench...) x user        refused, both ways
 *     reconfigurer x reconfigurer                         refused
 *     reconfigurer with no user                           admitted
 *     a reconfigurer's failed gate                        gives the mark back
 *     the user count at its limit                         refused, never wraps
 *
 * [!] WHAT IT DOES NOT COVER: that psram.c applies each step atomically
 * (PRIMASK) and asks the scan-out / camera gates AFTER the mark is set.
 */
#include <stdint.h>
#include <stdio.h>

#include "psram_guard.h"

static int fails;

#define EQ(got, want, what)                                                  \
	do {                                                                 \
		unsigned long g_ = (unsigned long)(got);                     \
		unsigned long w_ = (unsigned long)(want);                    \
		if (g_ != w_) {                                              \
			printf("  FAIL %s:%d %s: got 0x%lx, wanted 0x%lx\n", \
			       __func__, __LINE__, what, g_, w_);            \
			fails++;                                             \
		}                                                            \
	} while (0)

/* The word, with each step applied the way psram.c does: only on success. */
static uint32_t w;

static int user_take(void)    { uint32_t n = 0xDEADu; int ok = psram_guard_user_take(w, &n);    if (ok) w = n; return ok; }
static int user_give(void)    { uint32_t n = 0xDEADu; int ok = psram_guard_user_give(w, &n);    if (ok) w = n; return ok; }
static int reconf_take(void)  { uint32_t n = 0xDEADu; int ok = psram_guard_reconf_take(w, &n);  if (ok) w = n; return ok; }
static int reconf_give(void)  { uint32_t n = 0xDEADu; int ok = psram_guard_reconf_give(w, &n);  if (ok) w = n; return ok; }

static void test_users_together(void)
{
	w = 0u;
	EQ(user_take(), 1, "nn stream takes");
	EQ(user_take(), 1, "lcd on takes while nn stream holds");
	EQ(user_take(), 1, "a capture too");
	EQ(psram_guard_users(w), 3, "three users");
	EQ(user_give(), 1, "lcd on gives back");
	EQ(user_give(), 1, "capture gives back");
	EQ(psram_guard_users(w), 1, "nn stream still holds");
	EQ(user_give(), 1, "nn stream stops");
	EQ(w, 0u, "empty");
	/* A give with nobody holding is refused and moves nothing. */
	EQ(user_give(), 0, "give with no user");
	EQ(w, 0u, "still empty");
}

static void test_reconf_vs_users(void)
{
	/* psram clk while nn stream holds: refused, word unchanged. */
	w = 0u;
	EQ(user_take(), 1, "nn stream takes");
	EQ(reconf_take(), 0, "psram clk refused while a user holds");
	EQ(w, 1u, "unchanged");
	EQ(user_give(), 1, "nn stream stops");
	/* No user: the reconfigurer is admitted ... */
	EQ(reconf_take(), 1, "psram clk with no user");
	EQ(w, PSRAM_GUARD_RECONF, "mark set");
	/* ... and then every user is refused (the other direction). */
	EQ(user_take(), 0, "lcd on refused while psram clk runs");
	EQ(user_take(), 0, "nn stream refused too");
	EQ(w, PSRAM_GUARD_RECONF, "unchanged");
	/* Reconfigurers exclude each other. */
	EQ(reconf_take(), 0, "a second reconfigurer refused");
	/* A user's give cannot remove the mark. */
	EQ(user_give(), 0, "wrong-kind release refused");
	EQ(w, PSRAM_GUARD_RECONF, "mark kept");
	EQ(reconf_give(), 1, "psram clk done");
	EQ(w, 0u, "empty");
	/* And the mark's give cannot drop a user. */
	EQ(user_take(), 1, "user");
	EQ(reconf_give(), 0, "wrong-kind release refused");
	EQ(w, 1u, "user kept");
	EQ(user_give(), 1, "user gone");
}

static void test_failed_gate(void)
{
	/* psram_acquire() sets the mark, then asks the scan-out / camera gates;
	 * a gate that refuses gives the mark back, and the word is as before. */
	w = 0u;
	EQ(reconf_take(), 1, "mark set before looking");
	/* While the mark is out, a user that would arm a continuous gate (an
	 * `lcd on`) is refused -- the "take, then look" ordering. */
	EQ(user_take(), 0, "lcd on cannot slip in during the look");
	EQ(reconf_give(), 1, "gate refused: mark given back");
	EQ(w, 0u, "as before");
	EQ(user_take(), 1, "users admitted again");
	EQ(user_give(), 1, "and gone");
}

static void test_limit(void)
{
	uint32_t i;

	w = 0u;
	for (i = 0; i < PSRAM_GUARD_MAX_USERS; i++)
		if (!user_take())
			break;
	EQ(i, PSRAM_GUARD_MAX_USERS, "fills to the limit");
	EQ(user_take(), 0, "one more refused");
	EQ(psram_guard_users(w), PSRAM_GUARD_MAX_USERS, "not wrapped");
	EQ(w & PSRAM_GUARD_RECONF, 0u, "never reaches the mark");
	EQ(reconf_take(), 0, "reconfigurer refused when full");
}

static void test_malformed(void)
{
	uint32_t n = 0xDEADu;

	/* A mark together with users, or stray bits: refused by every step. */
	EQ(psram_guard_user_take(PSRAM_GUARD_RECONF | 1u, &n), 0, "mark+user take");
	EQ(psram_guard_user_give(PSRAM_GUARD_RECONF | 1u, &n), 0, "mark+user give");
	EQ(psram_guard_reconf_give(PSRAM_GUARD_RECONF | 1u, &n), 0, "mark+user rgive");
	EQ(psram_guard_user_take(0x00010000u, &n), 0, "stray bit");
	EQ(n, 0xDEADu, "nothing written on refusal");
	EQ(psram_guard_users(PSRAM_GUARD_RECONF | 1u), 0, "malformed counts 0");
	EQ(psram_guard_user_take(0u, NULL), 0, "NULL out");
}

int main(void)
{
	test_users_together();
	test_reconf_vs_users();
	test_failed_gate();
	test_limit();
	test_malformed();
	if (fails) {
		printf("test_psram_guard: %d FAILED\n", fails);
		return 1;
	}
	printf("test_psram_guard: all passed\n");
	return 0;
}
