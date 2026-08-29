/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    nn_stream_state.h
 * @brief   How this board's live-inference teardown is classified (issue #99).
 *
 * `nn stream stop` runs two halves -- stop the camera producer, then unlink the
 * panel sink -- and each can come back unconfirmed for reasons that call for
 * DIFFERENT things.  Getting that wrong is expensive in both directions: told
 * "reboot" for a momentary lock collision, an operator power-cycles a board that
 * a second stop would have fixed; told "retry" for a producer that never came
 * back, they retry for ever while a thread is still inside the sink.
 *
 * WHY IT IS A PURE FUNCTION IN ITS OWN FILE.  None of the interesting vectors
 * can be produced from a console.  This board has ONE console, its background
 * jobs run below the foreground shell under TX_NO_TIME_SLICE, and the inputs
 * that matter -- a stop that loses the API mutex, a detach that finds a callback
 * still in flight -- are microsecond windows inside another thread.  So the
 * table is separated from the code that acts on it and a host test walks every
 * entry, exactly as port/camera/cam_state.c already does for the camera's own
 * stop decision.
 *
 * [!] THE CODES BELOW ARE MIRRORED, NOT INCLUDED, and that is deliberate: this
 * file is compiled on the host with no board on the include path (camera.h
 * pulls in the whole datapath).  port/npu/nn_svc_grove.c static-asserts each one
 * against the camera's own definition, so a drift is a build failure on the
 * board rather than a table that quietly decides about numbers nobody returns.
 */
#ifndef NN_STREAM_STATE_H
#define NN_STREAM_STATE_H

#ifdef __cplusplus
extern "C" {
#endif

/* Mirrors of camera.h -- see the note above. */
#define NN_STREAM_CAM_OK        0
#define NN_STREAM_CAM_TIMEOUT (-3)
#define NN_STREAM_CAM_STATE   (-4)
#define NN_STREAM_CAM_BUSY    (-7)
#define NN_STREAM_CAM_LOCKED  (-8)

/** What the caller must do about the transient `nn` claim. */
enum nn_stream_act {
	/** Both halves confirmed.  Release it, exactly once. */
	NN_STREAM_ACT_DONE = 0,
	/**
	 * Keep it, and a later stop can still finish the job.
	 *
	 * [!] THE LIFECYCLE MUST BE LEFT CLAIMABLE AGAIN when this is returned.
	 * Parking it mid-transition would turn one moment of contention into a
	 * stream nothing can ever tear down -- the precise outcome this answer
	 * exists to avoid.
	 */
	NN_STREAM_ACT_RETRY,
	/** Keep it until a reset.  A thread may still be inside the sink, and
	 *  releasing would let a later unload dismantle an interpreter underneath
	 *  it. */
	NN_STREAM_ACT_TERMINAL,
};

/** Which sentence the operator gets.  They are not interchangeable: the first
 *  two mean "nothing was touched" and "something is still running". */
enum nn_stream_why {
	NN_STREAM_WHY_OK = 0,
	/** The camera API stayed locked, so the producer was never even asked and
	 *  nothing was touched.  Retryable ONLY because issue #99 introduced a
	 *  `nn stream stop` that can be issued on its own; before it, the sole
	 *  caller was the command that owned the sink, which is why the camera's
	 *  own header prescribed a reboot here. */
	NN_STREAM_WHY_CAM_LOCKED,
	/** The producer was asked and never acknowledged.  It is still running
	 *  somewhere and the camera has poisoned itself. */
	NN_STREAM_WHY_CAM_LOST,
	/** The camera refused: already poisoned, or a state this cannot stop. */
	NN_STREAM_WHY_CAM_STATE,
	/** The unlink found a transition or a callback still in flight.  The sink
	 *  is put back where it was, so repeating the stop is what settles it. */
	NN_STREAM_WHY_SINK_BUSY,
	/** The panel thread did not come back, or the unlink was refused for
	 *  good.  The sink is latched lost. */
	NN_STREAM_WHY_SINK_LOST,
};

struct nn_stream_verdict {
	unsigned char act;  /**< enum nn_stream_act  */
	unsigned char why;  /**< enum nn_stream_why  */
};

/**
 * May the sink be unlinked, given what the camera stop returned?
 *
 * [!] ONLY ON A CONFIRMED STOP.  This is camera.h's rule and the reason the
 * whole lost-producer state exists: anything but success means the producer may
 * still be inside consume(), and unlinking there is what the state was invented
 * to prevent.  Kept separate from the verdict so a host test can show that
 * widening it -- "not running is close enough" -- fails.
 *
 * @return non-zero when the detach may be attempted
 */
int nn_stream_may_detach(int cam_rc);

/**
 * Classify one whole teardown.
 *
 * @param cam_rc            what camera_stream_stop() returned
 * @param detach_attempted  whether the detach was run at all
 * @param detach_rc         what it returned; ignored unless attempted
 *
 * Unknown codes fail closed to terminal on both halves: a board that cannot say
 * whether an asynchronously used resource is quiescent must not guess.
 */
void nn_stream_stop_decide(int cam_rc, int detach_attempted, int detach_rc,
                           struct nn_stream_verdict *out);

#ifdef __cplusplus
}
#endif

#endif /* NN_STREAM_STATE_H */
