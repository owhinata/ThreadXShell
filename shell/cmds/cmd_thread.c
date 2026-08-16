/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    cmd_thread.c
 * @brief   `thread` built-in shell command (owhinata/stm32f746g-disco#13): one combined table of
 *          every ThreadX thread -- state / priority / run count + stack usage.
 *
 * Joins help/echo (cmd_builtin.c) and version/uptime/reboot (cmd_system.c) in the
 * `shell` executable only -- never linked into the host test harness.  It reads
 * board state through the standard buffered output API and touches only the shell
 * instance passed to it, so it stays reentrant across instances (req §10).
 *
 * Enumeration walks ThreadX's created-thread list (_tx_thread_created_ptr /
 * _tx_thread_created_count -- a circular doubly-linked list).  This firmware creates
 * every thread once in tx_application_define() and never deletes one, so the list is
 * static; we snapshot head+count under TX_DISABLE/TX_RESTORE, then walk it with
 * interrupts back on (cli_print waits on a mutex and must not run inside a critical
 * section).  Only those two internal globals are declared here; every per-thread
 * field we read lives in the public TX_THREAD typedef (tx_api.h), so the internal
 * tx_thread.h is not pulled in.
 *
 * Stack peak (high-water) usage is computed by scanning the 0xEF fill ThreadX lays
 * down at create time (tx_thread_create.c, TX_STACK_FILL == 0xEFEFEFEF -- present by
 * default unless TX_DISABLE_STACK_FILLING is defined, which this target does not).
 * This is the same best-effort method as ThreadX's own tx_thread_stack_analyze():
 * a used word can legitimately equal 0xEF, so peak may read a few bytes low.
 *
 * Clean-room design; no third-party code reused.
 */
#include "cli.h"

#include "tx_api.h"          /* TX_THREAD, tx_thread_state defines, TX_DISABLE/RESTORE */
#ifdef TX_EXECUTION_PROFILE_ENABLE
#include "tx_execution_profile.h"  /* EXECUTION_TIME, _tx_execution_*_time_get (issue #19) */
#endif

#include <stdint.h>
#include <stdio.h>           /* snprintf for the cpu% field */

/* stack_peak_used() reads the 0xEF fill ThreadX applies by default.  If a build
 * ever turns that off, the stack columns would be meaningless -- fail loudly here
 * rather than silently print wrong numbers.  (Checked after tx_api.h so the port's
 * macro state is final; never fires for the shell target, which does not set it.) */
#ifdef TX_DISABLE_STACK_FILLING
# error "thread command needs the ThreadX stack fill; do not build the shell with TX_DISABLE_STACK_FILLING"
#endif

/* ThreadX created-thread list head + count (internal globals, declared in the
 * private tx_thread.h).  Declare just the two we need rather than including it. */
extern TX_THREAD *_tx_thread_created_ptr;
extern ULONG      _tx_thread_created_count;

/* tx_thread_state (tx_api.h §0..14) -> short label; index is the state value. */
static const char *state_name(UINT s)
{
	static const char *const names[] = {
		"ready",   /*  0 TX_READY          */
		"compl",   /*  1 TX_COMPLETED      */
		"term",    /*  2 TX_TERMINATED     */
		"susp",    /*  3 TX_SUSPENDED      */
		"sleep",   /*  4 TX_SLEEP          */
		"queue",   /*  5 TX_QUEUE_SUSP     */
		"sem",     /*  6 TX_SEMAPHORE_SUSP */
		"event",   /*  7 TX_EVENT_FLAG     */
		"block",   /*  8 TX_BLOCK_MEMORY   */
		"byte",    /*  9 TX_BYTE_MEMORY    */
		"io",      /* 10 TX_IO_DRIVER      */
		"file",    /* 11 TX_FILE           */
		"tcpip",   /* 12 TX_TCP_IP         */
		"mutex",   /* 13 TX_MUTEX_SUSP     */
		"pchg",    /* 14 TX_PRIORITY_CHANGE*/
	};
	return (s < (sizeof names / sizeof names[0])) ? names[s] : "?";
}

/*
 * Peak (high-water) stack usage in bytes.  The stack grows down from
 * tx_thread_stack_end (high) toward tx_thread_stack_start (low), so the untouched
 * tail keeps its 0xEF fill at the low end.  The leading 0xEF run from stack_start
 * is the free headroom; peak = size - free.  (Same scan as _tx_thread_stack_analyze.)
 */
static ULONG stack_peak_used(const TX_THREAD *t)
{
	const UCHAR *base = (const UCHAR *)t->tx_thread_stack_start;
	ULONG size  = t->tx_thread_stack_size;
	ULONG freeb = 0;

	while (freeb < size && base[freeb] == (UCHAR)0xEF)
		freeb++;
	return size - freeb;
}

/*
 * Single-run guard.  The cpu% snapshot below is process-wide static state with
 * no locking of its own, so two concurrent runs (`thread` at the prompt while a
 * `thread &` background job walks the same table) would interleave their reads
 * and their commit and produce garbage windows for both.  Board-independent:
 * TX_DISABLE/TX_RESTORE are the same PRIMASK critical section on all three
 * ports, so no CMSIS header is needed here.
 *
 * Only reachable where the snapshot exists, i.e. under EPK -- on a board
 * without it the command is a pure read of ThreadX-owned state.
 */
#ifdef TX_EXECUTION_PROFILE_ENABLE
static volatile UINT thread_busy;

static int thread_try_acquire(void)
{
	TX_INTERRUPT_SAVE_AREA
	int ok;

	TX_DISABLE
	ok = (thread_busy == 0u);
	if (ok)
		thread_busy = 1u;
	TX_RESTORE
	return ok;
}

static void thread_release(void)
{
	thread_busy = 0u;
}
#else
/* No snapshot to protect without the kit: the command is then a pure read of
 * ThreadX-owned state, so concurrent runs stay allowed exactly as before. */
static int  thread_try_acquire(void) { return 1; }
static void thread_release(void)     { }
#endif

#ifdef TX_EXECUTION_PROFILE_ENABLE
/*
 * Board hook: can the cpu% numbers be believed on THIS port?
 *
 * The Execution Profile Kit is a COMPILE-TIME switch -- once built in, it
 * cannot be turned off at runtime, and its ISR accounting depends on every ISR
 * on the board calling the kit's enter/exit hooks.  Where that plumbing can
 * fail at runtime (the Grove port wraps a vendor-installed vector and brings up
 * a dedicated timer, either of which can refuse), the board overrides this weak
 * default so the command can print "--" plus a reason instead of numbers that
 * look right and are not.
 *
 * Default: available.  Boards that do not override are unaffected.
 * Contract: return 1 to trust the numbers; return 0 and, when `why` is
 * non-NULL, point *why at a one-line static reason.
 */
__attribute__((weak)) int cli_thread_cpu_source_ok(const char **why)
{
	(void)why;
	return 1;
}

/*
 * "top"-style cpu% (owhinata/wio-lite-ai#2).  The ThreadX Execution Profile Kit accumulates,
 * per thread and globally, busy time in the ticks of whatever free-running
 * counter that board's TX_EXECUTION_TIME_SOURCE names (see its tx_user.h /
 * port/threadx/tx_glue.c).  Only the RATIO matters here, so the counter's rate
 * never enters the arithmetic: cpu% = delta_thread / window, where window =
 * delta(all threads + isr + idle) over the window since the previous `thread`
 * run.  The previous snapshot lives here (threads are static and never deleted,
 * so a small pointer-keyed table suffices); the first run has no prior and
 * prints "--".
 */
#define THREAD_CPU_PREV_MAX 16
struct cpu_snap { TX_THREAD *t; EXECUTION_TIME prev; };
static struct cpu_snap cpu_prev[THREAD_CPU_PREV_MAX];
static UINT           cpu_prev_count;
static EXECUTION_TIME cpu_prev_thread, cpu_prev_isr, cpu_prev_idle;
static int            cpu_have_prev;

static EXECUTION_TIME cpu_prev_lookup(const TX_THREAD *t)
{
	UINT i;
	for (i = 0; i < cpu_prev_count; i++)
		if (cpu_prev[i].t == t)
			return cpu_prev[i].prev;
	return 0;   /* unseen thread -> baseline 0 (its first window may read high once) */
}

/* This run's snapshot, keyed the same way.  Returns 0 when the thread was not
 * sampled (more threads exist than the table holds). */
static int cpu_cur_lookup(const struct cpu_snap *snap, UINT n,
                          const TX_THREAD *t, EXECUTION_TIME *out)
{
	UINT i;
	for (i = 0; i < n; i++)
		if (snap[i].t == t) {
			*out = snap[i].prev;
			return 1;
		}
	return 0;
}

/* Render delta/window as "NN.N%" clamped to 0..100.0, or "--" when there is no
 * prior window (have == 0) or the window is empty.  delta is pre-clamped <= window. */
static void cpu_fmt(char *buf, size_t n, int have, EXECUTION_TIME delta, EXECUTION_TIME window)
{
	ULONG tenths;

	if (!have || window == 0) {
		buf[0] = '-'; buf[1] = '-'; buf[2] = '\0';
		return;
	}
	tenths = (ULONG)((delta * 1000ULL) / window);
	if (tenths > 1000u)
		tenths = 1000u;   /* clamp (non-atomic 64-bit reads / quantization) */
	snprintf(buf, n, "%lu.%lu%%", tenths / 10u, tenths % 10u);
}
#endif /* TX_EXECUTION_PROFILE_ENABLE */

static int cmd_thread(struct cli_instance *sh, int argc, char **argv)
{
	TX_INTERRUPT_SAVE_AREA

	TX_THREAD *t;
	ULONG count, i;
#ifdef TX_EXECUTION_PROFILE_ENABLE
	EXECUTION_TIME tt = 0, it = 0, id = 0, window;
	struct cpu_snap next_prev[THREAD_CPU_PREV_MAX];
	UINT next_count = 0;
	const char *cpu_why = NULL;
	int  cpu_ok;
	int  have;
	char cpubuf[8];
#endif

	(void)argc;
	(void)argv;

	/* Taken after argument validation (there is none to do here) and before
	 * any state is read, so the whole snapshot-walk-commit sequence is
	 * serialised.  Every exit below funnels through `done`. */
	if (!thread_try_acquire()) {
		cli_error(sh, "thread: already running\r\n");
		return 1;
	}

#ifdef TX_EXECUTION_PROFILE_ENABLE
	/* A board can declare its ISR accounting untrustworthy; keep the column
	 * layout and blank the values rather than silently printing wrong ones. */
	cpu_ok = cli_thread_cpu_source_ok(&cpu_why);
	have   = cpu_have_prev && cpu_ok;
	if (!cpu_ok)
		cli_warn(sh, "thread: cpu%% unavailable (%s)\r\n",
		         (cpu_why != NULL) ? cpu_why : "board reported no source");
#endif

	/* Atomically snapshot the created-list head + count (and, for cpu%, the
	 * global busy totals AND every per-thread total -- 64-bit, so read inside
	 * the same critical section for a coherent window).  The nodes are static
	 * (nothing is ever deleted while we look), so we walk the list afterwards
	 * with interrupts on -- cli_print waits on a mutex and must not run in a
	 * critical section.
	 *
	 * [!] The per-thread totals MUST be captured here rather than read during
	 * the walk.  The window is derived from the global totals taken at this
	 * instant, but the running thread keeps accumulating while the table is
	 * being printed: every SysTick flushes its elapsed slice into both its own
	 * total and the global one (tx_execution_profile.c, _tx_execution_isr_enter).
	 * Reading a thread's total later therefore compares a NEWER numerator
	 * against an OLDER denominator, and at 921600 baud a table takes long
	 * enough that the busy thread's share visibly exceeds 100% and the column
	 * stops summing to 100.  Sampling everything at one instant makes the rows
	 * add up by construction. */
	TX_DISABLE
	t     = _tx_thread_created_ptr;
	count = _tx_thread_created_count;
#ifdef TX_EXECUTION_PROFILE_ENABLE
	_tx_execution_thread_total_time_get(&tt);
	_tx_execution_isr_time_get(&it);
	_tx_execution_idle_time_get(&id);
	{
		TX_THREAD *p = t;
		ULONG      n;

		for (n = 0; n < count && next_count < THREAD_CPU_PREV_MAX;
		     n++, p = p->tx_thread_created_next) {
			next_prev[next_count].t    = p;
			next_prev[next_count].prev = p->tx_thread_execution_time_total;
			next_count++;
		}
	}
#endif
	TX_RESTORE

#ifdef TX_EXECUTION_PROFILE_ENABLE
	window = (tt - cpu_prev_thread) + (it - cpu_prev_isr) + (id - cpu_prev_idle);
	cli_print(sh, "%-20s %-6s %3s %8s %6s %5s %5s %6s\r\n",
	          "name", "state", "pri", "runs", "size", "peak", "use%", "cpu%");
#else
	cli_print(sh, "%-20s %-6s %3s %8s %6s %5s %5s %4s\r\n",
	          "name", "state", "pri", "runs", "size", "peak", "free", "use%");
#endif

	if (t == TX_NULL || count == 0) {
		cli_print(sh, "(no threads)\r\n");
		goto done;
	}

	for (i = 0; i < count; i++, t = t->tx_thread_created_next) {
		/* Ctrl+C between rows: stop before emitting the next one (owhinata/stm32f746g-disco#16).
		 * Outside the TX_DISABLE region below -- cli_cancel_requested() drains the
		 * transport and must not run with interrupts disabled.  The dispatcher
		 * detects cancel_req and prints "^C", so just return. */
		if (cli_cancel_requested(sh))
			goto done;

		ULONG size  = t->tx_thread_stack_size;
		ULONG peak  = stack_peak_used(t);
		ULONG pct   = size ? (peak * 100u) / size : 0u;
		const char *name = t->tx_thread_name ? t->tx_thread_name : "(unnamed)";
#ifdef TX_EXECUTION_PROFILE_ENABLE
		EXECUTION_TIME cur, prev, delta;
		int            sampled = cpu_cur_lookup(next_prev, next_count, t, &cur);

		/* Threads past THREAD_CPU_PREV_MAX were not in the snapshot, so there
		 * is no coherent numerator for them: print "--" rather than mix a live
		 * read into a window taken earlier. */
		prev  = cpu_prev_lookup(t);
		delta = (have && sampled && cur >= prev) ? (cur - prev) : 0;
		cpu_fmt(cpubuf, sizeof cpubuf, have && sampled, delta, window);

		cli_print(sh, "%-20s %-6s %3u %8lu %6lu %5lu %4lu%% %6s\r\n",
		          name, state_name(t->tx_thread_state),
		          t->tx_thread_priority,
		          (unsigned long)t->tx_thread_run_count,
		          (unsigned long)size, (unsigned long)peak,
		          (unsigned long)pct, cpubuf);
#else
		ULONG freeb = size - peak;

		cli_print(sh, "%-20s %-6s %3u %8lu %6lu %5lu %5lu %3lu%%\r\n",
		          name, state_name(t->tx_thread_state),
		          t->tx_thread_priority,
		          (unsigned long)t->tx_thread_run_count,
		          (unsigned long)size, (unsigned long)peak,
		          (unsigned long)freeb, (unsigned long)pct);
#endif
	}

#ifdef TX_EXECUTION_PROFILE_ENABLE
	/* Pseudo-rows so the cpu% column sums to ~100%: idle headroom and ISR load. */
	cpu_fmt(cpubuf, sizeof cpubuf, have, id - cpu_prev_idle, window);
	cli_print(sh, "%-20s %-6s %3s %8s %6s %5s %5s %6s\r\n",
	          "(idle)", "", "", "", "", "", "", cpubuf);
	cpu_fmt(cpubuf, sizeof cpubuf, have, it - cpu_prev_isr, window);
	cli_print(sh, "%-20s %-6s %3s %8s %6s %5s %5s %6s\r\n",
	          "(isr)", "", "", "", "", "", "", cpubuf);

	/* Commit this snapshot as the baseline for the next `thread`. */
	for (i = 0; i < next_count; i++)
		cpu_prev[i] = next_prev[i];
	cpu_prev_count  = next_count;
	cpu_prev_thread = tt;
	cpu_prev_isr    = it;
	cpu_prev_idle   = id;
	cpu_have_prev   = 1;
#endif

done:
	thread_release();   /* single cleanup point: guard cleared on every exit */
	return 0;
}

CLI_CMD_REGISTER(thread, NULL, "list threads + stack usage", cmd_thread, 1, 0);
