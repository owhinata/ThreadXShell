/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    nn_stream_state.h
 * @brief   How this board's live-inference teardown is classified (issues
 *          #99, #129, #142).
 *
 * Since issue #142 the camera does not stop when nn does: the operator holds
 * the stream too (svc/camera/cam_holders.h), so `nn stream stop` -- and the end of
 * `nn run` -- takes nn's overlay OFF the panel sink, waits until nothing uses
 * it, joins the inference worker and lets go of nn's place among the camera's
 * holders.  Each step can come back unconfirmed for reasons that call for
 * DIFFERENT things.  Getting that wrong is expensive in both directions: told
 * "reboot" for a momentary collision, an operator power-cycles a board that a
 * second stop would have fixed; told "retry" for a thread that never came back,
 * they retry for ever while it is still inside the overlay.
 *
 * [!] TWO PROOFS, AND THEY PERMIT DIFFERENT THINGS.
 *   - nn's overlay count reaching zero (cam_lcd_sink_overlay_remove()) says the
 *     producer is out of nn's step and the panel thread out of its draw, and
 *     can never enter again.  It permits the worker JOIN, and -- with the
 *     worker joined -- the record boundary and nn's leave.  It permits NO
 *     unlink of the panel sink: the camera is still streaming.
 *   - Only a CONFIRMED CAMERA STOP permits the sink's unlink.  That is the
 *     camera side's table (port/camera/cam_hold_rules.h), reached here only
 *     through the leave, when nn's was the last place on the camera; its
 *     answer arrives as the leave's result.
 *
 * WHY IT IS A PURE FUNCTION IN ITS OWN FILE.  None of the interesting vectors
 * can be produced from a console.  This board has ONE console, its background
 * jobs run below the foreground shell under TX_NO_TIME_SLICE, and the inputs
 * that matter -- a panel thread that holds the overlay past a deadline, a leave
 * that loses the holder lock -- are windows inside another thread.  So the
 * table is separated from the code that acts on it and a host test walks every
 * entry, exactly as port/camera/cam_state.c already does for the camera's own
 * stop decision.
 *
 * [!] THE CODES BELOW ARE MIRRORED, NOT INCLUDED, and that is deliberate: this
 * file is compiled on the host with no board on the include path (camera.h
 * pulls in the whole datapath).  port/nn/nn_svc_grove.c static-asserts each one
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
	/** Every step confirmed.  Release it, exactly once. */
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
	/** Keep it until a reset.  A thread may still be inside the overlay or
	 *  the NPU, and releasing would let a later unload dismantle an
	 *  interpreter underneath it. */
	NN_STREAM_ACT_TERMINAL,
};

/** Which sentence the operator gets.  They are not interchangeable: some mean
 *  "nothing was touched, ask again" and others "something is still running". */
enum nn_stream_why {
	NN_STREAM_WHY_OK = 0,
	/** The leave was nn's last place on the camera, and the camera API stayed
	 *  locked: the producer was never asked and nothing was touched. */
	NN_STREAM_WHY_CAM_LOCKED,
	/** The leave stopped the camera and the producer never acknowledged.  It
	 *  is still running somewhere and the camera has poisoned itself. */
	NN_STREAM_WHY_CAM_LOST,
	/** The camera, or the holder code, refused for good: a stream that ended
	 *  without a confirmed stop, or a sink that could not be unlinked. */
	NN_STREAM_WHY_CAM_STATE,
	/** The leave found a camera start or stop in progress (the holder lock
	 *  was not had in time, or an unlink asked to be called again).  Nothing
	 *  of nn's was given back to it; repeating the stop settles it. */
	NN_STREAM_WHY_SINK_BUSY,
	/** The overlay could not be shown unused: the panel sink is lost, or the
	 *  camera already ended without a confirmed stop.  Kept for good. */
	NN_STREAM_WHY_SINK_LOST,
	/** The worker did not come back within its deadline (issue #129).  It
	 *  may still be inside the NPU or the plugin, so nothing is released. */
	NN_STREAM_WHY_WORKER_LOST,
	/** The overlay was taken off, but the producer or the panel thread was
	 *  still counted inside it at the deadline (issue #142).  Taking it off
	 *  is done and stays done; repeating the stop waits again. */
	NN_STREAM_WHY_OV_BUSY,
};

/**
 * What the join of the inference worker came to (issue #129).  The stop joins
 * the worker once the overlay is shown unused, and before the record boundary
 * and the leave.
 */
enum nn_stream_wjoin {
	/**
	 * Not attempted.  The only right answer when the overlay was not shown
	 * unused (nn_stream_may_join_worker() says so): with the producer
	 * possibly still in nn's step, nothing the worker is doing is stable.
	 *
	 * [!] BEHIND A PROVEN OVERLAY IT IS TERMINAL, "worker did not return".
	 * The stop joins the worker whenever it may, so a proven overlay with the
	 * join skipped is a caller bug, and the safe reading of one is that the
	 * worker may still be inside the NPU or the plugin.
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
 * May the inference worker be joined, given what taking the overlay off
 * returned?  Only once it is shown unused: before that the producer may still
 * be inside nn's step and hand the worker a new job, so "parked" would not stay
 * true (issue #129).
 *
 * @param ov_rc  what cam_lcd_sink_overlay_remove() returned
 * @return non-zero when the join may be attempted
 */
int nn_stream_may_join_worker(int ov_rc);

/**
 * May the record boundary be taken and nn's place on the camera be given up?
 *
 * [!] ONLY WITH BOTH THE OVERLAY SHOWN UNUSED AND THE WORKER JOINED.  The
 * overlay half is the producer and the panel being out of nn's step for good;
 * the worker half is issue #129's: a worker that did not come back may still
 * publish, so a boundary taken now would let its decode land in the next
 * session.  And nn's place is given up only after both, so a later `camera
 * stream stop` -- which needs nn gone -- can never stop the camera while either
 * is still inside.  Kept separate from the verdict so a host test can show that
 * widening either -- "not running is close enough" -- fails.
 *
 * @param ov_rc        what cam_lcd_sink_overlay_remove() returned
 * @param worker_join  enum nn_stream_wjoin
 */
int nn_stream_may_release(int ov_rc, int worker_join);

/**
 * Classify one whole teardown.
 *
 * @param ov_rc            what taking the overlay off returned
 * @param worker_join      enum nn_stream_wjoin; ignored unless the overlay
 *                         was shown unused
 * @param leave_attempted  whether nn's leave was run at all
 * @param leave_rc         what cam_lcd_sink_release() returned; ignored
 *                         unless attempted
 *
 * Unknown codes fail closed to terminal on every part: a board that cannot say
 * whether an asynchronously used resource is quiescent must not guess.
 */
void nn_stream_stop_decide(int ov_rc, int worker_join, int leave_attempted,
                           int leave_rc, struct nn_stream_verdict *out);

#ifdef __cplusplus
}
#endif

#endif /* NN_STREAM_STATE_H */
