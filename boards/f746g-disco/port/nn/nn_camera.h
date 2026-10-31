/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    nn_camera.h
 * @brief   Live camera -> NN inference glue (owhinata/stm32f746g-disco#81, Epic
 * owhinata/stm32f746g-disco#80).
 *
 * Bridges the camera frame pipeline (port/camera) to the nn inference API
 * (svc/nn/nn.h): a synchronous push sink resizes + converts an RGB565 camera
 * frame straight into the model's input tensor whenever the worker wants one,
 * and a low-priority (prio 18) best-effort worker thread runs inference on it.
 * Follows the nx_mjpeg.c eth_sink lifecycle (thread created once + parked) and
 * the codex-reviewed ownership rule -- the input the worker feeds to nn_run() is
 * never written by the sink -- which since issue #130 is the shared hand-over
 * word of svc/nn/nn_core_frame.h (it was two staging buffers and a copy).  Since Epic owhinata/stm32f746g-disco#99 Phase 1
 * (owhinata/stm32f746g-disco#100) nncam is a plain camera *subscriber*: `nn stream
 * start/stop` enable/disable it and it attaches to the base capture (`camera stream`)
 * only while the base runs. A base detach (stop / DCMI overrun / cascade) PAUSES it
 * (frames stop) but keeps it enabled and holding the nn session; it re-attaches when
 * the base restarts.
 *
 * Drives `nn stream start|stop` and `nn run`.  Model-specific detection decode
 * (BlazeFace anchors + NMS) is layered above in port/nn/models
 * (owhinata/stm32f746g-disco#81 task 8); with the `null` backend this loop runs
 * end-to-end with 0 detections.
 */
#ifndef NN_CAMERA_H
#define NN_CAMERA_H

#include <stdbool.h>
#include <stdint.h>

#include "camera.h"            /* enum camera_res */
#include "blazeface.h"  /* struct bf_det / bf_result -- the vocabulary, not the decoder */
#include "nn_det_record.h" /* struct nn_result_extra (issue #121) */
#include "nn_report.h"     /* struct nn_report_capture (issue #159) */
#include "nn_input_codes.h" /* the input refusals' values (issue #162) */

#ifdef __cplusplus
extern "C" {
#endif

struct nn_camera_stats {
	bool     running;
	uint8_t  res;          /**< enum camera_res of the active stream */
	uint32_t frames;       /**< frames written and handed to the worker */
	uint32_t drops;        /**< frames skipped: the worker was busy (#130) */
	uint32_t infers;       /**< inferences completed                   */
	uint32_t errors;       /**< nn_run() failures                      */
	uint32_t last_us;      /**< latency of the last inference (us)     */
	uint32_t fps_x100;     /**< average inference rate x100 since start */
	uint32_t detections;   /**< detections from the last inference (task 8) */
	/**
	 * [!] Frames since the logical `nn stream start`, which `frames` above is
	 * NOT (issue #99): that one is cleared on every pipeline attach, so it
	 * restarts when the base capture starts and again after an overrun
	 * re-attach.  A bounded run needs a count that only moves forwards.
	 */
	uint32_t gen_frames;
	uint32_t gen_infers;   /**< ...and the same for these three, or the four   */
	uint32_t gen_errors;   /**< numbers describe two different periods and do  */
	uint32_t gen_drops;    /**< not add up (issue #99).                        */
	/** Of @ref gen_errors: decodes a plugin refused as a wiring fault (issue
	 *  #130 step 6c) -- see nn_decode_count.h. */
	uint32_t gen_decoder_errors;
	/** Of @ref gen_errors: a loaded plugin's BF_ERR_MODEL -- the container's
	 *  model and decoder disagree (issue #159). */
	uint32_t gen_model_errors;
	/**
	 * How much of each thread's stack was already spent where a plugin is
	 * CALLED (issue #159; wio-lite-ai's since #108): the deepest depth sampled
	 * at the indirect call into the decode slot (the nn worker), the draw slot
	 * (the GUIX thread) and every console slot (entry, shapes_ok, report,
	 * params).  0 until a site has run.  decode / draw are reset per stream;
	 * the shell's is kept from boot -- its deepest site is a model load.
	 */
	uint32_t depth_decode;
	uint32_t depth_draw;
	uint32_t depth_shell;
};

/* The nn worker's stack.  Sized from the measured high-water-mark: `thread`
 * peak = 1808 B under the tflm backend (CMSIS-NN, BlazeFace pin) after `nn bench
 * 10`, two `nn run` and a 300-frame `nn stream` (issue #130, 2026-10-04); 4096
 * keeps ~2.3x margin.  (The earlier 1024 B was the stedgeai backend,
 * owhinata/stm32f746g-disco#93.)  Here so `nn stream stats` can print each
 * plugin call site's depth over the stack it is in (issue #159). */
#define NNCAM_WORKER_STACK    4096u

/** Which thread a plugin slot was entered on (issue #159). */
enum nn_camera_site {
	NNCAM_SITE_DECODE = 0,   /**< the nn worker   */
	NNCAM_SITE_DRAW   = 1,   /**< the GUIX thread */
	NNCAM_SITE_SHELL  = 2,   /**< a console       */
};

/**
 * Record that a plugin slot is about to be entered at stack pointer @p sp on the
 * calling thread (issue #159).  Keeps the high-water of (stack top - sp) per
 * site; a sample that is not inside the calling thread's stack -- not on a
 * thread, or a host build's 0 -- is dropped rather than recorded.
 */
void nn_camera_note_depth_at(enum nn_camera_site site, uintptr_t sp);

/**
 * The geometry of the frames the stream is attached to (issue #159): what a
 * plugin's to_frame() maps the model's normalised space into -- the producer
 * squashes the whole frame onto the input.  0 x 0 before the first attach.
 */
void nn_camera_frame_geom(uint16_t *w, uint16_t *h);

/*
 * nn_camera_start()'s refusals about the decoder (issue #159; the same three
 * questions, and the same values, as wio-lite-ai's).  Outside the -2..-10 the
 * start already returns, and apart from NN_ERR_SCRATCH_HELD (-50).  NODRAW is
 * also a stream with no plugin at all (Step 4c): nothing would draw.
 */
#define NNCAM_ERR_SHAPES  (-12) /**< the loaded decoder cannot read these outputs  */
#define NNCAM_ERR_NODRAW  (-13) /**< a stream was asked for; nothing draws          */
#define NNCAM_ERR_DECBUSY (-14) /**< the decoder could not be held still to ask it */

/*
 * nn_camera_start()'s refusals about the model input, one per reason of the
 * shared admission test (svc/nn/nn_input_contract.h, issue #162).  The values
 * and the table from reason to code are svc/nn/nn_input_codes.h's, shared with
 * wio-lite-ai (-11 and -18..-25 are free on this board too); the "no input"
 * reason is -3, as "no model" already is.
 */
#define NNCAM_ERR_MODEL       NN_INPUT_START_ERR_MODEL     /* -3  */
#define NNCAM_ERR_QUANT       NN_INPUT_START_ERR_QUANT     /* -11 */
#define NNCAM_ERR_IN_DTYPE    NN_INPUT_START_ERR_DTYPE     /* -18 */
#define NNCAM_ERR_IN_RANK     NN_INPUT_START_ERR_RANK      /* -19 */
#define NNCAM_ERR_IN_BATCH    NN_INPUT_START_ERR_BATCH     /* -20 */
#define NNCAM_ERR_IN_EMPTY    NN_INPUT_START_ERR_EMPTY     /* -21 */
#define NNCAM_ERR_IN_CHANNELS NN_INPUT_START_ERR_CHANNELS  /* -22 */
#define NNCAM_ERR_IN_ZP       NN_INPUT_START_ERR_ZP        /* -23 */
#define NNCAM_ERR_IN_ALIGN    NN_INPUT_START_ERR_ALIGN     /* -24 */
#define NNCAM_ERR_IN_BYTES    NN_INPUT_START_ERR_BYTES     /* -25 */

/** The nn step of the camera sink (port/camera/cam_sink.h, issue #142).
 *  Defined in nn_camera.c; src/main.c registers it after the display step. */
struct cam_sink_step;
extern struct cam_sink_step nncam_step;

/**
 * Enable live camera inference (`nn stream start`).  @p res is a display hint
 * only -- the input adapts to whatever geometry the base capture publishes
 * (owhinata/stm32f746g-disco#100).  Claims the single nn session (refused -6 if `ai
 * bench`/another stream holds it) and enables the nn step of the camera sink: it is
 * armed immediately if the base is already running, otherwise it stays enabled +
 * idle and is armed at the next `camera stream start`.  Non-blocking.
 * Returns 0 or <0 (-2 already running / still tearing down / a teardown owns the
 * sink, -3 model, NNCAM_ERR_QUANT / NNCAM_ERR_IN_* the model input (issue #162),
 * -5 objects, -6 nn session busy, -9 the camera is
 * not streaming RGB565 / its mode cannot stream RGB565 (issue #142 D3), -10 the
 * camera could not be held or started, NN_ERR_SCRATCH_HELD `sdram test` holds
 * .sdram.ai -- retryable, issue #152; NNCAM_ERR_SHAPES / _NODRAW / _DECBUSY,
 * issue #159 -- _NODRAW for a stream with no plugin too).  Holds the camera open (issue #142): joins
 * a running stream, or starts it holding the operator's place too.
 * @p oneshot is non-zero for `nn run`'s session, whose decodes are not counted
 * in the stream's errors (issue #130 step 6c).
 */
int  nn_camera_start(enum camera_res res, int oneshot);

/** Disable live inference (`nn stream stop`): disable the nn step of the camera
 *  sink (the base keeps running), wait for the producer to leave the step, and
 *  release the nn session after the worker parks.
 *  Bounded waits (wall clock).  Returns:
 *    0   stopped;
 *   -1   not running;
 *   -2   the worker is still mid-inference -- it releases the session as it
 *        exits, and the sink is already released, so this is not a refusal;
 *   -7   the sink did not hand its frame back (issue #72).  A producer callback
 *        may still be preprocessing into the input tensor, so `nn stream
 *        start` is refused until a later stop re-polls and finds it clear.
 *        Retryable by design: if the callback never returns, every retry keeps
 *        refusing, which is what a terminal state would have given anyway;
 *   -8   a start or another stop owns the lifecycle -- nothing was done. */
int  nn_camera_stop(void);

/** True while inference is enabled. */
bool nn_camera_running(void);

/** True while an `nn stream` runs -- not a `nn run` (issue #142): what the
 *  preview draws boxes for.  The record and the report do not depend on it. */
bool nn_camera_overlay(void);

/** Snapshot current stats (any time). */
void nn_camera_stats_get(struct nn_camera_stats *out);

/**
 * One decode's boxes and the diagnostics that belong to them.
 *
 * [!] READ TOGETHER OR NOT AT ALL (issue #97).  The worker publishes both under
 * one lock.  `nn stream stats` does not decode anything itself -- it reports what
 * the worker last published -- so fetching the boxes and then asking the decoder
 * for its "last" numbers paired this frame's boxes with whatever had been decoded
 * by the time it got round to printing.
 */
struct nn_camera_decode {
	/** 0 = nothing decoded since the model went in.  [!] NOT "this session":
	 *  the result outlives a stop (issue #118). */
	int              valid;
	int              ndet;   /**< faces, or a negative BF_ERR_* code          */
	struct bf_result res;    /**< status, peak, pass/kept, threshold APPLIED  */
	/* Carried from the record in the same snapshot (issue #118) -- see
	 * svc/nn/nn_det_record.h for each. */
	uint8_t          current;   /**< published in the session in force        */
	uint32_t         accepted;  /**< publishes the record has ever taken      */
	uint32_t         epoch;     /**< moves when a model change clears it      */
	/** Whose result it is (issue #159): the raw tensors nobody interpreted,
	 *  or a plugin's -- which keeps its own result and is asked to report it. */
	uint8_t          kind;
	uint8_t          reportable;  /**< see nn_det_record.h                   */
};

/**
 * The model changed: clear the last result (issue #118).  Called by the load
 * and the unload under the nn session, before the session is given back.
 */
void nn_camera_record_invalidate(void);

/**
 * Take a coherent snapshot of the last published decode.
 *
 * @param dets  optional; the boxes, up to @p max of them
 * @param rep   optional; with a plugin in force, its account of THIS result,
 *              taken under the plugin lease in the same hold as the snapshot
 *              (issue #159; wio-lite-ai's since #110).  Left alone otherwise:
 *              the caller states NN_REPORT_NONE.
 * @param ext   optional; the classes the result was published with, for a
 *              model the decoder did not recognise (issue #121)
 * @return non-zero if a snapshot was taken (zero before the first attach, when
 *         the lock does not exist yet).  `valid == 0` means nothing has been
 *         decoded since the model went in -- which is NOT a decode that found
 *         nothing, and must not be printed as one.
 */
int nn_camera_decode_get(struct nn_camera_decode *out, struct bf_det *dets,
                         int max, struct nn_report_capture *rep,
                         struct nn_result_extra *ext);

/** Runtime input normalization: 1 = [-1,1], 0 = [0,1] (tuning, no reflash); float32
 *  and, before its scale and zero point, int8 (issue #162). */
void nn_camera_set_norm(int signed_range);
int  nn_camera_get_norm(void);

#ifdef __cplusplus
}
#endif

#endif /* NN_CAMERA_H */
