/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    nn_svc_f746.c
 * @brief   This board behind the shared `nn` command's contract (issue #50).
 *
 * svc/nn/nn_svc.h is what shell/cmds/cmd_nn.c speaks; port/nn is what this board
 * has.  This is the translation, and it is a THIN one: unlike the Grove adapter,
 * it introduces no state at all.
 *
 * [!] IT WRAPS THE EXISTING OWNERS AND ADDS NO SECOND COPY.  The model
 * singleton and the session gate already live in svc/nn/nn.c, and the stream
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

#include <string.h>

#include "camera.h"
#include "gx_user.h"         /* GX_THREAD_STACK_SIZE: the GUIX thread draw() runs on */
#include "cli_config.h"      /* CLI_INSTANCE/BG_JOB_STACK_SIZE: the `at call` lines */
#include "fmt.h"
#include "nn.h"
#include "nn_camera.h"
#include "nn_input_contract.h" /* nn_input_refusal_name() (#162) */
#include "nn_desc.h"
#include "plugin_target.h"   /* the target word this build provides (#159) */
#if defined(CONFIG_NN_BACKEND_TFLM)
#include "nn_active.h"       /* the plugin entries (#159 Step 4b)          */
#include "nn_panel.h"        /* the GUIX draw's figures (#159 Step 4b)     */
#include "plugin_lease.h"    /* the one lock into the plugin               */
#endif
#include "stm32f7xx_hal.h"   /* HAL_RCC_GetHCLKFreq: the DWT counter's clock */
#include "tx_api.h"

/*
 * [!] THE PLUGIN TARGET WORD IS CHECKED HERE, against this firmware's own build
 * (issue #159).  board.cmake hands one value to this firmware's policy (and,
 * from Step 4b, to the packer and the host container verifier); this is the
 * only check of its CMSE bit, which no image records.  The macros cannot tell
 * this Cortex-M7 / FPv5-SP from a Cortex-M4 / FPv4, so board.cmake also checks
 * the word against the linked image (cmake/check_target_word.py).  Every
 * backend: the word describes the firmware, not the model it can load.
 */
#ifndef F746_PLUGIN_TARGET_ID
#error "board.cmake must define F746_PLUGIN_TARGET_ID"
#endif
_Static_assert(F746_PLUGIN_TARGET_ID == PLUGIN_TARGET_ID_HERE,
               "F746_PLUGIN_TARGET_ID does not describe this firmware's build "
               "(svc/plugin/plugin_target.h)");

#if defined(CONFIG_NN_BACKEND_TFLM)
/*
 * Each plugin stack allowance (board.cmake's plugin_stack_table()) strictly
 * below the stack of every thread its slot runs on -- Grove's issue #103 found
 * placeholders equal to a whole thread stack, which a plugin could have
 * declared, been admitted with and overflowed.  The decode slot's thread (the
 * nn worker) is asserted in nn_camera.c, where its stack is.  Draw runs on the
 * GUIX thread from Step 4b.
 */
#include "gx_user.h"   /* GX_THREAD_STACK_SIZE: the GUIX system thread's stack */
_Static_assert(F746_PLUGIN_STACK_GUIX < GX_THREAD_STACK_SIZE,
               "the draw allowance must be below the GUIX thread's stack");
_Static_assert(F746_PLUGIN_STACK_SHELL < CLI_INSTANCE_STACK_SIZE &&
               F746_PLUGIN_STACK_SHELL < CLI_BG_JOB_STACK_SIZE,
               "the console allowance must be below every console's stack");
#endif

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

/* ---- info and model lifecycle ---------------------------------------------
 *
 * nn_svc_info(), nn_svc_model_load() and nn_svc_model_unload() are in
 * nn_svc_f746_model.c, behind the shared order (issue #131 step 7d). */

/* ---- tensors ------------------------------------------------------------- */

/* What an nn_model_open() refusal means to the caller: `sdram test` holding
 * .sdram.ai (issue #152) is a transient claim elsewhere -- BUSY, retry later --
 * not a missing model.  Every other failure keeps its old answer. */
static int nn_open_status(int rc)
{
	return (rc == NN_ERR_SCRATCH_HELD) ? NN_SVC_ERR_BUSY : NN_SVC_ERR_STATE;
}

/* The detail for a session or open refused for the same reason. */
static void nn_busy_detail(struct nn_op_result *res, int rc)
{
	if (rc == NN_ERR_SCRATCH_HELD)
		nn_detail_set(".sdram.ai is held by `sdram test`; try again "
		              "when it finishes");
	else
		nn_detail_set("NN busy (a stream or run is active)");
}

int nn_svc_tensors_pin(void)
{
	struct nn_model *m = NULL;
	int rc = nn_model_open(&m);

	if (rc != 0 || m == NULL)
		return nn_open_status(rc);
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
	int rc = nn_model_open(&m);

	if (rc != 0 || m == NULL)
		return nn_open_status(rc);
	return nn_output_count(m);
}

int nn_svc_output(unsigned index, struct tensor_desc *out)
{
	struct nn_model *m = NULL;
	struct nn_tensor *t;
	int rc = nn_model_open(&m);

	if (rc != 0 || m == NULL)
		return nn_open_status(rc);
	t = nn_output(m, (int)index);
	if (t == NULL)
		return NN_SVC_ERR_ARG;
	nn_desc_of(out, t);
	return NN_SVC_OK;
}

int nn_svc_input(struct tensor_desc *out)
{
	struct nn_model *m = NULL;
	struct nn_tensor *t;
	int rc = nn_model_open(&m);

	if (rc != 0 || m == NULL)
		return nn_open_status(rc);
	t = nn_input(m, 0);
	if (t == NULL)
		return NN_SVC_ERR_ARG;
	nn_desc_of(out, t);
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
/* nn_camera_start()'s -9 (issue #142 D3): nn reads RGB565 only, and starts the
 * camera in its current mode rather than choosing another. */
#define NN_CAMERA_NOT_RGB565 \
	"the camera is not streaming RGB565 (or its mode cannot): " \
	"`camera format rgb565` and `camera res qvga` first"
/* nn_camera_start()'s -10 (review F4): holding / starting the camera failed. */
#define NN_CAMERA_NO_START \
	"the camera could not be started -- `camera stream start` says why"

/*
 * nn_camera_start()'s refusal, in the words and the status both `nn run` and
 * `nn stream start` give.  The decoder's three (issue #159) are wio-lite-ai's
 * words; NODRAW stays STATE here, where wio-lite-ai says HW.
 *
 * The model input's refusals (issue #162) are wio-lite-ai's codes and words too:
 * one sentence per reason of the shared admission test, built from its reason
 * name, with status HW and nothing held, as wio-lite-ai reports them.
 */
static void nn_start_refusal(struct nn_op_result *res, int rc)
{
	int status = NN_SVC_ERR_HW;
	int reason = nn_input_start_reason_of(rc);

	if (reason != 0) {
		nn_detail_set(NN_INPUT_START_DETAIL_FMT, nn_input_refusal_name(reason),
		              nn_input_start_hint(reason));
		nn_result(res, status, NN_CLAIM_NONE);
		return;
	}
	switch (rc) {
	case NNCAM_ERR_MODEL:
		nn_detail_set("no model loaded, or it has no usable input tensor "
		              "(`nn info`)");
		break;
	case -9:
		nn_detail_set(NN_CAMERA_NOT_RGB565);
		status = NN_SVC_ERR_STATE;
		break;
	case -10:
		nn_detail_set(NN_CAMERA_NO_START);
		break;
	case NNCAM_ERR_SHAPES:
		nn_detail_set("the container's decoder cannot read this model's "
		              "outputs -- its two halves do not belong together "
		              "(`nn info`)");
		status = NN_SVC_ERR_STATE;
		break;
	/*
	 * [!] IT ALSO COVERS "THERE IS NO DECODER AT ALL" (Step 4c, wio-lite-ai's
	 * #116 words): a bare model reaches the same nn_active_can_draw() == 0
	 * as a plugin with no draw() slot, so the sentence names both and points
	 * at `nn info`, which knows which it is.
	 */
	case NNCAM_ERR_NODRAW:
		nn_detail_set("nothing would annotate a live preview: no decoder "
		              "is loaded, or the one that is draws nothing (`nn "
		              "info`); `nn run` still works");
		status = NN_SVC_ERR_STATE;
		break;
	case NNCAM_ERR_DECBUSY:
		nn_detail_set("the decoder could not be held still long enough to "
		              "ask it -- try again");
		status = NN_SVC_ERR_BUSY;
		break;
	default:
		nn_detail_set(NN_CAMERA_START_FAILED, rc);
		if (rc == -2)
			status = NN_SVC_ERR_STATE;
		break;
	}
	nn_result(res, status, NN_CLAIM_NONE);
}

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
	/* [!] CARRIED, NOT STATED, since issue #159 Step 4b: a loaded plugin
	 * publishes a count and keeps its result, and with none the worker
	 * publishes the raw tensors -- the record says which, in the same
	 * snapshot. */
	snap->kind       = dec->kind;
	snap->reportable = dec->reportable;
	snap->current    = dec->current;
	snap->accepted   = dec->accepted;
	snap->epoch      = dec->epoch;
}

/* ---- the stream lifecycle's binding (issues #99, #130) -------------------
 *
 * The machine is svc/nn/nn_stream_life.c's and the policy around it svc/nn/nn_core.c's
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
	/* A plugin's BF_ERR_MODEL is a mismatch (issue #159, nn_decode_count.h). */
	raw->model_errors   = st.gen_model_errors;
	raw->decoder_errors = st.gen_decoder_errors;
	raw->last_us   = st.last_us;
	raw->producing = st.running ? 1u : 0u;
}

static void nn_core_record_of(struct nn_det_snapshot *snap)
{
	struct nn_camera_decode dec;

	memset(&dec, 0, sizeof dec);
	(void)nn_camera_decode_get(&dec, NULL, 0, NULL, NULL);
	nn_snap_of(&dec, snap);
}

/* [!] NO COUNTER IS BASED AT THE COMMIT.  nn_camera_start() zeroes the per-
 * stream counters itself, before the record's base is read, so none needs one.
 * (Until issue #138 this worker counted an inference BEFORE it published it,
 * which no base could follow -- see nn_core_board::based; it counts in the
 * publish now, but zeroing at the start stays the simpler answer.)
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
	 * [!] NO "BASE IS OFF" REFUSAL ANY MORE (issue #142 D6): nn_camera_start()
	 * holds the camera open, starting it -- and holding the operator's place,
	 * so it keeps streaming after the run -- when nothing streams.
	 */
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
		if (rc == NN_ERR_SCRATCH_HELD) {
			nn_busy_detail(res, rc);        /* `sdram test` (#152) */
			nn_result(res, NN_SVC_ERR_BUSY, NN_CLAIM_NONE);
			return;
		}
		nn_start_refusal(res, rc);
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
	(void)nn_camera_decode_get(&dec, NULL, 0, NULL, NULL);
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
			(void)nn_camera_decode_get(&dec, NULL, 0, NULL, NULL);
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
	/* NONE unless a loaded plugin's account is captured below -- stated
	 * rather than left to the caller's initialiser (issue #110).  With a
	 * plugin in force the account is taken under the lease, in the same hold
	 * as the snapshot (issue #159). */
	nn_report_set(rep, NN_REPORT_NONE);
	(void)nn_camera_decode_get(&dec, dets, max, rep, ext);
	nn_snap_of(&dec, snap);
	/* [!] THIS RUN'S, OR NOT VALID (issue #118): the record's own `valid` is
	 * true of whatever ran last, and a valid snapshot is printed as this run's
	 * result. */
	snap->valid = nn_det_last_valid(snap, base);
	if (!snap->valid && ext != NULL)
		ext->what = (uint8_t)NN_EXTRA_NONE;     /* not this run's either */

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
	nn_report_set(rep, NN_REPORT_NONE);
	(void)nn_camera_decode_get(&dec, dets, max, rep, ext);
	nn_snap_of(&dec, snap);
	nn_result(res, NN_SVC_OK, NN_CLAIM_NONE);
}

/* ---- bench --------------------------------------------------------------- */

void nn_svc_bench_prepare(struct nn_op_result *res)
{
	struct nn_model *m = NULL;
	int i, rc;

	nn_detail_clear();

	rc = nn_model_open(&m);
	if (rc != 0 || m == NULL) {
		if (rc == NN_ERR_SCRATCH_HELD)
			nn_busy_detail(res, rc);
		else
			nn_detail_set("no model is loaded");
		nn_result(res, nn_open_status(rc), NN_CLAIM_NONE);
		return;
	}
	rc = nn_session_try_acquire();
	if (rc != 0) {
		nn_busy_detail(res, rc);
		nn_result(res, NN_SVC_ERR_BUSY, NN_CLAIM_NONE);
		return;
	}
	/* [!] A MODEL, ASKED UNDER THE SESSION (issue #131): the singleton is open
	 * with nothing in it after `nn model unload`, and a load cannot change the
	 * answer while this holds the session. */
	if (!nn_model_present(m)) {
		nn_session_release();
		nn_detail_set("no model is loaded");
		nn_result(res, NN_SVC_ERR_STATE, NN_CLAIM_NONE);
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
	int rc;

	nn_detail_clear();
	memset(out, 0, sizeof *out);
	out->min_us = 0xFFFFFFFFu;

	rc = nn_model_open(&m);
	if (rc != 0 || m == NULL) {
		if (rc == NN_ERR_SCRATCH_HELD)
			nn_busy_detail(res, rc);
		else
			nn_detail_set("no model is loaded");
		nn_result(res, nn_open_status(rc), NN_CLAIM_NONE);
		return;
	}
	rc = nn_session_try_acquire();
	if (rc != 0) {
		nn_busy_detail(res, rc);
		nn_result(res, NN_SVC_ERR_BUSY, NN_CLAIM_NONE);
		return;
	}
	/* Asked again under THIS hold: an unload may have run between prepare and
	 * here (issue #131).  Without it an empty model failed as "inference
	 * failed" -- a hardware answer for a state one. */
	if (!nn_model_present(m)) {
		nn_session_release();
		nn_detail_set("no model is loaded");
		nn_result(res, NN_SVC_ERR_STATE, NN_CLAIM_NONE);
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
 * The subscriber worker (port/nn/nn_worker.c, with its state and start / stop in
 * port/nn/nn_camera.c) is unchanged and still owns the lifecycle and the NN
 * session.  What this adds is an IDENTITY, so a `--frames`
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
		if (rc == NN_ERR_SCRATCH_HELD) {
			nn_busy_detail(res, rc);        /* `sdram test` (#152) */
			nn_result(res, NN_SVC_ERR_BUSY, NN_CLAIM_NONE);
			return;
		}
		nn_start_refusal(res, rc);
		return;
	}
	/* The record's accepted count is read by the commit, after the start's
	 * record boundary: the two interleavings are in svc/nn/nn_det_record.h
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
	 * [!] THIS IS A SUBSCRIBER OF THE BASE CAPTURE.  Since issue #142 the start
	 * holds the camera open (starting it when nothing streams), so the base is
	 * off here only while an overrun auto-recovery is pending -- and a bounded
	 * `--frames` run then waits for frames that come back with it.
	 */
	if (!camera_streaming())
		nn_detail_set("inference enabled -- the camera is re-arming after "
		              "an overrun; frames resume when it does");
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

/*
 * [!] EVERY LINE BELOW FITS NN_STREAM_LINE_MAX AT ITS WORST, AND THE BUILD SAYS
 * SO (wio-lite-ai's rule since issue #126: the shared command hands each line
 * 96 B and the copy truncates without a mark).  A format's worst case is its
 * literal less each conversion, plus the most that conversion can print: 10
 * digits for any uint32_t, 4 for a depth or a stack size.
 */
#define NN_LINE_WORST(fmt, nconv, digits) \
	(sizeof(fmt) - 1u - 2u * (nconv) - (nconv) + (digits))
#define NN_LINE_AT_WORK "at call : nn-worker %lu/%lu (decode), guix %lu/%lu (draw); high-water"
#define NN_LINE_AT_SH   "at call : shell %lu/%lu (entry, shapes_ok, report, param); high-water"
#define NN_LINE_PL_DREW "plugin  : drew %lu px max/frame of %lu, %lu refused"
#define NN_LINE_PL_HOLD "plugin  : frame lock held %lu us max by a draw"
#define NN_LINE_PL_MISS "plugin  : %lu frame(s) missed (run of %lu)"
/* The same line when an entry into the plugin has ever been refused because its
 * caller did not hold the lease (wio-lite-ai's form).  Counted from boot. */
#define NN_LINE_PL_UNHELD NN_LINE_PL_MISS "; %lu entry(s) refused unheld"
_Static_assert(NN_LINE_WORST(NN_LINE_PL_DREW, 3u, 3u * 10u) < NN_STREAM_LINE_MAX &&
               NN_LINE_WORST(NN_LINE_PL_HOLD, 1u, 10u) < NN_STREAM_LINE_MAX &&
               NN_LINE_WORST(NN_LINE_PL_MISS, 2u, 2u * 10u) < NN_STREAM_LINE_MAX &&
               NN_LINE_WORST(NN_LINE_PL_UNHELD, 3u, 3u * 10u) < NN_STREAM_LINE_MAX,
               "a stream line's worst case is longer than the caller's buffer");
_Static_assert(NNCAM_WORKER_STACK < 10000u && GX_THREAD_STACK_SIZE < 10000u &&
               CLI_INSTANCE_STACK_SIZE < 10000u && CLI_BG_JOB_STACK_SIZE < 10000u,
               "`at call` budgets four digits for a depth and its stack");
_Static_assert(NN_LINE_WORST(NN_LINE_AT_WORK, 4u, 4u * 4u) < NN_STREAM_LINE_MAX &&
               NN_LINE_WORST(NN_LINE_AT_SH, 2u, 2u * 4u) < NN_STREAM_LINE_MAX,
               "an `at call` line's worst case is longer than the caller's buffer");

/*
 * This board's extra lines: the base geometry, and -- since issue #159 Step 4b
 * -- where a plugin is called and what it cost the GUIX preview.
 *
 * Optional lines are SKIPPED rather than left as holes, because the caller
 * stops at the first index that reports nothing.  The `at call` lines appear
 * once a plugin call site has run; the plugin lines while a plugin is in force.
 * The figures are this stream's (armed by the start, not cleared by the stop),
 * so after a stop they still describe the stream that ended.
 */
int nn_svc_stream_lines(enum nn_stream_lines_ctx ctx, unsigned index,
                        char *buf, size_t cap)
{
	struct nn_camera_stats st;
	unsigned i, n;
	int plugin = 0;

	if (buf == NULL || cap == 0u)
		return NN_SVC_ERR_ARG;
	buf[0] = '\0';
	if (ctx != NN_STREAM_LINES_STATS)
		return 0;

	nn_camera_stats_get(&st);
#if defined(CONFIG_NN_BACKEND_TFLM)
	plugin = nn_active_is_plugin();
#endif
	for (i = 0u, n = 0u; i < 6u; i++) {
		if ((i == 1u || i == 2u) && st.depth_decode == 0u &&
		    st.depth_draw == 0u && st.depth_shell == 0u)
			continue;               /* no site has run: no numbers */
		if (i >= 3u && !plugin)
			continue;               /* the panel's plugin numbers */
		if (n++ != index)
			continue;
		switch (i) {
		case 0u:
			/* The base geometry the input adapted to.  It is a report,
			   never a setting -- see nn_svc_stream_start(). */
			nn_detail_to(buf, cap, "base    : %s",
			             (st.res == (uint8_t)CAM_RES_QQVGA) ? "qqvga" :
			             (st.res == (uint8_t)CAM_RES_QVGA)  ? "qvga" : "vga");
			return 1;
		case 1u:
			/* How much stack is already spent where a plugin is CALLED --
			 * the term the plugin allowances are checked against, which
			 * `thread`'s peak does not answer.  Over each thread's stack. */
			nn_detail_to(buf, cap, NN_LINE_AT_WORK,
			             (unsigned long)st.depth_decode,
			             (unsigned long)NNCAM_WORKER_STACK,
			             (unsigned long)st.depth_draw,
			             (unsigned long)GX_THREAD_STACK_SIZE);
			return 1;
		case 2u:
			nn_detail_to(buf, cap, NN_LINE_AT_SH,
			             (unsigned long)st.depth_shell,
			             (unsigned long)CLI_INSTANCE_STACK_SIZE);
			return 1;
#if defined(CONFIG_NN_BACKEND_TFLM)
		case 3u:
		case 4u:
		case 5u: {
			uint32_t spent = 0u, refused = 0u, hold = 0u;
			uint32_t miss = 0u, run = 0u, unheld;
			uint16_t w = 0u, h = 0u;

			nn_panel_stats(&spent, &refused, &hold);
			if (i == 3u) {
				nn_camera_frame_geom(&w, &h);
				nn_detail_to(buf, cap, NN_LINE_PL_DREW,
				             (unsigned long)spent,
				             (unsigned long)nn_panel_budget(w, h),
				             (unsigned long)refused);
			} else if (i == 4u) {
				/* What the draw budget bounds: how long the GUIX thread held
				 * the lock the producer stores the next frame under (the
				 * README's draw-budget section). */
				nn_detail_to(buf, cap, NN_LINE_PL_HOLD,
				             (unsigned long)hold);
			} else {
				plugin_lease_misses(&miss, &run);
				if ((unheld = plugin_lease_unheld()) != 0u)
					nn_detail_to(buf, cap, NN_LINE_PL_UNHELD,
					             (unsigned long)miss, (unsigned long)run,
					             (unsigned long)unheld);
				else
					nn_detail_to(buf, cap, NN_LINE_PL_MISS,
					             (unsigned long)miss, (unsigned long)run);
			}
			return 1;
		}
#endif
		default:
			break;
		}
	}
	return 0;
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
	 * (nearest-neighbour, port/nn/nn_producer.c), so the input's normalised space
	 * and the frame's are the same space.  Grove crops the centre square before
	 * scaling, so there the two differ and its adapter maps between them -- the
	 * reason this is a board operation at all.
	 */
	*out = *in;
	return NN_SVC_OK;
}

int nn_svc_thresh_get(unsigned *milli)
{
#if defined(CONFIG_NN_BACKEND_TFLM)
	int r;
#endif

	if (milli == NULL)
		return NN_SVC_ERR_ARG;
	*milli = NN_SVC_THRESH_NONE;
#if defined(CONFIG_NN_BACKEND_TFLM)
	/*
	 * [!] UNDER THE LEASE (issue #159, wio-lite-ai's #110).  This reaches a
	 * plugin's param_get, which reads plugin state that a decode may be
	 * rewriting and that a concurrent `nn model load` may be REPLACING -- and
	 * this command takes no NN session.  With no plugin the shared entry
	 * answers NONE: there is no resident decoder to hold a threshold since
	 * Step 4c.  No lease is BUSY, not "none" (issue #122 P6).
	 */
	if (!plugin_lease_take())
		return NN_SVC_ERR_BUSY;
	r = nn_active_get_thresh_milli(milli);
	plugin_lease_give();
	return r == NN_ACTIVE_THRESH_OK ? NN_SVC_OK : NN_SVC_ERR_BUSY;
#else
	/* [!] NO PLUGIN MECHANISM AND NO DECODER (wio-lite-ai's issue #116): the
	 * same answer the tflm build gives with none loaded. */
	return NN_SVC_OK;
#endif
}

int nn_svc_thresh_set(unsigned milli)
{
#if defined(CONFIG_NN_BACKEND_TFLM)
	int r;

	if (!plugin_lease_take())
		return NN_SVC_ERR_BUSY;
	r = nn_active_set_thresh_milli(milli);
	plugin_lease_give();
	switch (r) {
	case NN_ACTIVE_THRESH_OK:
		return NN_SVC_OK;
	case NN_ACTIVE_THRESH_NO_DECODER:
		/* nothing holds a threshold: a state, not a bad value */
		return NN_SVC_ERR_STATE;
	case NN_ACTIVE_NOT_HELD:
		return NN_SVC_ERR_BUSY;
	default:
		return NN_SVC_ERR_ARG;
	}
#else
	/* Nothing holds one here either -- and "there is nothing to set" is not
	 * "that value is out of range" (wio-lite-ai's issue #116). */
	(void)milli;
	return NN_SVC_ERR_STATE;
#endif
}

/* nn_svc_info_extra() -- the container's claims -- is in nn_svc_f746_model.c,
 * beside the claims it reads (issue #159). */
