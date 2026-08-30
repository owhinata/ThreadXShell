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
 * of failure are formatted here and copied into the result the shared command
 * prints.
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
#include "stm32f7xx_hal.h"   /* HAL_RCC_GetHCLKFreq: the DWT counter's clock */
#include "tx_api.h"

/*
 * [!] THERE IS NO SHARED DIAGNOSTIC BUFFER -- see the note in the Grove adapter.
 * A failure's words go straight into the caller's result, so two consoles
 * building results at once cannot overwrite each other's explanation.
 */

static void nn_detail_to(char *dst, size_t cap, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	(void)fmt_vsnformat(dst, cap, fmt, ap);
	va_end(ap);
}

/* Every failure path writes into the result it is about to return. */
#define nn_detail_set(...) \
	nn_detail_to(res->detail, sizeof res->detail, __VA_ARGS__)
#define nn_detail_clear()  (res->detail[0] = '\0')

/* [!] The detail is COPIED into the caller's result here, at the one place a
 * result is built.  nn_detail is this file's buffer and the next command on
 * another console overwrites it -- so a pointer to it would be printed after it
 * had already become somebody else's sentence. */
static void nn_result(struct nn_op_result *res, int status, enum nn_claim claim)
{
	res->status = status;
	res->claim  = (uint8_t)claim;
}

/*
 * This board's stop codes, and nothing else (issue #99).
 *
 * [!] EXHAUSTIVE, WITH THE DEFAULT ELSEWHERE.  This used to end in a catch-all
 * "retryable" -- and even spelled the catch-all out twice, which is how a rule
 * that had stopped being a rule managed to look deliberate.  nn_stream_disp_of()
 * supplies the fail-closed default now.
 *
 * nn_camera_stop() returns exactly these five.
 */
static const struct nn_stream_disp nn_stop_disp[] = {
	{  0, NN_STREAM_CLAIM_NONE      },  /* stopped                          */
	{ -1, NN_STREAM_CLAIM_NONE      },  /* was not running                  */
	/* The worker is still inside nn_run(); it releases the session as it
	   exits, and the sink is already released, so this is not a refusal. */
	{ -2, NN_STREAM_CLAIM_RETRYABLE },
	/* The sink did not hand its frame back: a producer callback may still be
	   preprocessing into our staging buffers (issue #72). */
	{ -7, NN_STREAM_CLAIM_RETRYABLE },
	/* A start or another stop owns the lifecycle -- nothing was done. */
	{ -8, NN_STREAM_CLAIM_RETRYABLE },
};

static enum nn_claim nn_claim_of_stop(int stop_rc)
{
	return (enum nn_claim)nn_stream_disp_of(stop_rc, nn_stop_disp,
	                                        (unsigned)(sizeof nn_stop_disp /
	                                                   sizeof nn_stop_disp[0]));
}

/* ---- info ---------------------------------------------------------------- */

void nn_svc_info(struct nn_svc_info *out)
{
	const struct nn_backend_info *bi = nn_backend();
	struct nn_model *m = NULL;
	int held;

	memset(out, 0, sizeof *out);
	nn_svc_str(out->backend, sizeof out->backend, bi ? bi->name : NULL);
	nn_svc_str(out->version, sizeof out->version, bi ? bi->version : NULL);

	if (nn_model_open(&m) != 0 || m == NULL)
		return;

	/* Copied, not borrowed: the backend owns this name and a reload replaces
	   it, so the caller must not hold a pointer into it while printing. */
	out->model_active = 1u;
	out->arena_used  = 0u;   /* this backend reports only the reservation */
	/*
	 * [!] THE NAME IS COPIED UNDER THE SESSION WHEN THE SESSION IS FREE, and
	 * copied anyway when it is not.  That is a deliberate middle, not an
	 * oversight:
	 *
	 *   Taking the session unconditionally would make `nn info` REFUSE while a
	 *   stream runs -- the worker holds it for the whole stream -- and that is
	 *   exactly when the report is worth asking for.  Both boards' previous
	 *   `nn info` read this name with no claim at all for that reason.
	 *
	 *   Not taking it at all leaves the copy able to race a reload, which
	 *   rewrites the backend's name buffer byte by byte, and produce a torn
	 *   name.  Cosmetic and self-correcting, but avoidable.
	 *
	 * So: try.  A reload cannot be in flight while the session is free, so the
	 * common case is now provably clean; when it is held the behaviour is what
	 * it always was, and no diagnostic is lost.
	 */
	held = (nn_session_try_acquire() == 0);
	nn_svc_str(out->model, sizeof out->model, nn_model_name(m));
	out->arena_bytes = nn_activations_bytes(m);
	if (held)
		nn_session_release();


	/* [!] Say plainly when nothing is being inferred, so a latency from
	 * `nn bench` is never mistaken for a model's. */
	if (bi && strcmp(bi->name, "null") == 0)
		nn_svc_str(out->source, sizeof out->source,
		           "synthetic workload, not inference");

	/* Every section is answered here, streaming or not -- which is the point of
	   the copy above (issue #99 made this explicit rather than implied). */
	out->avail_identity = (uint8_t)NN_AVAIL_OK;
	out->avail_runtime  = (uint8_t)NN_AVAIL_OK;
	out->avail_tensors  = (uint8_t)NN_AVAIL_OK;
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
	uint32_t hclk = HAL_RCC_GetHCLKFreq();
	uint32_t mhz = (hclk / 1000000u) ? (hclk / 1000000u) : 1u;
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
		/*
		 * [!] DWT CYCCNT COUNTS THE CORE CLOCK, so the divisor is HCLK --
		 * NOT CLI_CPU_CYCLES_PER_US, which is this board's TIM2 timebase
		 * rate (108) and half the core clock (216).  Using it made every
		 * latency twice what it should be, silently, on a board where
		 * nothing else would have contradicted the number.
		 */
		us = mhz ? (nn_last_cycles(m) / mhz) : 0u;
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
	/* Say what the cycle -> microsecond conversion assumed. */
	out->clock_mhz = mhz;
	nn_result(res, NN_SVC_OK, NN_CLAIM_NONE);
}

/* ---- norm, boxes and threshold ------------------------------------------- */

/* ---- live inference (issue #99) ------------------------------------------
 *
 * The subscriber worker in port/nn/nn_camera.c is unchanged and still owns the
 * lifecycle and the NN session.  What this adds is an IDENTITY, so a `--frames`
 * waiter cannot stop a stream that another console started after its own had
 * gone.
 */
/* The lifecycle itself is svc/nn_stream_life.c's -- see there for why one
   machine rather than three, and for the rule that every call below is made
   under this port's own critical section. */
static struct nn_stream_life nn_life;
static uint32_t nn_stream_t0;
static uint32_t nn_stream_ms;

/* Admit a start BEFORE the worker is touched.  This board has no re-arm, so
   IDLE is the only phase a start may come from. */
static enum nn_stream_start_claim nn_stream_admit(void)
{
	enum nn_stream_start_claim r;
	TX_INTERRUPT_SAVE_AREA

	TX_DISABLE
	r = nn_stream_life_begin(&nn_life);
	TX_RESTORE
	return r;
}

static void nn_stream_unadmit(void)
{
	TX_INTERRUPT_SAVE_AREA

	TX_DISABLE
	(void)nn_stream_life_abort(&nn_life);
	TX_RESTORE
}

static void nn_stream_mint(uint32_t *gen)
{
	TX_INTERRUPT_SAVE_AREA

	TX_DISABLE
	/* [!] Published only once the generation exists -- see the note in
	 * svc/nn_stream_life.h on applying side effects to a refused transition. */
	*gen = nn_stream_life_commit(&nn_life);
	if (*gen != NN_STREAM_GEN_ANY) {
		nn_stream_t0 = (uint32_t)tx_time_get();
		nn_stream_ms = 0u;
	}
	TX_RESTORE
}

/*
 * [!] TEST AND CLAIM IN ONE CALL, under one critical section.  Comparing the
 * generation and then acting is what let two callers both be admitted for one
 * stream -- and the second of them then ran its teardown against whatever was
 * there by the time it got round to it, which on this board is an untagged
 * nn_camera_stop().
 */
static enum nn_stream_stop_claim nn_stream_claim_stop(uint32_t gen)
{
	enum nn_stream_stop_claim r;
	TX_INTERRUPT_SAVE_AREA

	TX_DISABLE
	r = nn_stream_life_claim_stop(&nn_life, gen);
	TX_RESTORE
	return r;
}


void nn_svc_stream_start(const struct nn_stream_spec *spec,
                         struct nn_op_result *res, uint32_t *gen)
{
	int rc;

	if (res == NULL)
		return;
	res->detail[0] = '\0';
	if (spec == NULL || gen == NULL) {
		nn_result(res, NN_SVC_ERR_ARG, NN_CLAIM_NONE);
		return;
	}
	if (spec->test) {
		nn_detail_set("this board has no test pattern to stream");
		nn_result(res, NN_SVC_ERR_SPEC, NN_CLAIM_NONE);
		return;
	}

	/* The geometry is NOT chosen here: the input adapts to whatever the base
	   capture publishes.  The resolution word this command used to take was
	   discarded by the port, and issue #99 removed it. */
	/* [!] ADMITTED BEFORE THE WORKER IS TOUCHED, so this never says IDLE while
	 * the board is already streaming. */
	switch (nn_stream_admit()) {
	case NN_STREAM_START_GO:
		break;
	case NN_STREAM_START_RUNNING:
		nn_detail_set("a stream is already running (`nn stream stats`)");
		nn_result(res, NN_SVC_ERR_STATE, NN_CLAIM_NONE);
		return;
	case NN_STREAM_START_DEAD:
		nn_detail_set("a previous teardown was never confirmed; only a reboot "
		              "clears it");
		nn_result(res, NN_SVC_ERR_HW, NN_CLAIM_TERMINAL);
		return;
	case NN_STREAM_START_BUSY:
	default:
		nn_detail_set("a start or a stop is already in progress -- retry");
		nn_result(res, NN_SVC_ERR_BUSY, NN_CLAIM_NONE);
		return;
	}

	rc = nn_camera_start(CAM_RES_QVGA);
	if (rc != 0) {
		nn_stream_unadmit();
		nn_detail_set("start failed (%d): NN busy (bench or another stream), "
		              "SDRAM down, or no model loaded?", rc);
		nn_result(res, (rc == -2) ? NN_SVC_ERR_STATE : NN_SVC_ERR_HW,
		          NN_CLAIM_NONE);
		return;
	}
	nn_stream_mint(gen);
	if (*gen == NN_STREAM_GEN_ANY) {
		/*
		 * [!] REFUSED, WHICH MEANS THIS CALLER NO LONGER OWNS THE START -- and
		 * therefore does not know who does.  Unreachable under the transitions
		 * admission allows, so this is invariant-failure handling; it fails
		 * CLOSED rather than tidying up.  Issuing a stop here would be an
		 * ownerless teardown that could collide with one already in progress,
		 * and releasing the claim could free something a live thread is inside.
		 * Report terminal and leave everything exactly as it is.
		 */
		nn_detail_set("the stream lifecycle moved underneath this start; what "
		              "owns the hardware now cannot be established");
		nn_result(res, NN_SVC_ERR_HW, NN_CLAIM_TERMINAL);
		return;
	}

	/*
	 * [!] THIS IS A SUBSCRIBER OF THE BASE CAPTURE, and saying so matters more
	 * now than it did: a bounded `--frames` run waits for frames that will
	 * never arrive until the base is started, and the wait itself cannot tell
	 * that from a slow camera.
	 */
	if (!camera_streaming())
		nn_detail_set("inference enabled -- start the base "
		              "(`camera stream start`) before frames arrive");
	else
		nn_detail_set("inference stream started");
	nn_result(res, NN_SVC_OK, NN_CLAIM_NONE);
}

int nn_svc_stream_poll(uint32_t gen, struct nn_stream_stats *out)
{
	struct nn_camera_stats st;
	struct nn_camera_decode dec;
	uint32_t seq0, seq1, g, t0, ms;
	uint8_t  phase;
	TX_INTERRUPT_SAVE_AREA

	if (out == NULL)
		return NN_SVC_ERR_ARG;

	TX_DISABLE
	nn_stream_life_snapshot(&nn_life, &g, &phase, &seq0);
	t0 = nn_stream_t0;
	ms = nn_stream_ms;
	TX_RESTORE

	if (g == NN_STREAM_GEN_ANY)
		return NN_SVC_ERR_STATE;                  /* nothing has ever run */
	/* [!] Only a DIFFERENT generation is "somebody else's".  Our own, finished,
	 * still answers -- that is how a waiter tells "mine ended" from "a
	 * successor is running", and they call for different words. */
	if (gen != NN_STREAM_GEN_ANY && gen != g)
		return NN_SVC_ERR_GEN;

	/* Outside the critical section: these take their own locks. */
	nn_camera_stats_get(&st);
	dec.valid = 0;
	(void)nn_camera_decode_get(&dec, NULL, 0);

	TX_DISABLE
	nn_stream_life_snapshot(&nn_life, NULL, NULL, &seq1);
	TX_RESTORE
	if (seq1 != seq0)
		return NN_SVC_ERR_STALE;

	memset(out, 0, sizeof *out);
	out->running = (phase == (uint8_t)NN_STREAM_PHASE_RUNNING &&
	               st.running) ? 1u : 0u;
	/* [!] The per-STREAM counts, not the per-attach ones -- see nn_camera.h.
	 * Mixing the two made `nn stream stats` report 60 frames in, 36 infers and
	 * 0 skipped, which cannot all be true of one period. */
	/* [!] OFFERED, not staged: a frame dropped for want of a free stage was
	 * still offered, and `skipped` has to be a subset of `frames`. */
	out->skipped = st.gen_drops;
	out->frames  = st.gen_frames + st.gen_drops;
	out->infers  = st.gen_infers;
	out->errors  = st.gen_errors;
	out->last_us = st.last_us;
	out->elapsed_ms = out->running
	                ? (uint32_t)(((uint32_t)tx_time_get() - t0) * 1000u /
	                             TX_TIMER_TICKS_PER_SECOND)
	                : ms;
	out->last_valid = dec.valid ? 1u : 0u;
	out->last_ndet  = (int32_t)dec.ndet;
	return NN_SVC_OK;
}

void nn_svc_stream_stop(uint32_t gen, struct nn_op_result *res)
{
	enum nn_claim claim;
	int rc;
	TX_INTERRUPT_SAVE_AREA

	if (res == NULL)
		return;
	res->detail[0] = '\0';

	switch (nn_stream_claim_stop(gen)) {
	case NN_STREAM_STOP_GO:
		break;
	case NN_STREAM_STOP_WRONG_GEN:
		nn_detail_set("that stream has already been replaced by another");
		nn_result(res, NN_SVC_ERR_GEN, NN_CLAIM_NONE);
		return;
	case NN_STREAM_STOP_BUSY:
		/* [!] NOTHING WAS ATTEMPTED, SO NOTHING IS THE CALLER'S TO RELEASE
		 * (issue #99, bench).  This used to report RETRYABLE, which made the
		 * shared reporter add "teardown did not finish; nn is still held" --
		 * said to a background waiter whose stream another console was at that
		 * moment tearing down perfectly well.  The claim is somebody else's and
		 * they are settling it; the retry advice belongs in the detail, not in
		 * a warning about a teardown this caller never began. */
		nn_detail_set("a start or another stop owns the stream -- retry");
		nn_result(res, NN_SVC_ERR_BUSY, NN_CLAIM_NONE);
		return;
	case NN_STREAM_STOP_DEAD:
		nn_detail_set("a previous teardown was never confirmed; only a reboot "
		              "clears it");
		nn_result(res, NN_SVC_ERR_HW, NN_CLAIM_TERMINAL);
		return;
	case NN_STREAM_STOP_IDLE:
	default:
		nn_detail_set("not running");
		nn_result(res, NN_SVC_ERR_STATE, NN_CLAIM_NONE);
		return;
	}

	/*
	 * [!] THE LIFECYCLE IS SETTLED ON EVERY EXIT FROM HERE, because the claim
	 * above left it in STOPPING and nothing else can start or stop until it is
	 * handed back.  A path that returned without doing so would wedge the
	 * stream for good -- the failure the retryable answer exists to avoid.
	 */
	rc = nn_camera_stop();
	claim = nn_claim_of_stop(rc);
	TX_DISABLE
	/* [!] The elapsed freeze rides on the transition, not beside it: if the
	 * settle were refused, freezing anyway would date a generation that is
	 * still running. */
	if (claim == NN_CLAIM_RETRYABLE) {
		(void)nn_stream_life_retry(&nn_life);  /* stoppable again, same gen */
	} else {
		int took = (claim == NN_CLAIM_TERMINAL)
		         ? nn_stream_life_poison(&nn_life)
		         : nn_stream_life_finish(&nn_life);  /* the generation is kept */

		if (took)
			nn_stream_ms = (uint32_t)(((uint32_t)tx_time_get() -
			                           nn_stream_t0) * 1000u /
			                          TX_TIMER_TICKS_PER_SECOND);
	}
	TX_RESTORE

	if (rc == -1) {
		nn_detail_set("not running");
		nn_result(res, NN_SVC_ERR_STATE, NN_CLAIM_NONE);
		return;
	}
	if (rc != 0) {
		/*
		 * [!] THE INCOMPLETE TEARDOWNS ARE RETRYABLE, NOT FAILURES (issue #72),
		 * and anything this board does not document is TERMINAL -- see
		 * nn_stop_disp[].
		 * -7 means the sink is detached but still pinned, so a producer callback
		 * may still be writing the staging buffers; -8 means another start or
		 * stop owns the transition.  In both the claim is still out and the
		 * release is idempotent, so repeating the stop is what settles it.
		 */
		if (rc == -8)
			nn_detail_set("another `nn stream` start/stop is in progress -- "
			              "retry");
		else if (rc == -7)
			nn_detail_set("the camera has not released the inference frame; "
			              "the stream stays reserved -- retry");
		else if (rc == -2)
			nn_detail_set("the worker is still inside an inference; it "
			              "releases the session as it exits -- retry");
		else
			/* [!] AND AN UNDOCUMENTED CODE MUST NOT SAY "RETRY" (issue #99).
			 * It is classified TERMINAL by nn_stop_disp[], so the shared
			 * reporter is about to say nn stays held until reboot -- telling
			 * the operator to retry in the same breath is two instructions
			 * that contradict each other. */
			nn_detail_set("the teardown returned an undocumented code (%d), so "
			              "it cannot be classified", rc);
		nn_result(res, NN_SVC_ERR_HW, claim);
		return;
	}
	nn_result(res, NN_SVC_OK, NN_CLAIM_NONE);
}

int nn_svc_stream_lines(enum nn_stream_lines_ctx ctx, unsigned index,
                        char *buf, size_t cap)
{
	struct nn_camera_stats st;

	if (buf == NULL || cap == 0u)
		return NN_SVC_ERR_ARG;
	buf[0] = '\0';
	if (ctx != NN_STREAM_LINES_STATS || index != 0u)
		return 0;

	nn_camera_stats_get(&st);
	/* The base geometry the input adapted to.  It is a report, never a setting
	   -- see the note in nn_svc_stream_start(). */
	nn_detail_to(buf, cap, "base    : %s",
	             (st.res == (uint8_t)CAM_RES_QQVGA) ? "qqvga" :
	             (st.res == (uint8_t)CAM_RES_QVGA)  ? "qvga" : "vga");
	return 1;
}

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

/*
 * Nothing to add (issue #101).
 *
 * The plugin container is a Grove path today; this board has no second source of
 * model-adjacent facts, so it says nothing.  Silence is a legal answer here --
 * unlike the shared fields, whose absence the command reports as withheld,
 * because these lines are the board's own subject and no reader can mistake
 * their absence for a fact about the model.
 */
void nn_svc_info_extra(nn_svc_write_fn write, void *ctx)
{
	(void)write;
	(void)ctx;
}
