/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Host unit test for the background-job pool's one-time initialisation
 * (issue #243): shell/core/cli_job.c compiled as it is, over the ThreadX type
 * shim (shim/tx_api.h) and fake ThreadX calls defined here.  cli_init() calls
 * cli_jobs_init_once() first thing for every instance; cli_core.c cannot be
 * built on the host (it needs ThreadX and an MRS on IPSR), so that call is
 * checked on the firmware, and this test pins what the called function does.
 *
 * The fake event-flags create counts every call, refuses (like ThreadX) to
 * create a group that already exists, can be told to fail its n-th call and
 * can run a hook inside a call -- the hook stands in for another thread that
 * runs while the initialisation is still creating.  Which slot a launch took
 * is read from the group the launch clears (cli_job.c resets the slot's own
 * group), compared with the group each create call was given: the slots are
 * created in table order, so the n-th call is slot n-1.
 *
 * The pool's state is the file's own static storage, so every case runs in a
 * fresh child process (fork) and starts from a pool that was never set up.
 *
 *   (a) the first call creates one group per slot; a 2nd and 3rd call create
 *       none, and the two slots are then used in turn and a third launch is
 *       refused with the same text as today;
 *   (b) a call made while the first is still creating creates nothing;
 *   (c) the first or the second create fails: that slot is never used, the
 *       other one is, a later call does not create it again, and the launch
 *       after the usable slot is taken is refused with the same text;
 *   (d) both creates fail: every launch is refused with the same text and no
 *       thread is created;
 *   (e) a launch while the first group is being created is refused with the
 *       same text, and works once the call has returned;
 *   (f) a launch while the second group is being created takes the first
 *       slot, and the rest of the initialisation leaves that job alone: `jobs`
 *       still lists it, the next launch takes the second slot with the next
 *       number, and the one after is refused.
 */
#define _POSIX_C_SOURCE 200809L

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "tx_api.h"
#include "cli.h"
#include "cli_instance.h"
#include "cli_internal.h"
#include "cli_registry.h"

#define TX_GROUP_ERROR_FAKE ((UINT)0x06)   /* ThreadX's TX_GROUP_ERROR */
#define MAX_GROUPS 16

static const char refused[] = "too many background jobs (max 2)\r\n";

/* ---- fake ThreadX ------------------------------------------------------------- */

static TX_EVENT_FLAGS_GROUP *created[MAX_GROUPS];  /* in the order created */
static TX_EVENT_FLAGS_GROUP *by_call[MAX_GROUPS];  /* the group of each call */
static int    n_created;         /* creates that succeeded */
static int    n_create_calls;    /* every create call */
static int    n_recreate;        /* creates of a group that already existed */
static int    n_create_masked;   /* creates called inside a masked section */
static unsigned fail_mask;       /* bit n-1 set: the n-th create call fails */
static void (*create_hook)(int call);
static int    masked;            /* depth of the masked section */
static TX_EVENT_FLAGS_GROUP *last_cleared;   /* the group a launch cleared */
static int    n_threads;

UINT tx_shim_disable(void)        { masked++; return 0; }
void tx_shim_restore(UINT saved)  { (void)saved; masked--; }

UINT tx_event_flags_create(TX_EVENT_FLAGS_GROUP *g, CHAR *name)
{
	int call = ++n_create_calls;
	int i;

	(void)name;
	if (masked)
		n_create_masked++;
	for (i = 0; i < n_created; i++)
		if (created[i] == g) {
			n_recreate++;
			return TX_GROUP_ERROR_FAKE;
		}
	if (call <= MAX_GROUPS)
		by_call[call - 1] = g;
	if (create_hook)
		create_hook(call);
	if (call <= 32 && (fail_mask & (1u << (call - 1))))
		return TX_GROUP_ERROR_FAKE;
	if (n_created < MAX_GROUPS)
		created[n_created++] = g;
	return TX_SUCCESS;
}

UINT tx_event_flags_set(TX_EVENT_FLAGS_GROUP *g, ULONG flags, UINT option)
{
	(void)flags;
	if (option == TX_AND)
		last_cleared = g;
	return TX_SUCCESS;
}

UINT tx_thread_create(TX_THREAD *t, CHAR *name, VOID (*entry)(ULONG), ULONG arg,
                      VOID *stack, ULONG stack_size, UINT prio, UINT preempt,
                      ULONG slice, UINT autostart)
{
	(void)name; (void)entry; (void)arg; (void)stack; (void)stack_size;
	(void)prio; (void)preempt; (void)slice; (void)autostart;
	t->tx_thread_state = 0;   /* READY: never completes in this test */
	n_threads++;
	return TX_SUCCESS;
}

UINT tx_thread_delete(TX_THREAD *t) { (void)t; return TX_SUCCESS; }

/* ---- the rest of the shell that cli_job.c calls -------------------------------- */

static char out[1024];
static size_t out_len;

static int capture(const char *fmt, va_list ap)
{
	int n = vsnprintf(out + out_len, sizeof out - out_len, fmt, ap);

	if (n > 0)
		out_len += (size_t)n < sizeof out - out_len ? (size_t)n
		                                            : sizeof out - out_len - 1;
	return n;
}

int cli_print(struct cli_instance *sh, const char *fmt, ...)
{
	va_list ap;
	int n;

	(void)sh;
	va_start(ap, fmt);
	n = capture(fmt, ap);
	va_end(ap);
	return n;
}

int cli_error(struct cli_instance *sh, const char *fmt, ...)
{
	va_list ap;
	int n;

	(void)sh;
	va_start(ap, fmt);
	n = capture(fmt, ap);
	va_end(ap);
	return n;
}

int  cli_register_thread(TX_THREAD *t, struct cli_instance *sh) { (void)t; (void)sh; return 0; }
void cli_unregister_thread(TX_THREAD *t) { (void)t; }
const char *cli_reg_strerror(enum cli_reg_status st) { (void)st; return "fake"; }
void cli_dispatch_segment(struct cli_instance *sh, char *seg) { (void)sh; (void)seg; }

/* ---- helpers ----------------------------------------------------------------- */

static struct cli_instance fg;
static int failures;

static void check(const char *what, int ok)
{
	printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
	if (!ok)
		failures++;
}

static void out_reset(void) { out_len = 0; out[0] = '\0'; }

/* Launch @p cmd; returns the slot it took (0 / 1), -1 when the launch was
 * refused, -2 when it succeeded on a slot whose group was never created, -3
 * when it reported failure after creating a thread. */
static int launch(const char *cmd)
{
	char seg[64];
	int threads = n_threads;
	int i;

	snprintf(seg, sizeof seg, "%s", cmd);
	last_cleared = NULL;
	if (cli_job_launch(&fg, seg) != 0)
		return n_threads == threads ? -1 : -3;
	for (i = 0; i < n_created; i++)
		if (created[i] == last_cleared)
			break;
	if (i == n_created)
		return -2;
	for (i = 0; i < MAX_GROUPS; i++)
		if (by_call[i] == last_cleared)
			return i;
	return -2;
}

/* ---- cases ------------------------------------------------------------------- */

static void case_once(void)
{
	cli_jobs_init_once();
	check("the first call creates one group per slot", n_created == CLI_MAX_BG_JOBS);
	cli_jobs_init_once();
	cli_jobs_init_once();
	check("a 2nd and 3rd call create nothing",
	      n_create_calls == CLI_MAX_BG_JOBS && n_recreate == 0);
	check("no create runs with interrupts masked", n_create_masked == 0);

	out_reset();
	check("the first launch takes the first slot", launch("sleep 5") == 0);
	check("... and announces job 1", strcmp(out, "[1] sleep 5\r\n") == 0);
	check("the second launch takes the second slot", launch("sleep 30") == 1);
	out_reset();
	check("a third launch is refused", launch("sleep 30") == -1);
	check("... with today's text", strcmp(out, refused) == 0);
}

static void nested_init(int call)
{
	if (call == 1)
		cli_jobs_init_once();   /* another caller, while the first creates */
}

static void case_nested(void)
{
	create_hook = nested_init;
	cli_jobs_init_once();
	create_hook = NULL;
	check("a call during the first call's creates creates nothing",
	      n_create_calls == CLI_MAX_BG_JOBS && n_recreate == 0 &&
	      n_created == CLI_MAX_BG_JOBS);
}

static void case_fail(int which)   /* 1 or 2: the create call that fails */
{
	int usable = which == 1 ? 1 : 0;   /* the other slot */

	fail_mask = 1u << (which - 1);
	cli_jobs_init_once();
	fail_mask = 0;
	check("one group created", n_created == 1);
	cli_jobs_init_once();
	check("a later call does not create the failed slot again",
	      n_create_calls == CLI_MAX_BG_JOBS && n_recreate == 0);

	check("a launch takes the slot whose group exists", launch("sleep 5") == usable);
	out_reset();
	check("the next launch is refused (the failed slot is never used)",
	      launch("sleep 5") == -1);
	check("... with today's text", strcmp(out, refused) == 0);
}

static void case_none_usable(void)
{
	fail_mask = (1u << CLI_MAX_BG_JOBS) - 1u;
	cli_jobs_init_once();
	fail_mask = 0;
	check("no group created", n_created == 0);
	out_reset();
	check("a launch is refused", launch("sleep 5") == -1);
	check("... with today's text", strcmp(out, refused) == 0);
	out_reset();
	check("so is the next one", launch("sleep 5") == -1);
	check("... with today's text", strcmp(out, refused) == 0);
	check("no thread was created", n_threads == 0);
}

static void hook_launch_first(int call)
{
	if (call == 1) {
		out_reset();
		check("a launch during the first create is refused",
		      launch("sleep 5") == -1);
		check("... with today's text", strcmp(out, refused) == 0);
	}
}

static void case_launch_during_first(void)
{
	create_hook = hook_launch_first;
	cli_jobs_init_once();
	create_hook = NULL;
	check("no thread was created", n_threads == 0);
	out_reset();
	check("after the call, a launch takes the first slot", launch("sleep 5") == 0);
	check("... as job 1", strcmp(out, "[1] sleep 5\r\n") == 0);
}

static int during_second_slot = -9;
static void hook_launch_second(int call)
{
	if (call == 2) {
		out_reset();
		during_second_slot = launch("sleep 5");
	}
}

static void case_launch_during_second(void)
{
	create_hook = hook_launch_second;
	cli_jobs_init_once();
	create_hook = NULL;
	check("a launch during the second create takes the first slot",
	      during_second_slot == 0);
	check("... as job 1", strcmp(out, "[1] sleep 5\r\n") == 0);

	out_reset();
	cli_jobs_print(&fg);
	check("after the call, `jobs` still lists job 1",
	      strcmp(out, "[1] Running    sleep 5\r\n") == 0);
	out_reset();
	check("the next launch takes the second slot", launch("sleep 30") == 1);
	check("... as job 2", strcmp(out, "[2] sleep 30\r\n") == 0);
	out_reset();
	check("the one after is refused", launch("sleep 30") == -1);
	check("... with today's text", strcmp(out, refused) == 0);
}

/* ---- driver ------------------------------------------------------------------ */

struct test_case {
	const char *name;
	void (*run)(void);
};

static void case_fail_first(void)  { case_fail(1); }
static void case_fail_second(void) { case_fail(2); }

static const struct test_case cases[] = {
	{ "(a) one initialisation however often it is called", case_once },
	{ "(b) a call during the first call", case_nested },
	{ "(c) the first create fails", case_fail_first },
	{ "(c) the second create fails", case_fail_second },
	{ "(d) no slot usable", case_none_usable },
	{ "(e) a launch during the first create", case_launch_during_first },
	{ "(f) a launch during the second create", case_launch_during_second },
};

int main(void)
{
	size_t i;
	int bad = 0;

	for (i = 0; i < sizeof cases / sizeof cases[0]; i++) {
		pid_t pid;
		int status = 0;

		printf("%s\n", cases[i].name);
		fflush(stdout);
		pid = fork();
		if (pid < 0) {
			perror("fork");
			return 1;
		}
		if (pid == 0) {
			cases[i].run();
			fflush(stdout);
			_exit(failures ? 1 : 0);
		}
		if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status) ||
		    WEXITSTATUS(status) != 0) {
			printf("  FAIL the case did not pass (status 0x%x)\n", status);
			bad++;
		}
	}
	if (bad) {
		printf("test_cli_job: %d case(s) FAILED\n", bad);
		return 1;
	}
	printf("test_cli_job: all cases passed\n");
	return 0;
}
