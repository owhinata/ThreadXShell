/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    nn_svc_f746.c
 * @brief   This board behind the shared `nn` command's contract (issue #50).
 *
 * svc/nn_svc.h is what shell/cmds/cmd_nn.c speaks; port/nn is what this board
 * has.  This is the translation, and it is a THIN one: unlike the Grove adapter,
 * it introduces no state at all.
 *
 * [!] IT WRAPS THE EXISTING OWNERS AND ADDS NO SECOND COPY.  The model
 * singleton and the session gate already live in port/nn/nn.c, and the stream
 * worker already records its own release authority in port/nn/nn_camera.c.  A
 * model handle or a session flag kept here as well would be a second answer to a
 * question that already has one, and the two would drift the first time either
 * side changed.  Everything below therefore queries; nothing below remembers.
 *
 * [!] AND THE DIAGNOSTICS GO THROUGH A DETAIL BUFFER.  A port adapter cannot
 * take a shell instance, so the sentences this board used to print at the point
 * of failure are formatted here and printed by the shared command through
 * nn_svc_strerror().
 */
#include "nn_svc.h"

#include <stdarg.h>
#include <string.h>

#include "camera.h"
#include "fmt.h"
#include "nn.h"
#include "nn_camera.h"
#include "nn_decoder.h"
#include "sdram.h"
#include "tx_api.h"

/* The one thing this file remembers, and it is not state about the model: it is
   the last failure's words, so that a status code does not have to carry them. */
static char nn_detail[160];

static void nn_detail_clear(void)
{
	nn_detail[0] = '\0';
}

static void nn_detail_set(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	(void)fmt_vsnformat(nn_detail, sizeof nn_detail, fmt, ap);
	va_end(ap);
}

static void nn_result(struct nn_op_result *res, int status, enum nn_claim claim)
{
	res->status = status;
	res->claim  = (uint8_t)claim;
}

const char *nn_svc_strerror(int status)
{
	(void)status;
	return (nn_detail[0] != '\0') ? nn_detail : NULL;
}

/*
 * A stop's return, as cleanup authority.
 *
 * [!] -7 AND -8 ARE NOT FAILURES TO REPORT AND FORGET (issue #72).  -7 means the
 * sink is detached but still pinned, so a producer callback may still be writing
 * the staging buffers; -8 means another start/stop owns the transition.  In both
 * the claim is still out, the release is idempotent and either the worker or a
 * later stop settles it -- so this is RETRYABLE, and the next start being
 * refused has to have been said out loud once already.
 */
static enum nn_claim nn_claim_of_stop(int stop_rc)
{
	if (stop_rc == 0 || stop_rc == -1)   /* stopped, or was not running */
		return NN_CLAIM_NONE;
	if (stop_rc == -7 || stop_rc == -8)
		return NN_CLAIM_RETRYABLE;
	return NN_CLAIM_RETRYABLE;           /* a timeout settles the same way */
}

/* ---- info ---------------------------------------------------------------- */

void nn_svc_info(struct nn_svc_info *out)
{
	const struct nn_backend_info *bi = nn_backend();
	struct nn_model *m = NULL;

	out->backend = bi ? bi->name : NULL;

	if (nn_model_open(&m) != 0 || m == NULL) {
		out->model_active = 0u;
		out->model        = NULL;
		out->source       = NULL;
		out->arena_bytes  = 0u;
		return;
	}
	out->model_active = 1u;
	out->model        = nn_model_name(m);
	out->arena_bytes  = nn_activations_bytes(m);
	out->arena_used   = 0u;    /* this backend reports only the reservation */

	/* [!] Say plainly when nothing is being inferred, so a latency from
	 * `nn bench` is never mistaken for a model's. */
	out->source = (bi && strcmp(bi->name, "null") == 0)
	                  ? "synthetic workload, not inference"
	                  : NULL;
}

/* ---- model lifecycle ----------------------------------------------------- */

void nn_svc_model_load(const struct nn_spec *spec, nn_svc_read_fn read,
                       void *ctx, struct nn_op_result *res,
                       enum nn_model_state *state)
{
	struct nn_model *m = NULL;
	void *buf = NULL;
	uint32_t cap = 0u, len = 0u;
	int rc;

	nn_detail_clear();
	*state = (nn_model_open(&m) == 0 && m != NULL) ? NN_MODEL_PREVIOUS
	                                              : NN_MODEL_EMPTY;

	/* The tag is refused before anything is acquired. */
	if (spec->tag != NN_SPEC_PATH && spec->tag != NN_SPEC_BUILTIN) {
		nn_detail_set("this board loads a model from the SD card (--path) or "
		              "uses the one built into the image (builtin); it has no "
		              "%s",
		              spec->tag == NN_SPEC_NAME ? "asset store"
		              : spec->tag == NN_SPEC_SLOT ? "slot index"
		              : spec->tag == NN_SPEC_ADDR ? "raw model window"
		                                          : "such source");
		nn_result(res, NN_SVC_ERR_SPEC, NN_CLAIM_NONE);
		return;
	}

	if (nn_camera_running()) {
		nn_detail_set("stop the inference stream first (`nn stream stop`)");
		nn_result(res, NN_SVC_ERR_BUSY, NN_CLAIM_NONE);
		return;
	}
	if (!sdram_is_up()) {
		nn_detail_set("SDRAM is not up, and the model buffer lives there");
		nn_result(res, NN_SVC_ERR_STATE, NN_CLAIM_NONE);
		return;
	}
	if (nn_model_open(&m) != 0 || m == NULL) {
		nn_detail_set("the model could not be opened");
		nn_result(res, NN_SVC_ERR_STATE, NN_CLAIM_NONE);
		return;
	}

	/*
	 * [!] THE SESSION IS CLAIMED FIRST, so slot selection, the SD read and the
	 * swap are atomic against another console: otherwise a second shell could
	 * flip the chosen (inactive) slot to active between load_region() and the
	 * read, and the live flatbuffer would be corrupted.
	 */
	if (nn_session_try_acquire() != 0) {
		nn_detail_set("NN busy (a stream, run or bench is active)");
		nn_result(res, NN_SVC_ERR_BUSY, NN_CLAIM_NONE);
		return;
	}

	if (spec->tag == NN_SPEC_BUILTIN) {
		rc = nn_model_reload(NULL, 0u, NULL);
	} else {
		rc = nn_model_load_region(&buf, &cap);
		if (rc != 0) {
			nn_detail_set("this backend has no runtime model loader");
			nn_session_release();
			nn_result(res, NN_SVC_ERR_NOSUP, NN_CLAIM_NONE);
			return;
		}
		if (read == NULL) {
			nn_detail_set("no filesystem reader was supplied for --path");
			nn_session_release();
			nn_result(res, NN_SVC_ERR_NOSUP, NN_CLAIM_NONE);
			return;
		}
		/* The reader prints its own failure: it is the board's shell-layer
		   file and it has the console this command came in on. */
		if (read(ctx, spec->path, buf, cap, &len) != 0) {
			nn_detail_clear();
			nn_session_release();
			nn_result(res, NN_SVC_ERR_ARG, NN_CLAIM_NONE);
			return;
		}
		rc = nn_model_reload(buf, len, spec->path);
	}

	nn_session_release();

	/*
	 * [!] THE RESULTING MODEL STATE IS READ BACK, NOT ASSUMED.  This backend's
	 * reload is transactional -- on a refusal the previous model normally stays
	 * active -- but it documents one exception: if even that could not be
	 * rebuilt, the model is left CLOSED.  Reporting "rejected, previous
	 * unchanged" there would leave an operator believing a model is loaded
	 * when none is, so the answer comes from asking.
	 */
	m = NULL;
	if (nn_model_open(&m) == 0 && m != NULL)
		*state = (rc == 0) ? NN_MODEL_NEW : NN_MODEL_PREVIOUS;
	else
		*state = NN_MODEL_EMPTY;

	if (rc != 0) {
		nn_detail_set("the model was refused (%d)", rc);
		nn_result(res, NN_SVC_ERR_ARG, NN_CLAIM_NONE);
		return;
	}
	nn_result(res, NN_SVC_OK, NN_CLAIM_NONE);
}

void nn_svc_model_unload(struct nn_op_result *res)
{
	struct nn_model *m = NULL;

	nn_detail_clear();

	if (nn_camera_running()) {
		nn_detail_set("stop the inference stream first (`nn stream stop`)");
		nn_result(res, NN_SVC_ERR_BUSY, NN_CLAIM_NONE);
		return;
	}
	if (nn_session_try_acquire() != 0) {
		nn_detail_set("NN busy (a stream, run or bench is active)");
		nn_result(res, NN_SVC_ERR_BUSY, NN_CLAIM_NONE);
		return;
	}
	/* Idempotent: this board's model is a singleton that is rebuilt rather than
	   destroyed, so "unload" returns it to the built-in one. */
	if (nn_model_open(&m) == 0 && m != NULL)
		(void)nn_model_reload(NULL, 0u, NULL);
	nn_session_release();
	nn_result(res, NN_SVC_OK, NN_CLAIM_NONE);
}

/* ---- tensors ------------------------------------------------------------- */

int nn_svc_tensors_pin(void)
{
	struct nn_model *m = NULL;

	if (nn_model_open(&m) != 0 || m == NULL)
		return NN_SVC_ERR_STATE;
	if (nn_session_try_acquire() != 0)
		return NN_SVC_ERR_BUSY;
	return NN_SVC_OK;
}

void nn_svc_tensors_unpin(void)
{
	nn_session_release();
}

int nn_svc_output_count(void)
{
	struct nn_model *m = NULL;

	if (nn_model_open(&m) != 0 || m == NULL)
		return NN_SVC_ERR_STATE;
	return nn_output_count(m);
}

int nn_svc_output(unsigned index, struct tensor_desc *out)
{
	struct nn_model *m = NULL;
	struct nn_tensor *t;

	if (nn_model_open(&m) != 0 || m == NULL)
		return NN_SVC_ERR_STATE;
	t = nn_output(m, (int)index);
	if (t == NULL)
		return NN_SVC_ERR_ARG;
	nn_decoder_desc(out, t);
	return NN_SVC_OK;
}

int nn_svc_input(struct tensor_desc *out)
{
	struct nn_model *m = NULL;
	struct nn_tensor *t;

	if (nn_model_open(&m) != 0 || m == NULL)
		return NN_SVC_ERR_STATE;
	t = nn_input(m, 0);
	if (t == NULL)
		return NN_SVC_ERR_ARG;
	nn_decoder_desc(out, t);
	return NN_SVC_OK;
}

/* ---- one shot ------------------------------------------------------------ */

void nn_svc_run_once(struct nn_det_snapshot *snap, struct bf_det *dets, int max,
                     nn_svc_cancel_fn cancel, void *ctx,
                     struct nn_op_result *res)
{
	struct nn_camera_stats s;
	struct nn_camera_decode dec;
	int rc, stop_rc;

	nn_detail_clear();

	if (nn_camera_running()) {
		nn_detail_set("a stream is already running -- `nn stream stats`");
		nn_result(res, NN_SVC_ERR_BUSY, NN_CLAIM_NONE);
		return;
	}
	/*
	 * The inference path is a SUBSCRIBER here: it needs the base capture
	 * running to get a frame, and a one-shot cannot pull one from an idle base.
	 */
	if (!camera_streaming()) {
		nn_detail_set("base capture is off -- `camera stream start` first");
		nn_result(res, NN_SVC_ERR_STATE, NN_CLAIM_NONE);
		return;
	}
	rc = nn_camera_start(CAM_RES_QVGA);
	if (rc != 0) {
		nn_detail_set("start failed (%d): NN busy, or no model loaded", rc);
		nn_result(res, NN_SVC_ERR_BUSY, NN_CLAIM_NONE);
		return;
	}

	/*
	 * Bounded wait for one inference -- about two seconds.  The worker is below
	 * this thread in priority, so sleeping is what lets it run at all.
	 *
	 * [!] THE DEADLINE IS WALL CLOCK, not a count of completed sleeps: a
	 * tx_thread_sleep() that returns early because a tick was already pending
	 * would otherwise burn the budget in an instant and report a timeout that
	 * never happened.
	 */
	{
		ULONG deadline = tx_time_get() + (2u * TX_TIMER_TICKS_PER_SECOND);

		for (;;) {
			nn_camera_stats_get(&s);
			if (s.infers >= 1u)
				break;
			if (nn_svc_cancelled(cancel, ctx))
				break;
			if ((LONG)(tx_time_get() - deadline) >= 0)
				break;
			tx_thread_sleep(1u);
		}
	}

	/* [!] The boxes are taken BEFORE the stop: stopping ends the session, which
	 * invalidates the published record on purpose, so what this run is about has
	 * to be read while the session that produced them is still current. */
	memset(&dec, 0, sizeof dec);
	(void)nn_camera_decode_get(&dec, dets, max);
	snap->valid = dec.valid;
	snap->ndet  = dec.ndet;
	snap->res   = dec.res;

	stop_rc = nn_camera_stop();
	nn_result(res, NN_SVC_OK, nn_claim_of_stop(stop_rc));
	if (res->claim != NN_CLAIM_NONE)
		nn_detail_set("the camera has not released the inference frame (%d)",
		              stop_rc);
}

void nn_svc_decode_current(struct nn_det_snapshot *snap, struct bf_det *dets,
                           int max, struct nn_op_result *res)
{
	struct nn_camera_decode dec;

	nn_detail_clear();
	memset(&dec, 0, sizeof dec);
	(void)nn_camera_decode_get(&dec, dets, max);
	snap->valid = dec.valid;
	snap->ndet  = dec.ndet;
	snap->res   = dec.res;
	nn_result(res, NN_SVC_OK, NN_CLAIM_NONE);
}

/* ---- bench --------------------------------------------------------------- */

void nn_svc_bench_prepare(struct nn_op_result *res)
{
	struct nn_model *m = NULL;
	int i;

	nn_detail_clear();

	if (nn_model_open(&m) != 0 || m == NULL) {
		nn_detail_set("no model is loaded");
		nn_result(res, NN_SVC_ERR_STATE, NN_CLAIM_NONE);
		return;
	}
	if (nn_session_try_acquire() != 0) {
		nn_detail_set("NN busy (a stream or run is active)");
		nn_result(res, NN_SVC_ERR_BUSY, NN_CLAIM_NONE);
		return;
	}
	/* A fixed pattern so every run measures the same work. */
	for (i = 0; i < nn_input_count(m); i++) {
		struct nn_tensor *t = nn_input(m, i);

		if (t && t->data)
			memset(t->data, 0, t->bytes);
	}
	nn_session_release();
	nn_result(res, NN_SVC_OK, NN_CLAIM_NONE);
}

void nn_svc_bench_run(uint32_t iters, struct nn_bench_stats *out,
                      nn_svc_cancel_fn cancel, void *ctx,
                      struct nn_op_result *res)
{
	struct nn_model *m = NULL;
	uint32_t i;

	nn_detail_clear();
	memset(out, 0, sizeof *out);
	out->min_us = 0xFFFFFFFFu;

	if (nn_model_open(&m) != 0 || m == NULL) {
		nn_detail_set("no model is loaded");
		nn_result(res, NN_SVC_ERR_STATE, NN_CLAIM_NONE);
		return;
	}
	if (nn_session_try_acquire() != 0) {
		nn_detail_set("NN busy (a stream or run is active)");
		nn_result(res, NN_SVC_ERR_BUSY, NN_CLAIM_NONE);
		return;
	}

	for (i = 0u; i < iters; i++) {
		uint32_t us;

		if (nn_svc_cancelled(cancel, ctx)) {
			nn_detail_set("cancelled after %lu of %lu run(s)",
			              (unsigned long)i, (unsigned long)iters);
			nn_session_release();
			nn_result(res, NN_SVC_ERR_CANCEL, NN_CLAIM_NONE);
			return;
		}
		if (nn_run(m) != 0) {
			nn_detail_set("inference failed on run %lu of %lu",
			              (unsigned long)i + 1u, (unsigned long)iters);
			nn_session_release();
			nn_result(res, NN_SVC_ERR_HW, NN_CLAIM_NONE);
			return;
		}
		/* DWT core cycles -> microseconds.  The divisor is the TIMEBASE's
		   clock, which on this board is not the core clock. */
		us = nn_last_cycles(m) / (uint32_t)(CLI_CPU_CYCLES_PER_US);
		out->total_us += us;
		if (us < out->min_us)
			out->min_us = us;
		if (us > out->max_us)
			out->max_us = us;
		out->runs++;
	}
	nn_session_release();

	if (out->runs == 0u)
		out->min_us = 0u;
	else
		out->avg_us = (uint32_t)(out->total_us / out->runs);
	nn_result(res, NN_SVC_OK, NN_CLAIM_NONE);
}

/* ---- norm, boxes and threshold ------------------------------------------- */

void nn_svc_norm_set(int signed_range)
{
	nn_camera_set_norm(signed_range);
}

int nn_svc_norm_get(void)
{
	return nn_camera_get_norm();
}

int nn_svc_box_to_frame(const struct bf_det *in, struct bf_det *out)
{
	/*
	 * [!] THE IDENTITY HERE, AND THAT IS A FACT ABOUT THIS BOARD RATHER THAN A
	 * PLACEHOLDER.  The producer resizes the WHOLE frame into the model input
	 * (nearest-neighbour, port/nn/nn_camera.c), so the input's normalised space
	 * and the frame's are the same space.  Grove crops the centre square before
	 * scaling, so there the two differ and its adapter maps between them -- the
	 * reason this is a board operation at all.
	 */
	*out = *in;
	return NN_SVC_OK;
}

unsigned nn_svc_thresh_get(void)
{
	return nn_decoder_get_thresh_milli();
}

int nn_svc_thresh_set(unsigned milli)
{
	return (nn_decoder_set_thresh_milli(milli) == BF_OK) ? NN_SVC_OK
	                                                     : NN_SVC_ERR_ARG;
}
