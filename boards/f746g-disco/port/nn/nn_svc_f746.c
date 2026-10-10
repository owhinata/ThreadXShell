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
#include "nn_core.h"
#include "nn_report.h"
#include "nn_svc_adapter.h" /* nn_detail_set, nn_result, nn_info_line (#130) */
#include "nn_swap.h"        /* the shared load ending table (#122, #131) */

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
	   preprocessing into the input tensor (issue #72). */
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
	struct nn_swap_verdict v;
	void *buf = NULL;
	uint32_t cap = 0u, len = 0u;
	int open_after = 0;
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
		rc = nn_model_reload(NULL, 0u, NULL, &open_after);
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
		rc = nn_model_reload(buf, len, spec->path, &open_after);
	}

	/*
	 * [!] THE RESULTING MODEL STATE IS THE RELOAD'S OWN OUTCOME, NOT A QUESTION
	 * ASKED AFTERWARDS (issue #122 P1).  This backend's reload is transactional
	 * -- on a refusal the previous model normally stays active -- but it
	 * documents one exception: if even that could not be rebuilt, the model is
	 * left CLOSED.  This used to ask nn_model_open() after the session was
	 * released, and that call is not a question: on a closed singleton it OPENS
	 * one (the tflm backend adopts the built-in model) and succeeds -- so the
	 * exception read as PREVIOUS, and a console that asked in between could
	 * change the answer.  wio-lite-ai learned this in its issue #108.
	 *
	 * The state comes from the table every board shares (svc/nn_swap.c,
	 * issue #131); this board has no plugin, so none is ever refused.  It
	 * does not read whether a model was open when the load began -- the
	 * reload restores the previous model or leaves none, and a RESTORED
	 * ending is reported as PREVIOUS -- so it says "open" here (reading it
	 * under the session is issue #131 step 7d).  [!] THE STATUS STILL COMES
	 * FROM rc, not from the table's ok: the one input on which they differ (a
	 * reload that returned 0 and left no model) is not reachable on this
	 * board, and shell/test/test_nn_swap.c says so.
	 */
	nn_swap_decide(1, nn_swap_end_of(rc, open_after, 0), &v);
	*state = (enum nn_model_state)v.state;

	/*
	 * [!] THE LAST RESULT GOES WITH THE MODEL IT CAME FROM (issue #118), and
	 * it goes whenever what is open changed -- a new model, or a rollback that
	 * left nothing -- decided from the outcome above, not on every attempt.
	 * Until P1 this cleared on refusals too, because which of the two had
	 * happened was only known after the session was gone.  PREVIOUS changed
	 * nothing and keeps it.  Under the session, so no worker is publishing.
	 */
	if (v.invalidate)
		nn_camera_record_invalidate();

	nn_session_release();

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
		(void)nn_model_reload(NULL, 0u, NULL, NULL);
	/* The model went back to the built-in one, and the last result goes with
	 * the one it came from (issue #118) -- under the session. */
	nn_camera_record_invalidate();
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

/** How long `nn run` waits for its one inference, in seconds. */
#define NN_RUN_WAIT_S 2u

/* nn_camera_start()'s refusal, in the one wording both `nn run` and `nn stream
 * start` use (a literal, so nn_detail_set checks its length). */
#define NN_CAMERA_START_FAILED \
	"start failed (%d): NN busy (bench or another stream), SDRAM down, or " \
	"no model loaded?"

/*
 * The worker's snapshot, as the shared command reads it.  [!] EVERY FIELD IS
 * WRITTEN (issues #104, #110, #118): a projection that drops one hands the
 * caller whatever its initialiser left there.
 */
static void nn_snap_of(const struct nn_camera_decode *dec,
                       struct nn_det_snapshot *snap)
{
	snap->valid      = dec->valid;
	snap->ndet       = dec->ndet;
	snap->res        = dec->res;
	/* [!] STATED, NOT CARRIED: this board has one decoder and it fills the
	 * caller's array, so the kind is never in doubt -- and the resident
	 * decoder keeps no account of its own, so there is none to withhold. */
	snap->kind       = (uint8_t)NN_DET_CALLER_BOXES;
	snap->reportable = 0u;
	snap->current    = dec->current;
	snap->accepted   = dec->accepted;
	snap->epoch      = dec->epoch;
}

/* ---- the stream lifecycle's binding (issues #99, #130) -------------------
 *
 * The machine is svc/nn_stream_life.c's and the policy around it svc/nn_core.c's
 * -- one copy for every board.  What is here is only what this board is: its
 * critical section, its clock, and where its counters and its record are.
 */
static struct nn_core nn_core;

static unsigned nn_core_cs_enter(void)
{
	TX_INTERRUPT_SAVE_AREA

	TX_DISABLE
	return (unsigned)interrupt_save;
}

static void nn_core_cs_exit(unsigned posture)
{
	TX_INTERRUPT_SAVE_AREA

	interrupt_save = (UINT)posture;
	TX_RESTORE
}

static uint32_t nn_core_ticks(void)
{
	return (uint32_t)tx_time_get();
}

/* [!] The per-STREAM counts, not the per-attach ones -- see nn_camera.h: the
 * per-attach ones go back to zero on a re-attach.  Mixing the two made `nn
 * stream stats` report 60 frames in, 36 infers and 0 skipped, which cannot all
 * be true of one period.  [!] OFFERED, not handed over: a frame skipped because
 * the worker was inside a job was still offered (issue #130: skipped now means
 * that, as on wio-lite-ai -- a frame is no longer staged to be run later). */
static void nn_core_counts_of(struct nn_core_raw *raw, void *keep)
{
	struct nn_camera_stats st;

	(void)keep;
	nn_camera_stats_get(&st);
	raw->offered   = st.gen_frames + st.gen_drops;
	raw->skipped   = st.gen_drops;
	raw->infers    = st.gen_infers;
	raw->errors    = st.gen_errors;
	/* model_errors stays 0: BF_ERR_MODEL is a result on this board (D6). */
	raw->decoder_errors = st.gen_decoder_errors;
	raw->last_us   = st.last_us;
	raw->producing = st.running ? 1u : 0u;
}

static void nn_core_record_of(struct nn_det_snapshot *snap)
{
	struct nn_camera_decode dec;

	memset(&dec, 0, sizeof dec);
	(void)nn_camera_decode_get(&dec, NULL, 0, NULL);
	nn_snap_of(&dec, snap);
}

/* [!] NO COUNTER IS BASED AT THE COMMIT.  nn_camera_start() zeroes the per-
 * stream counters itself, before the record's base is read, and this worker
 * counts an inference BEFORE it publishes it -- so a base latched later could
 * take an inference whose result then counts as this stream's (see
 * nn_core_board::based).
 *
 * This board has no re-arm and no transient claim decided with the lifecycle
 * (the worker's session is taken by nn_camera_start()); its stream clock runs
 * only while the worker says it is running. */
static const struct nn_core_board nn_core_board = {
	.cs_enter             = nn_core_cs_enter,
	.cs_exit              = nn_core_cs_exit,
	.ticks                = nn_core_ticks,
	.ticks_per_s          = TX_TIMER_TICKS_PER_SECOND,
	.counts               = nn_core_counts_of,
	.record               = nn_core_record_of,
	.based                = 0u,   /* gen_* are zeroed by nn_camera_start() */
	.rearm                = 0u,
	.clock_needs_producer = 1u,
};

void nn_svc_run_once(struct nn_det_snapshot *snap, struct bf_det *dets, int max,
                     struct nn_report_capture *rep, struct nn_result_extra *ext,
                     nn_svc_cancel_fn cancel, void *ctx,
                     struct nn_op_result *res)
{
	struct nn_camera_decode dec;
	uint32_t gen, base;
	int rc, stop_rc;
	/* Why the wait ended -- the run's status, decided below (issue #122 P7). */
	enum { RUN_INFERRED, RUN_CANCELLED, RUN_TIMEOUT } why = RUN_TIMEOUT;

	nn_detail_clear();
	/*
	 * The inference path is a SUBSCRIBER here: it needs the base capture
	 * running to get a frame, and a one-shot cannot pull one from an idle base.
	 */
	if (!camera_streaming()) {
		nn_detail_set("base capture is off -- `camera stream start` first");
		nn_result(res, NN_SVC_ERR_STATE, NN_CLAIM_NONE);
		return;
	}
	/*
	 * [!] THE LIFECYCLE IS CLAIMED BEFORE THE WORKER IS TOUCHED (issue #120).
	 * `nn run` drives the same subscriber worker as `nn stream`, and it used to
	 * do so with the lifecycle saying IDLE -- so another console's `nn stream
	 * stop` was "not running" while this held the worker, and a `nn stream
	 * start` got as far as the worker's own refusal.  As a one-shot it cannot be
	 * stopped by anyone else and it stops itself by its own generation.
	 */
	switch (nn_core_oneshot_admit(&nn_core, &nn_core_board)) {
	case NN_STREAM_START_GO:
		break;
	case NN_STREAM_START_RUNNING:
		nn_detail_set("a stream is already running -- `nn stream stats`");
		nn_result(res, NN_SVC_ERR_BUSY, NN_CLAIM_NONE);
		return;
	case NN_STREAM_START_ONESHOT:
		nn_detail_set("another `nn run` holds the camera, or one returned "
		              "with its teardown unfinished (`nn stream stop` "
		              "finishes it)");
		nn_result(res, NN_SVC_ERR_BUSY, NN_CLAIM_NONE);
		return;
	case NN_STREAM_START_DEAD:
		nn_detail_set("a previous teardown was never confirmed; only a "
		              "reboot clears it");
		nn_result(res, NN_SVC_ERR_HW, NN_CLAIM_TERMINAL);
		return;
	case NN_STREAM_START_BUSY:
	default:
		nn_detail_set("a start or a stop is already in progress -- retry");
		nn_result(res, NN_SVC_ERR_BUSY, NN_CLAIM_NONE);
		return;
	}

	rc = nn_camera_start(CAM_RES_QVGA, 1);   /* a one-shot */
	if (rc != 0) {
		(void)nn_core_abort(&nn_core, &nn_core_board);
		/* The same words and status as `nn stream start` -- one refusal from
		 * one worker reads the same from either command. */
		nn_detail_set(NN_CAMERA_START_FAILED, rc);
		nn_result(res, (rc == -2) ? NN_SVC_ERR_STATE : NN_SVC_ERR_HW,
		          NN_CLAIM_NONE);
		return;
	}
	gen = nn_core_oneshot_commit(&nn_core, &nn_core_board);
	if (gen == NN_STREAM_GEN_ANY) {
		/* Unreachable under the transitions admission allows; fail CLOSED,
		 * exactly as a refused stream commit does -- a teardown now would
		 * be ownerless. */
		nn_detail_set("the stream lifecycle moved underneath this run; what "
		              "owns the hardware now cannot be established");
		nn_result(res, NN_SVC_ERR_HW, NN_CLAIM_TERMINAL);
		return;
	}

	/*
	 * [!] WHAT THIS RUN PRODUCED IS COUNTED BY THE RECORD (issue #118).  It
	 * used to wait on the worker's inference counter, which this board bumps
	 * BEFORE the decode and the publish -- so the wait could end with nothing
	 * published yet.  The record's accepted count moves under the publish's
	 * own lock and only for a publish the generation rule took.  The base is
	 * sampled after nn_camera_start()'s boundary.
	 */
	memset(&dec, 0, sizeof dec);
	(void)nn_camera_decode_get(&dec, NULL, 0, NULL);
	base = dec.accepted;

	/*
	 * Bounded wait for one inference -- NN_RUN_WAIT_S.  The worker is below
	 * this thread in priority, so sleeping is what lets it run at all.
	 *
	 * [!] THE DEADLINE IS WALL CLOCK, not a count of completed sleeps: a
	 * tx_thread_sleep() that returns early because a tick was already pending
	 * would otherwise burn the budget in an instant and report a timeout that
	 * never happened.
	 */
	{
		ULONG deadline = tx_time_get() +
		                 (NN_RUN_WAIT_S * TX_TIMER_TICKS_PER_SECOND);

		for (;;) {
			(void)nn_camera_decode_get(&dec, NULL, 0, NULL);
			if (dec.accepted != base) {
				why = RUN_INFERRED;
				break;
			}
			if (nn_svc_cancelled(cancel, ctx)) {
				why = RUN_CANCELLED;
				break;
			}
			if ((LONG)(tx_time_get() - deadline) >= 0) {
				why = RUN_TIMEOUT;
				break;
			}
			tx_thread_sleep(1u);
		}
	}

	/* The boxes are taken BEFORE the stop -- the record keeps them across it
	 * now (issue #118), but reading first keeps this in the order the other
	 * boards use. */
	memset(&dec, 0, sizeof dec);
	(void)nn_camera_decode_get(&dec, dets, max, ext);
	nn_snap_of(&dec, snap);
	/* [!] THIS RUN'S, OR NOT VALID (issue #118): the record's own `valid` is
	 * true of whatever ran last, and a valid snapshot is printed as this run's
	 * result. */
	snap->valid = nn_det_last_valid(snap, base);
	if (!snap->valid && ext != NULL)
		ext->what = (uint8_t)NN_EXTRA_NONE;     /* not this run's either */
	/* No external decoder on this board, so nothing was captured -- stated
	 * rather than left to the caller's initialiser, for the same reason the
	 * kind is (issue #110). */
	nn_report_set(rep, NN_REPORT_NONE);

	/* [!] A RESULT THAT MADE IT IS NOT A TIMEOUT (issue #122, review).  The
	 * deadline can pass in the same moment the inference publishes; the record
	 * was read after that, so reporting a timeout would throw away an answer
	 * that is sitting right here.  ONLY a timeout is promoted: a cancel is the
	 * operator's decision, and a valid record does not overrule it.  "Made
	 * it" is the accepted count against this run's base (issue #118). */
	if (why == RUN_TIMEOUT && snap->valid)
		why = RUN_INFERRED;

	/* [!] STOPPED BY ITS OWN GENERATION, claimed like any stop (issue #120).
	 * Nothing else can have claimed it -- an operator's stop is refused while
	 * this runs -- so a refusal is an invariant failure and fails closed. */
	if (nn_core_claim_stop(&nn_core, &nn_core_board, gen) !=
	    NN_STREAM_STOP_GO) {
		nn_detail_set("the stream lifecycle moved underneath this run; what "
		              "owns the hardware now cannot be established");
		nn_result(res, NN_SVC_ERR_HW, NN_CLAIM_TERMINAL);
		return;
	}
	stop_rc = nn_camera_stop();
	/* A retryable teardown leaves the one-shot RUNNING and the operator's to
	 * finish with `nn stream stop` -- see nn_stream_life.h. */
	nn_core_settle(&nn_core, &nn_core_board, nn_claim_of_stop(stop_rc), NULL,
	               NULL);

	/*
	 * [!] WHY THE WAIT ENDED IS THE STATUS (issue #122 P7).  A cancelled or
	 * timed-out run used to come back NN_SVC_OK with nothing published, which
	 * the shared command could only print as "no decode was published for that
	 * frame".  The disposition stays its own field beside either answer.
	 */
	switch (why) {
	case RUN_INFERRED:
		nn_result(res, NN_SVC_OK, nn_claim_of_stop(stop_rc));
		if (res->claim != NN_CLAIM_NONE)
			nn_detail_set("the camera has not released the inference "
			              "frame (%d)", stop_rc);
		return;
	case RUN_CANCELLED:
		nn_result(res, NN_SVC_ERR_CANCEL, nn_claim_of_stop(stop_rc));
		nn_detail_set("cancelled before an inference completed%s",
		              (res->claim != NN_CLAIM_NONE)
		              ? "; the camera has not released the frame either "
		                "(`nn stream stop` finishes it)"
		              : "");
		return;
	case RUN_TIMEOUT:
	default:
		nn_result(res, NN_SVC_ERR_TIMEOUT, nn_claim_of_stop(stop_rc));
		nn_detail_set("no inference completed within %u s%s",
		              (unsigned)NN_RUN_WAIT_S,
		              (res->claim != NN_CLAIM_NONE)
		              ? "; the camera has not released the frame either "
		                "(`nn stream stop` finishes it)"
		              : "");
		return;
	}
}

void nn_svc_decode_current(struct nn_det_snapshot *snap, struct bf_det *dets,
                           int max, struct nn_report_capture *rep,
                           struct nn_result_extra *ext,
                           struct nn_op_result *res)
{
	struct nn_camera_decode dec;

	nn_detail_clear();
	memset(&dec, 0, sizeof dec);
	/* [!] THE LAST RESULT, WHOEVER PRODUCED IT (issue #118) -- a `nn run`, a
	 * running stream or a stopped one.  Only a model change clears it. */
	(void)nn_camera_decode_get(&dec, dets, max, ext);
	nn_snap_of(&dec, snap);
	nn_report_set(rep, NN_REPORT_NONE);
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
	switch (nn_core_admit(&nn_core, &nn_core_board)) {
	case NN_STREAM_START_GO:
		break;
	case NN_STREAM_START_RUNNING:
		nn_detail_set("a stream is already running (`nn stream stats`)");
		nn_result(res, NN_SVC_ERR_STATE, NN_CLAIM_NONE);
		return;
	case NN_STREAM_START_ONESHOT:
		nn_detail_set("a `nn run` holds the camera, or one returned with its "
		              "teardown unfinished (`nn stream stop` finishes it)");
		nn_result(res, NN_SVC_ERR_BUSY, NN_CLAIM_NONE);
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

	rc = nn_camera_start(CAM_RES_QVGA, 0);
	if (rc != 0) {
		(void)nn_core_abort(&nn_core, &nn_core_board);
		nn_detail_set(NN_CAMERA_START_FAILED, rc);
		nn_result(res, (rc == -2) ? NN_SVC_ERR_STATE : NN_SVC_ERR_HW,
		          NN_CLAIM_NONE);
		return;
	}
	/* The record's accepted count is read by the commit, after the start's
	 * record boundary: the two interleavings are in svc/nn_det_record.h
	 * (issue #118).  It takes the record's lock outside the commit's critical
	 * section, which may not take it. */
	*gen = nn_core_commit(&nn_core, &nn_core_board, NULL);
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
	return nn_core_poll(&nn_core, &nn_core_board, gen, out);
}

void nn_svc_stream_stop(uint32_t gen, struct nn_op_result *res)
{
	struct nn_core_final final;
	enum nn_claim claim;
	int rc;

	if (res == NULL)
		return;
	res->detail[0] = '\0';

	switch (nn_core_claim_stop(&nn_core, &nn_core_board, gen)) {
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
	case NN_STREAM_STOP_ONESHOT:
		/* [!] Refused, not "not running" (issue #120): a `nn run` owns the
		 * worker and stops it itself.  Nothing was attempted. */
		nn_detail_set("a `nn run` owns the camera and stops it itself -- "
		              "retry when it returns");
		nn_result(res, NN_SVC_ERR_BUSY, NN_CLAIM_NONE);
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
	/* The stream's final numbers, once the worker is stopped.  [!] AND ITS LAST
	   RESULT (issue #118): the stop no longer retires the record, and taking
	   it now, after the stop's boundary, keeps a later `nn run` from showing
	   through. */
	nn_core_take_final(&nn_core, &nn_core_board, &final, NULL);
	nn_core_settle(&nn_core, &nn_core_board, claim, &final, NULL);

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
		 * may still be writing the input tensor; -8 means another start or
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

int nn_svc_thresh_get(unsigned *milli)
{
	/* Always answers: the resident decoder's threshold is a plain value
	 * behind no lock, so there is nothing to be busy on here. */
	if (milli == NULL)
		return NN_SVC_ERR_ARG;
	*milli = nn_decoder_get_thresh_milli();
	return NN_SVC_OK;
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

