/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    nn_worker.c
 * @brief   The inference worker thread and its join (issue #129).  See
 *          nn_worker.h; the hand-over decisions are svc/nn_handoff.c's, run by
 *          svc/nn_core_frame.c (issue #130).
 */
#include "nn_worker.h"

#include "tx_api.h"

#include "cam_lcd_sink.h"   /* CAM_PANEL_PRIO                       */
#include "camera.h"         /* CAM_PRODUCER_PRIO                    */
#include "cli_config.h"     /* CLI_INSTANCE_{PRIORITY,STACK_SIZE}   */
#include "nn_overlay.h"     /* nn_overlay_work(): the job; the word */
#include "npu_hw.h"         /* NPU_INFERENCE_TIMEOUT_TICKS          */
#include "plugin_lease.h"   /* PLUGIN_LEASE_WAIT_MS                 */

#define LOG_TAG "nn_worker"
#include "log.h"

_Static_assert(CAM_PANEL_PRIO < CAM_PRODUCER_PRIO &&
               CAM_PRODUCER_PRIO < NN_WORKER_PRIO &&
               NN_WORKER_PRIO < (unsigned)CLI_INSTANCE_PRIORITY,
               "panel < producer < worker < console (issue #129, option C); "
               "worker < mve is asserted in cmds/cmd_mve.c");
_Static_assert(NN_WORKER_STACK_BYTES >= (unsigned)CLI_INSTANCE_STACK_SIZE,
               "the worker becomes the decode's shallowest ceiling: never "
               "below the console's stack (issue #129)");
_Static_assert((NN_WORKER_STACK_BYTES % 8u) == 0u,
               "AAPCS stack alignment");

/*
 * How long a stop waits for the worker, in ticks of the wall clock.
 *
 * Derived, not chosen, from what a worker that is behaving can be inside of
 * when the stop comes in:
 *   - one invoke: the ethos-u driver can take its inference semaphore TWICE on
 *     its timeout/interrupt race path, each bounded by
 *     NPU_INFERENCE_TIMEOUT_TICKS (npu_hw.h)              2 x 1000 ticks
 *   - the plugin lease, which the worker takes bounded     PLUGIN_LEASE_WAIT_MS
 *   - the decode (tens of microseconds) and the time the producer and the
 *     panel preempt it, both above this thread             NN_WORKER_JOIN_SLACK
 * A stop pending means the worker does not START an invoke, so this covers at
 * most the one already running.
 *
 * [!] THE NUMBER IS NOT THE SAFETY.  What keeps a late worker harmless is the
 * state: a join that runs out is the terminal "worker did not return", and
 * nothing it may still touch is unlinked or released.  This only decides how
 * long an operator waits for that verdict.
 */
#define NN_WORKER_JOIN_SLACK_MS 250u
#define NN_WORKER_MS_TO_TICKS(ms) \
	(((ms) * (unsigned)TX_TIMER_TICKS_PER_SECOND + 999u) / 1000u)
#define NN_WORKER_JOIN_TICKS                                  \
	(2u * NPU_INFERENCE_TIMEOUT_TICKS +                      \
	 NN_WORKER_MS_TO_TICKS(PLUGIN_LEASE_WAIT_MS) +            \
	 NN_WORKER_MS_TO_TICKS(NN_WORKER_JOIN_SLACK_MS))

/* Event flag bits.  Wake-ups only: the word below is the truth. */
#define NN_WK_WAKE    0x1u   /* producer -> worker: something was handed over */
#define NN_WK_SETTLED 0x2u   /* worker -> stop: it went back to parked        */

static TX_THREAD             nn_worker_thread;
static UCHAR                 nn_worker_stack[NN_WORKER_STACK_BYTES]
	__attribute__((aligned(8)));
static TX_EVENT_FLAGS_GROUP  nn_worker_flags;
static volatile int          nn_worker_ok;

/* The hand-over word (svc/nn_handoff.h) is the overlay's frame-path state
 * since issue #130 -- svc/nn_core_frame.c steps it, one critical section per
 * transition -- and this file reaches it through nn_overlay_take(),
 * nn_overlay_want() and nn_overlay_join(). */

static void nn_worker_entry(ULONG arg)
{
	ULONG got;

	(void)arg;

	for (;;) {
		(void)tx_event_flags_get(&nn_worker_flags, NN_WK_WAKE,
		                         TX_OR_CLEAR, &got, TX_WAIT_FOREVER);
		/* [!] A wake-up is not a job.  Only a hand-over this thread can
		 * TAKE is one; anything else was already taken or never was. */
		if (!nn_overlay_take())
			continue;

		/* Invoke, lease, geometry, decode, publish.  The input and the
		 * outputs are this thread's until the job's DONE, which the frame
		 * path says only after the decode has finished reading them: a
		 * stream asks for the next frame, a one-shot (`nn run`) wanted one
		 * and parks for good (issue #129). */
		nn_overlay_work();
		(void)tx_event_flags_set(&nn_worker_flags, NN_WK_SETTLED, TX_OR);
	}
}

void nn_worker_create_objects(void)
{
	if (tx_event_flags_create(&nn_worker_flags, "nn_worker") != TX_SUCCESS) {
		LOG_ERR("event flags not created; streams will be refused");
		return;
	}
	if (tx_thread_create(&nn_worker_thread, "nn_worker", nn_worker_entry, 0,
	                     nn_worker_stack, sizeof nn_worker_stack,
	                     NN_WORKER_PRIO, NN_WORKER_PRIO,
	                     TX_NO_TIME_SLICE, TX_AUTO_START) != TX_SUCCESS) {
		LOG_ERR("thread not created; streams will be refused");
		return;
	}
	nn_worker_ok = 1;
}

int nn_worker_ready(void)
{
	return nn_worker_ok;
}

int nn_worker_arm(void)
{
	if (!nn_worker_ok)
		return 0;
	return nn_overlay_want();
}

void nn_worker_wake(void)
{
	/* Only an armed word can have been handed over, and only a worker that
	 * exists can have armed it -- so this is a formality, kept so a wake-up
	 * can never touch flags that were not created. */
	if (nn_worker_ok)
		(void)tx_event_flags_set(&nn_worker_flags, NN_WK_WAKE, TX_OR);
}

int nn_worker_join(void)
{
	ULONG t0 = tx_time_get();
	ULONG got, spent;

	for (;;) {
		if (nn_overlay_join())
			return 0;
		/* Without the thread there is nothing that could ever settle the
		 * word, and no flags to wait on. */
		if (!nn_worker_ok)
			return -1;
		/* [!] WALL CLOCK, NOT A COUNT OF WAKE-UPS.  Stale SETTLED flags from
		 * earlier jobs return at once; counting them would run the deadline
		 * out against a worker that is behaving. */
		spent = tx_time_get() - t0;
		if (spent >= (ULONG)NN_WORKER_JOIN_TICKS)
			return -1;
		/* A SETTLED set between the JOIN above and this wait is still
		 * pending here, so the wake-up cannot be lost. */
		(void)tx_event_flags_get(&nn_worker_flags, NN_WK_SETTLED,
		                         TX_OR_CLEAR, &got,
		                         (ULONG)NN_WORKER_JOIN_TICKS - spent);
	}
}
