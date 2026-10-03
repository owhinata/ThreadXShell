/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    nn_stream_state.c
 * @brief   The live-inference teardown table (issues #99, #129).  See the
 *          header for why this is a pure function that nothing else lives
 *          beside.
 */
#include <stddef.h>   /* NULL */

#include "nn_stream_state.h"

int nn_stream_may_detach(int cam_rc)
{
	/*
	 * [!] EXACTLY ONE VALUE, ENUMERATED RATHER THAN EXCLUDED.  "Anything that
	 * is not an outright failure" would let CAM_ERR_LOCKED through -- which is
	 * the answer that says the producer was never asked -- and unlinking a sink
	 * a live producer may be inside is the failure the camera's lost-producer
	 * state exists to prevent.
	 */
	return (cam_rc == NN_STREAM_CAM_OK);
}

int nn_stream_may_join_worker(int cam_rc)
{
	/* The same single value as the detach, for the same reason one step
	 * earlier: a producer that may still be running may still hand over. */
	return (cam_rc == NN_STREAM_CAM_OK);
}

void nn_stream_stop_decide(int cam_rc, int worker_join, int detach_attempted,
                           int detach_rc, struct nn_stream_verdict *out)
{
	if (out == NULL)
		return;

	if (cam_rc != NN_STREAM_CAM_OK) {
		switch (cam_rc) {
		case NN_STREAM_CAM_LOCKED:
			/* Nothing was touched, and there is now a command that can ask
			   again on its own.  See the note in the header. */
			out->act = (unsigned char)NN_STREAM_ACT_RETRY;
			out->why = (unsigned char)NN_STREAM_WHY_CAM_LOCKED;
			return;
		case NN_STREAM_CAM_TIMEOUT:
			out->act = (unsigned char)NN_STREAM_ACT_TERMINAL;
			out->why = (unsigned char)NN_STREAM_WHY_CAM_LOST;
			return;
		case NN_STREAM_CAM_STATE:
		default:
			/* Fail closed: an unrecognised code is not evidence of
			   quiescence. */
			out->act = (unsigned char)NN_STREAM_ACT_TERMINAL;
			out->why = (unsigned char)NN_STREAM_WHY_CAM_STATE;
			return;
		}
	}

	/*
	 * The worker, between the producer and the panel (issue #129).  Reached
	 * only on a confirmed producer stop; above it the join is not attempted
	 * and whatever was passed is ignored, like a leftover detach code.
	 */
	switch (worker_join) {
	case NN_STREAM_WJOIN_NOT_TRIED:
		/* Stage 1: nothing arms the worker, so there is nothing to join and
		 * the verdict is the panel's alone.  See the header. */
		break;
	case NN_STREAM_WJOIN_OK:
		break;
	case NN_STREAM_WJOIN_FAILED:
	default:
		/* [!] Fail closed, whatever the detach said: a worker that may
		 * still be inside the NPU or the plugin is not quiescent, and an
		 * unrecognised join result is not evidence that it is. */
		out->act = (unsigned char)NN_STREAM_ACT_TERMINAL;
		out->why = (unsigned char)NN_STREAM_WHY_WORKER_LOST;
		return;
	}

	if (!detach_attempted) {
		/* The camera confirmed but the caller skipped the second half, so the
		   sink is still linked and a panel thread may still be in it.  A caller
		   bug, and the safe reading of one. */
		out->act = (unsigned char)NN_STREAM_ACT_TERMINAL;
		out->why = (unsigned char)NN_STREAM_WHY_SINK_LOST;
		return;
	}

	switch (detach_rc) {
	case NN_STREAM_CAM_OK:
		out->act = (unsigned char)NN_STREAM_ACT_DONE;
		out->why = (unsigned char)NN_STREAM_WHY_OK;
		return;
	case NN_STREAM_CAM_BUSY:
		/* The core says a transition or a callback of ours is still in flight
		   and to ask again; the sink is put back where it was.  Latching
		   terminal here would strand the panel until reboot for something that
		   clears by itself (issue #79). */
		out->act = (unsigned char)NN_STREAM_ACT_RETRY;
		out->why = (unsigned char)NN_STREAM_WHY_SINK_BUSY;
		return;
	case NN_STREAM_CAM_TIMEOUT:
	case NN_STREAM_CAM_STATE:
	default:
		out->act = (unsigned char)NN_STREAM_ACT_TERMINAL;
		out->why = (unsigned char)NN_STREAM_WHY_SINK_LOST;
		return;
	}
}
