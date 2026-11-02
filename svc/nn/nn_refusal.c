/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    nn_refusal.c
 * @brief   The one table of `nn` admission refusals and `nn run` endings
 *          (issue #174).  See nn_refusal.h.
 *
 * [!] NO MUTABLE STORAGE: the tables are const and the words go straight into
 * the caller's result.
 */
#include "nn_refusal.h"

#include <stddef.h>   /* NULL */

#include "nn_detail.h"       /* NN_SVC_DETAIL_LIT                  */
#include "nn_svc_adapter.h"  /* nn_detail_set, nn_result           */

/* One refusal: the two answers, which are always given together, and the
 * sentence. */
struct nn_refusal_says {
	int         status;
	uint8_t     claim;
	const char *text;
};

/*
 * [!] ONE SENTENCE FOR "DEAD" ON EVERY BOARD (issue #174).  A board whose
 * `reboot` refuses while the camera's stop is unconfirmed says so itself and
 * sends the operator to its reset button; on a board whose `reboot` never
 * refuses the parenthesis is simply never needed.
 */
#define NN_REFUSAL_DEAD \
	"a previous teardown was never confirmed; only a reset clears it " \
	"(`reboot`, or the board's reset button if it refuses)"

/*
 * [!] BUSY HAS TWO SENTENCES, CHOSEN BY A PROPERTY.  A board with a gate
 * (@ref nn_core_board::gate_held) refuses an admission while an ordinary nn
 * command holds that gate -- and its `nn run` also answers BUSY when its claim's
 * commit was refused, which is a start or a stop moving the lifecycle -- so it
 * names both and does not claim to know which.  A board without one is BUSY
 * only while a start or a stop is in progress.
 */
#define NN_REFUSAL_BUSY_GATE \
	"another nn command, or a start or a stop, holds it -- retry"
#define NN_REFUSAL_BUSY_NOGATE \
	"a start or a stop is already in progress -- retry"

static const struct nn_refusal_says nn_refusal_busy[2] = {
	{ NN_SVC_ERR_BUSY, (uint8_t)NN_CLAIM_NONE,
	  NN_SVC_DETAIL_LIT(NN_REFUSAL_BUSY_NOGATE) },
	{ NN_SVC_ERR_BUSY, (uint8_t)NN_CLAIM_NONE,
	  NN_SVC_DETAIL_LIT(NN_REFUSAL_BUSY_GATE) },
};

static const struct nn_refusal_says nn_refusal_dead = {
	NN_SVC_ERR_HW, (uint8_t)NN_CLAIM_TERMINAL,
	NN_SVC_DETAIL_LIT(NN_REFUSAL_DEAD)
};

/* `nn run`: a stream already up is BUSY, not STATE -- the run did not ask to
 * start a stream, so "one is already running" is not the answer to it. */
static const struct nn_refusal_says nn_refusal_run_running = {
	NN_SVC_ERR_BUSY, (uint8_t)NN_CLAIM_NONE,
	NN_SVC_DETAIL_LIT("a stream is already running -- `nn stream stats`")
};
static const struct nn_refusal_says nn_refusal_run_oneshot = {
	NN_SVC_ERR_BUSY, (uint8_t)NN_CLAIM_NONE,
	NN_SVC_DETAIL_LIT("another `nn run` holds the camera, or one returned "
	                  "with its teardown unfinished (`nn stream stop` "
	                  "finishes it)")
};

static const struct nn_refusal_says nn_refusal_start_running = {
	NN_SVC_ERR_STATE, (uint8_t)NN_CLAIM_NONE,
	NN_SVC_DETAIL_LIT("a stream is already running (`nn stream stats`)")
};
/* [!] NOT A RE-ARM (issue #120): the session is a `nn run`'s. */
static const struct nn_refusal_says nn_refusal_start_oneshot = {
	NN_SVC_ERR_BUSY, (uint8_t)NN_CLAIM_NONE,
	NN_SVC_DETAIL_LIT("a `nn run` holds the camera, or one returned with "
	                  "its teardown unfinished (`nn stream stop` finishes "
	                  "it)")
};

static const struct nn_refusal_says nn_refusal_stop_idle = {
	NN_SVC_ERR_STATE, (uint8_t)NN_CLAIM_NONE,
	NN_SVC_DETAIL_LIT("not running")
};
static const struct nn_refusal_says nn_refusal_stop_wrong_gen = {
	NN_SVC_ERR_GEN, (uint8_t)NN_CLAIM_NONE,
	NN_SVC_DETAIL_LIT("that stream has already been replaced by another")
};
/* [!] NOTHING WAS ATTEMPTED, SO NOTHING IS THE CALLER'S TO RELEASE (issue #99,
 * bench): the claim is somebody else's and they are settling it. */
static const struct nn_refusal_says nn_refusal_stop_busy = {
	NN_SVC_ERR_BUSY, (uint8_t)NN_CLAIM_NONE,
	NN_SVC_DETAIL_LIT("a start or another stop owns the stream -- retry")
};
/* [!] Refused, not "not running" (issue #120): a `nn run` owns the worker and
 * stops it itself.  Nothing was attempted. */
static const struct nn_refusal_says nn_refusal_stop_oneshot = {
	NN_SVC_ERR_BUSY, (uint8_t)NN_CLAIM_NONE,
	NN_SVC_DETAIL_LIT("a `nn run` holds the camera and stops it itself -- "
	                  "retry when it returns")
};

static int nn_refusal_has_gate(const struct nn_core_board *b)
{
	return b != NULL && b->gate_held != NULL;
}

static int nn_refusal_put(const struct nn_refusal_says *say,
                          struct nn_op_result *res)
{
	if (res == NULL)
		return 1;
	nn_result(res, say->status, (enum nn_claim)say->claim);
	nn_detail_set("%s", say->text);
	return 1;
}

int nn_refusal_run(enum nn_stream_start_claim why,
                   const struct nn_core_board *b, struct nn_op_result *res)
{
	switch (why) {
	case NN_STREAM_START_GO:
		return 0;
	case NN_STREAM_START_RUNNING:
		return nn_refusal_put(&nn_refusal_run_running, res);
	case NN_STREAM_START_ONESHOT:
		return nn_refusal_put(&nn_refusal_run_oneshot, res);
	case NN_STREAM_START_DEAD:
		return nn_refusal_put(&nn_refusal_dead, res);
	case NN_STREAM_START_BUSY:
	default:
		return nn_refusal_put(&nn_refusal_busy[nn_refusal_has_gate(b)],
		                      res);
	}
}

int nn_refusal_start(enum nn_stream_start_claim why,
                     const struct nn_core_board *b, struct nn_op_result *res)
{
	switch (why) {
	case NN_STREAM_START_GO:
		return 0;
	case NN_STREAM_START_RUNNING:
		return nn_refusal_put(&nn_refusal_start_running, res);
	case NN_STREAM_START_ONESHOT:
		return nn_refusal_put(&nn_refusal_start_oneshot, res);
	case NN_STREAM_START_DEAD:
		return nn_refusal_put(&nn_refusal_dead, res);
	case NN_STREAM_START_BUSY:
	default:
		return nn_refusal_put(&nn_refusal_busy[nn_refusal_has_gate(b)],
		                      res);
	}
}

int nn_refusal_stop(enum nn_stream_stop_claim why,
                    const struct nn_core_board *b, struct nn_op_result *res)
{
	(void)b;   /* a stop's claim does not consult the gate */
	switch (why) {
	case NN_STREAM_STOP_GO:
		return 0;
	case NN_STREAM_STOP_IDLE:
		return nn_refusal_put(&nn_refusal_stop_idle, res);
	case NN_STREAM_STOP_WRONG_GEN:
		return nn_refusal_put(&nn_refusal_stop_wrong_gen, res);
	case NN_STREAM_STOP_DEAD:
		return nn_refusal_put(&nn_refusal_dead, res);
	case NN_STREAM_STOP_ONESHOT:
		return nn_refusal_put(&nn_refusal_stop_oneshot, res);
	case NN_STREAM_STOP_BUSY:
	default:
		/* [!] Not STATE: see nn_refusal.h (the one changed answer). */
		return nn_refusal_put(&nn_refusal_stop_busy, res);
	}
}

int nn_refusal_run_end(enum nn_run_end end, uint32_t wait_ms,
                       enum nn_claim claim, const char *unfinished,
                       struct nn_op_result *res)
{
	const char *tail =
		(claim != NN_CLAIM_NONE && unfinished != NULL) ? unfinished : "";
	int status;

	switch (end) {
	case NN_RUN_LOST:
		status = NN_SVC_ERR_HW;
		break;
	case NN_RUN_CANCELLED:
		status = NN_SVC_ERR_CANCEL;
		break;
	case NN_RUN_TIMEOUT:
	default:
		status = NN_SVC_ERR_TIMEOUT;
		break;
	}
	if (res == NULL)
		return status;
	nn_result(res, status, claim);
	switch (end) {
	case NN_RUN_LOST:
		nn_detail_set("the camera stream was lost before an inference "
		              "completed%s", tail);
		break;
	case NN_RUN_CANCELLED:
		nn_detail_set("cancelled before an inference completed%s", tail);
		break;
	case NN_RUN_TIMEOUT:
	default:
		nn_detail_set("no inference completed within %lu ms%s",
		              (unsigned long)wait_ms, tail);
		break;
	}
	return status;
}
