/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    nn_stream_state.c
 * @brief   The live-inference teardown table (issues #99, #129, #142).  See the
 *          header for why this is a pure function that nothing else lives
 *          beside.
 */
#include <stddef.h>   /* NULL */

#include "nn_stream_state.h"

int nn_stream_may_join_worker(int ov_rc)
{
	/*
	 * [!] EXACTLY ONE VALUE, ENUMERATED RATHER THAN EXCLUDED.  A timeout is
	 * "the producer or the panel was still counted inside nn's step", and a
	 * worker joined then could be handed a new job the moment this returns.
	 */
	return (ov_rc == NN_STREAM_CAM_OK);
}

int nn_stream_may_release(int ov_rc, int worker_join)
{
	/* The overlay AND the worker, each by its one success value (issues
	 * #129, #142): not "not tried", not "anything but a failure". */
	return (ov_rc == NN_STREAM_CAM_OK && worker_join == NN_STREAM_WJOIN_OK);
}

void nn_stream_stop_decide(int ov_rc, int worker_join, int leave_attempted,
                           int leave_rc, struct nn_stream_verdict *out)
{
	if (out == NULL)
		return;

	if (ov_rc != NN_STREAM_CAM_OK) {
		switch (ov_rc) {
		case NN_STREAM_CAM_TIMEOUT:
			/* The overlay is off and stays off; what is left is a count
			   that had not reached zero by the deadline -- the panel's
			   blit is bounded by its own DMA timeout, so a second stop
			   waits again (issue #142, the step rule: a drain that times
			   out keeps the claim and is retried, never terminal). */
			out->act = (unsigned char)NN_STREAM_ACT_RETRY;
			out->why = (unsigned char)NN_STREAM_WHY_OV_BUSY;
			return;
		case NN_STREAM_CAM_STATE:
		default:
			/* Fail closed: the sink is lost or the camera ended
			   unconfirmed, or a code nobody documented -- none of it is
			   evidence that nothing is inside the overlay. */
			out->act = (unsigned char)NN_STREAM_ACT_TERMINAL;
			out->why = (unsigned char)NN_STREAM_WHY_SINK_LOST;
			return;
		}
	}

	/*
	 * The worker, between the overlay and the leave (issue #129).  Reached
	 * only behind a proven overlay; above it the join is not attempted and
	 * whatever was passed is ignored, like a leftover leave code.
	 */
	switch (worker_join) {
	case NN_STREAM_WJOIN_OK:
		break;
	case NN_STREAM_WJOIN_NOT_TRIED:
	case NN_STREAM_WJOIN_FAILED:
	default:
		/* [!] Fail closed, whatever the leave said: a worker that may
		 * still be inside the NPU or the plugin is not quiescent, a join
		 * the caller skipped is not evidence that it is, and neither is an
		 * unrecognised join result. */
		out->act = (unsigned char)NN_STREAM_ACT_TERMINAL;
		out->why = (unsigned char)NN_STREAM_WHY_WORKER_LOST;
		return;
	}

	if (!leave_attempted) {
		/* Both proven but the caller skipped the leave: nn still holds the
		   camera, and the claim must not go back with it held.  A caller
		   bug, and the safe reading of one. */
		out->act = (unsigned char)NN_STREAM_ACT_TERMINAL;
		out->why = (unsigned char)NN_STREAM_WHY_CAM_STATE;
		return;
	}

	switch (leave_rc) {
	case NN_STREAM_CAM_OK:
		out->act = (unsigned char)NN_STREAM_ACT_DONE;
		out->why = (unsigned char)NN_STREAM_WHY_OK;
		return;
	case NN_STREAM_CAM_BUSY:
		/* A camera start or stop held the holder lock past the wait, or the
		   last place's unlink found a transition in flight and put the
		   sink back (issue #79).  Latching terminal here would strand the
		   panel until reboot for something that clears by itself. */
		out->act = (unsigned char)NN_STREAM_ACT_RETRY;
		out->why = (unsigned char)NN_STREAM_WHY_SINK_BUSY;
		return;
	case NN_STREAM_CAM_LOCKED:
		/* The last place's camera stop never got the API: nothing was
		   asked and nothing touched. */
		out->act = (unsigned char)NN_STREAM_ACT_RETRY;
		out->why = (unsigned char)NN_STREAM_WHY_CAM_LOCKED;
		return;
	case NN_STREAM_CAM_TIMEOUT:
		out->act = (unsigned char)NN_STREAM_ACT_TERMINAL;
		out->why = (unsigned char)NN_STREAM_WHY_CAM_LOST;
		return;
	case NN_STREAM_CAM_STATE:
	default:
		out->act = (unsigned char)NN_STREAM_ACT_TERMINAL;
		out->why = (unsigned char)NN_STREAM_WHY_CAM_STATE;
		return;
	}
}
