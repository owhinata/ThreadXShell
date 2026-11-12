/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Host unit test for the console hand-over of a binary transfer (issue #229):
 * cli_console_claim() / cli_console_release() / cli_console_claimable() as the
 * firmware builds them (shell/core/cli_printf.c), and cli_console_claimed()
 * (shell/include/cli_instance.h), over the host output lock (host_glue.c),
 * which counts its takes and releases and can be made to fail.
 *
 * The claim is a count per console.  Every case also checks that the lock is
 * held by exactly what the count says: across the case, (takes - releases)
 * changes by the same amount as the count.  The lock counters are compared by
 * their change, never by their value.
 *
 *   (a) one console: claim -> claimed, release -> not claimed; a background
 *       job is refused with -2 and leaves the count at 0;
 *   (b) two consoles A and B both claim; B's release leaves A claimed, and
 *       A's own release ends it;
 *   (c) A claims twice: one release leaves it claimed, the second ends it;
 *   (d) a background job asking sees its foreground's count;
 *   (e) a release without a claim gives no lock back and changes nothing;
 *   (f) the lock cannot be taken: -1, and neither the count nor the lock moves;
 *   (g) at the count's limit the claim is refused with -3 (not -1, not -2),
 *       the lock it took is given back and the count does not wrap.
 */
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cli.h"
#include "cli_instance.h"
#include "cli_internal.h"
#include "cli_backend_dummy.h"
#include "host_glue.h"

CLI_BACKEND_DUMMY_DEFINE(tr_a);
CLI_BACKEND_DUMMY_DEFINE(tr_b);
static struct cli_instance a;    /* a foreground console */
static struct cli_instance b;    /* another foreground console */
static struct cli_instance job;  /* a background-job worker of a */
static int failures;

static void check(const char *what, int ok)
{
	printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
	if (!ok)
		failures++;
}

static void setup(void)
{
	memset(&a, 0, sizeof a);
	memset(&b, 0, sizeof b);
	memset(&job, 0, sizeof job);
	a.tr   = &tr_a;
	tr_a.sh = &a;
	b.tr   = &tr_b;
	tr_b.sh = &b;
	job.tr = &tr_a;              /* a worker aliases its foreground's transport */
	job.fg = &a;
	cli_test_set_lock_fail(0);
}

/* Lock takes not yet given back, since the program started. */
static int held(void)
{
	return (int)(cli_test_lock_gets() - cli_test_lock_puts());
}

static void test_one_console(void)
{
	printf("(a) one console\n");
	setup();
	int h0 = held();

	check("claim succeeds", cli_console_claim(&a) == 0);
	check("the console is claimed", cli_console_claimed(&a));
	check("the claim holds the lock once", held() - h0 == 1);

	cli_console_release(&a);
	check("released: not claimed", !cli_console_claimed(&a));
	check("released: the lock is given back", held() - h0 == 0);

	unsigned g0 = cli_test_lock_gets();
	check("a background job is refused with -2", cli_console_claim(&job) == -2);
	check("before the lock is touched", cli_test_lock_gets() == g0);
	check("and the count stays 0",
	      a.console_claims == 0u && job.console_claims == 0u &&
	      !cli_console_claimed(&a));
	check("nothing is held", held() - h0 == 0);
}

static void test_two_consoles(void)
{
	printf("(b) two consoles\n");
	setup();
	int h0 = held();

	check("A claims", cli_console_claim(&a) == 0);
	check("B claims", cli_console_claim(&b) == 0);
	check("both are claimed", cli_console_claimed(&a) && cli_console_claimed(&b));
	check("two claims hold two locks", held() - h0 == 2);

	cli_console_release(&b);
	check("B's release leaves A claimed", cli_console_claimed(&a));
	check("and B not claimed", !cli_console_claimed(&b));
	check("one lock left", held() - h0 == 1);

	cli_console_release(&a);
	check("A's release ends A's claim", !cli_console_claimed(&a));
	check("nothing is held", held() - h0 == 0);
}

static void test_nested(void)
{
	printf("(c) one console claimed twice\n");
	setup();
	int h0 = held();

	check("first claim", cli_console_claim(&a) == 0);
	check("second claim", cli_console_claim(&a) == 0);
	check("two locks for two claims", held() - h0 == 2 && a.console_claims == 2u);

	cli_console_release(&a);
	check("the inner release leaves it claimed", cli_console_claimed(&a));
	check("one lock left", held() - h0 == 1 && a.console_claims == 1u);

	cli_console_release(&a);
	check("the outer release ends it", !cli_console_claimed(&a));
	check("nothing is held", held() - h0 == 0);
}

static void test_job_sees_foreground(void)
{
	printf("(d) a background job asks\n");
	setup();
	int h0 = held();

	check("not claimed before", !cli_console_claimed(&job));
	check("the foreground claims", cli_console_claim(&a) == 0);
	check("the job sees its foreground's claim", cli_console_claimed(&job));
	cli_console_release(&job);
	check("a job's release is not the foreground's",
	      cli_console_claimed(&a) && held() - h0 == 1);
	cli_console_release(&a);
	check("released: the job sees no claim", !cli_console_claimed(&job));
	check("nothing is held", held() - h0 == 0);
}

static void test_release_unclaimed(void)
{
	printf("(e) release without a claim\n");
	setup();
	int      h0 = held();
	unsigned p0 = cli_test_lock_puts();

	cli_console_release(&a);
	check("no lock is given back", cli_test_lock_puts() == p0);
	check("the count stays 0", a.console_claims == 0u && !cli_console_claimed(&a));

	/* And with an output lock of somebody else's taken: it is still theirs. */
	check("an outer holder takes the lock", cli_lock(&a) == 0);
	cli_console_release(&a);
	check("the release does not give that lock back", held() - h0 == 1);
	cli_unlock(&a);
	check("nothing is held", held() - h0 == 0);
}

static void test_lock_fails(void)
{
	printf("(f) the lock cannot be taken\n");
	setup();
	int      h0 = held();
	unsigned g0 = cli_test_lock_gets();
	unsigned p0 = cli_test_lock_puts();

	cli_test_set_lock_fail(1);
	check("the claim is refused with -1", cli_console_claim(&a) == -1);
	check("the count does not move", a.console_claims == 0u && !cli_console_claimed(&a));
	check("the lock does not move",
	      cli_test_lock_gets() == g0 && cli_test_lock_puts() == p0);
	cli_test_set_lock_fail(0);

	check("the next claim works", cli_console_claim(&a) == 0);
	cli_console_release(&a);
	check("and its release ends it", !cli_console_claimed(&a));
	check("nothing is held", held() - h0 == 0);
}

static void test_limit(void)
{
	printf("(g) the count's limit\n");
	setup();
	/* Placed just below the limit rather than reached by 65,535 claims; the
	 * lock stand-in does not know about these, so only changes are compared. */
	a.console_claims = (uint16_t)(UINT16_MAX - 1u);
	int h0 = held();

	check("claimable just below the limit", cli_console_claimable(&a) == 0);
	check("the claim to the limit succeeds", cli_console_claim(&a) == 0);
	check("the count is at the limit", a.console_claims == UINT16_MAX);
	check("and holds one more lock", held() - h0 == 1);

	unsigned g0 = cli_test_lock_gets();
	check("claimable answers -3 at the limit", cli_console_claimable(&a) == -3);
	check("a claim past the limit is refused with -3", cli_console_claim(&a) == -3);
	check("after taking the lock once", cli_test_lock_gets() == g0 + 1u);
	check("which it gave back", held() - h0 == 1);
	check("the count does not wrap", a.console_claims == UINT16_MAX &&
	      cli_console_claimed(&a));

	cli_console_release(&a);
	check("one release gives one lock back", held() - h0 == 0 &&
	      a.console_claims == (uint16_t)(UINT16_MAX - 1u));
	a.console_claims = 0u;      /* the placed claims were never taken */

	check("a background job is claimable -2", cli_console_claimable(&job) == -2);
}

int main(void)
{
	test_one_console();
	test_two_consoles();
	test_nested();
	test_job_sees_foreground();
	test_release_unclaimed();
	test_lock_fails();
	test_limit();
	if (failures) {
		printf("test_console_claim: %d FAILED\n", failures);
		return 1;
	}
	printf("OK: console claim / one console / two consoles / nested / job / "
	       "unclaimed release / lock fails / limit pass\n");
	return 0;
}
