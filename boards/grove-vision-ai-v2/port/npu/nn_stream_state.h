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
	/** The producer stopped, but the inference worker did not come back
	 *  within its deadline (issue #129).  It may still be inside the NPU or
	 *  the plugin, so nothing is unlinked or released. */
	NN_STREAM_WHY_WORKER_LOST,
};

/**
 * What the join of the inference worker came to (issue #129).  The stop joins
 * the worker BETWEEN the producer and the detach: after a confirmed producer
 * stop, before the record boundary and the unlink.
 */
enum nn_stream_wjoin {
	/**
	 * Not attempted.  The only right answer when the producer was not
	 * confirmed out (nn_stream_may_join_worker() says so): with a producer
	 * still running, nothing the worker is doing is stable.
	 *
	 * [!] ON A CONFIRMED PRODUCER STOP IT IS TERMINAL, "worker did not
	 * return".  The stop joins the worker whenever it may, so a confirmed
	 * producer with the join skipped is a caller bug, and the safe reading of
	 * one is that the worker may still be inside the NPU or the plugin -- the
	 * same reading as the detach skipped below.
	 */
	NN_STREAM_WJOIN_NOT_TRIED = 0,
	/** The worker was parked with nothing handed over or running. */
	NN_STREAM_WJOIN_OK,
	/** The deadline passed with a job still handed over or running. */
	NN_STREAM_WJOIN_FAILED,
};

struct nn_stream_verdict {
	unsigned char act;  /**< enum nn_stream_act  */
	unsigned char why;  /**< enum nn_stream_why  */
};

/**
 * May the record boundary be taken and the sink be unlinked, given what the
 * camera stop and the worker join returned?
 *
 * [!] ONLY ON A CONFIRMED STOP OF BOTH.  The producer half is camera.h's rule
 * and the reason the whole lost-producer state exists: anything but success
 * means the producer may still be inside consume(), and unlinking there is what
 * the state was invented to prevent.  The worker half is issue #129's: a worker
 * that did not come back may still publish, so a boundary taken now would let
 * its decode land in the next session.  Kept separate from the verdict so a
 * host test can show that widening either -- "not running is close enough" --
 * fails.
 *
 * @param cam_rc       what camera_stream_stop() returned
 * @param worker_join  enum nn_stream_wjoin
 * @return non-zero when the boundary and the detach may be attempted
 */
int nn_stream_may_detach(int cam_rc, int worker_join);

/**
 * May the inference worker be joined, given what the camera stop returned?
 * Only on a confirmed producer stop: before it, the producer can still hand the
 * worker a new job, so "parked" would not stay true (issue #129).
 *
 * @return non-zero when the join may be attempted
 */
int nn_stream_may_join_worker(int cam_rc);

/**
 * Classify one whole teardown.
 *
 * @param cam_rc            what camera_stream_stop() returned
 * @param worker_join       enum nn_stream_wjoin: what the worker join came to;
 *                          ignored unless the producer stop was confirmed
 * @param detach_attempted  whether the detach was run at all
 * @param detach_rc         what it returned; ignored unless attempted
 *
 * Unknown codes fail closed to terminal on every part: a board that cannot say
 * whether an asynchronously used resource is quiescent must not guess.
 */
void nn_stream_stop_decide(int cam_rc, int worker_join, int detach_attempted,
                           int detach_rc, struct nn_stream_verdict *out);

#ifdef __cplusplus
}
#endif

#endif /* NN_STREAM_STATE_H */
