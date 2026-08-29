/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    cmd_nn.c
 * @brief   The one `nn` command, shared by every board that runs inference
 *          (issue #50).
 *
 * Three boards each grew their own: `ai` on wio-lite-ai, a different `ai` on
 * f746g-disco, `nn` on grove-vision-ai-v2 -- about 3,100 lines of C printing the
 * same facts three ways, so that changing one meant reading three.  This is the
 * one command; each board supplies an adapter behind svc/nn_svc.h.
 *
 *   nn info                     backend / model / tensors / arena
 *   nn model load <source>      point the model lifecycle at something
 *   nn model unload             release it
 *   nn bench [n]                repeat-invoke timing
 *   nn run                      one frame -> infer -> boxes
 *   nn out [tensor] [n]         dequantised values of an output tensor
 *   nn dets                     decode the current outputs
 *   nn thresh [1..999]          detection score threshold
 *   nn norm <0|1>               float input normalisation, where it means anything
 *   nn stream <start|stop|stats>  live inference, where the board has a worker
 *   nn preview [frames]         live inference on the panel, where it has one
 *
 * [!] THIS FILE OWNS NO STORAGE, and cmake/check_no_mutable_storage.py audits
 * that per board.  It is the same rule as the shared decoder (issue #97) and for
 * the same reason: a static added here lands in memory that no board placed and
 * no board's residency gate names, and the f746 build in particular has nothing
 * that would notice.  Everything worth remembering is the adapter's.
 *
 * [!] AND IT PRINTS NO FLOATS.  svc/fmt.c implements no %f, so fractional values
 * go through nn_cmd_core.c's scaled-integer decomposition.  Reaching for a float
 * formatter here would pull one into three firmwares at once.
 */
#include "cli.h"

#include <stdint.h>
#include <string.h>

#include "nn_cmd_core.h"
#include "nn_svc.h"
#include "nn_svc_config.h"   /* the board says what it has -- see below */

/*
 * Capabilities: undefined means absent.  A board declares what it HAS, and the
 * table below registers a subcommand only under its macro, so `help` describes
 * that board rather than the family.
 *
 * [!] These name properties, never boards.  A macro that stands for "is Grove"
 * is `#ifdef <BOARD>` with a friendlier spelling, and this repo forbids that in
 * shell/ for the reason it keeps re-learning: it is how a shared file quietly
 * becomes three files again.
 */
#ifndef NN_SVC_HAS_MODEL_LOAD
#define NN_SVC_HAS_MODEL_LOAD  0
#endif
#ifndef NN_SVC_HAS_CAMERA
#define NN_SVC_HAS_CAMERA      0
#endif
#ifndef NN_SVC_HAS_BENCH
#define NN_SVC_HAS_BENCH       0
#endif
#ifndef NN_SVC_HAS_NORM
#define NN_SVC_HAS_NORM        0
#endif
#ifndef NN_SVC_HAS_STREAM
#define NN_SVC_HAS_STREAM      0
#endif
#ifndef NN_SVC_HAS_PREVIEW
#define NN_SVC_HAS_PREVIEW     0
#endif
#ifndef NN_SVC_HAS_MODEL_PATH
#define NN_SVC_HAS_MODEL_PATH  0
#endif
#ifndef NN_SVC_HAS_OVERLAY
#define NN_SVC_HAS_OVERLAY     0
#endif

#if NN_SVC_HAS_STREAM
/** The board's own `nn stream` set: a worker's lifecycle is not a formatting
 *  difference, so it stays with the board until one grammar replaces both this
 *  and `preview` (issue #50 defers that deliberately). */
extern const struct cli_cmd nn_board_stream_subcmds[];
#endif
#if NN_SVC_HAS_PREVIEW
/** The board's own `nn preview`, likewise. */
int nn_board_preview(struct cli_instance *sh, int argc, char **argv);
#endif
#if NN_SVC_HAS_MODEL_PATH
/** The board's own whole-file read, for the `--path` source.  It lives in the
 *  board's cmds/ because the filesystem helper it uses takes a shell instance;
 *  passing it DOWN is what keeps the port from reaching up for it. */
int nn_board_read_file(void *ctx, const char *path, void *buf, uint32_t cap,
                       uint32_t *len);
#endif

/* Longest shape string worth carrying: four dimensions of up to ten digits, the
   separators, and the terminator. */
#define NN_SHAPE_MAX 48

/*
 * The cancellation hook a board waits on.
 *
 * The board must not see a shell instance, so it gets a plain function and an
 * opaque context.  This is the only place the two are connected.
 */
static int nn_cancel_shim(void *ctx)
{
	return cli_cancel_requested((struct cli_instance *)ctx) ? 1 : 0;
}

/* ---- saying what happened ------------------------------------------------ */

/*
 * Report one operation's two answers.
 *
 * [!] THE CLAIM IS PRINTED WHEN IT IS NOT THE ORDINARY ONE, because that is the
 * line that tells an operator what to do next, and the two unusual answers need
 * different actions: `retryable` means the stop can be repeated, `terminal`
 * means only a reset will clear it.  Collapsing them into "failed" would send
 * somebody to reboot a board that a second `nn stream stop` would have fixed --
 * or, worse, have them retry forever on one that will never come back.
 */
static void nn_report(struct cli_instance *sh, const char *what,
                      const struct nn_op_result *res)
{
	if (res->status != NN_SVC_OK)
		cli_error(sh, "nn: %s: %s (%d)\r\n", what,
		          res->detail[0] ? res->detail : nn_status_name(res->status),
		          res->status);

	switch (res->claim) {
	case NN_CLAIM_RETRYABLE:
		cli_warn(sh, "nn: teardown did not finish; nn is still held.  It may "
		             "settle on its own -- repeat the stop to find out\r\n");
		break;
	case NN_CLAIM_TERMINAL:
		cli_warn(sh, "nn: teardown could not be confirmed, so nn stays held "
		             "until reboot.  Releasing it now could dismantle "
		             "something a running thread is inside\r\n");
		break;
	default:
		break;
	}
}

/* ---- nn info ------------------------------------------------------------- */

/*
 * One tensor line.  The quantisation parameters are printed only for the integer
 * types, because for a float tensor they are meaningless -- f746 and wio publish
 * a scale of 0 for an unquantised tensor, and a line reading "q(s=0.000000)"
 * would look like a measured zero rather than "not applicable".
 */
static void nn_print_tensor(struct cli_instance *sh, const char *tag, int idx,
                            const struct tensor_desc *t)
{
	char shape[NN_SHAPE_MAX];

	(void)nn_shape_str(t, shape, sizeof shape);

	if (t->dtype == TENSOR_DTYPE_INT8 || t->dtype == TENSOR_DTYPE_UINT8 ||
	    t->dtype == TENSOR_DTYPE_INT16 || t->dtype == TENSOR_DTYPE_INT32) {
		const char *sign;
		uint32_t ip, frac;

		if (nn_f32_parts(t->scale, &sign, &ip, &frac) != 0)
			cli_print(sh, "  %s[%d]  %s %s  q(s=nan zp=%ld)  %luB\r\n",
			          tag, idx, shape, nn_dtype_name(t->dtype),
			          (long)t->zero_point, (unsigned long)t->bytes);
		else
			cli_print(sh, "  %s[%d]  %s %s  q(s=%s%lu.%06lu zp=%ld)  %luB\r\n",
			          tag, idx, shape, nn_dtype_name(t->dtype), sign,
			          (unsigned long)ip, (unsigned long)frac,
			          (long)t->zero_point, (unsigned long)t->bytes);
	} else {
		cli_print(sh, "  %s[%d]  %s %s  %luB\r\n", tag, idx, shape,
		          nn_dtype_name(t->dtype), (unsigned long)t->bytes);
	}
}

static int cmd_nn_info(struct cli_instance *sh, int argc, char **argv)
{
	struct nn_svc_info info;
	int i, n;

	(void)argc; (void)argv;

	memset(&info, 0, sizeof info);
	nn_svc_info(&info);

	if (info.version[0])
		cli_print(sh, "backend : %s (%s)\r\n", info.backend, info.version);
	else
		cli_print(sh, "backend : %s\r\n",
		          info.backend[0] ? info.backend : "-");
	cli_print(sh, "model   : %s\r\n",
	          info.model_active ? (info.model[0] ? info.model : "(unnamed)")
	                            : "(none)");
	if (info.source[0])
		cli_print(sh, "source  : %s\r\n", info.source);
	cli_print(sh, "arena   : %lu B reserved\r\n",
	          (unsigned long)info.arena_bytes);
	if (info.arena_used)
		cli_print(sh, "used    : %lu B (activations)\r\n",
		          (unsigned long)info.arena_used);

	if (info.model_active) {
		struct tensor_desc t;

		if (nn_svc_input(&t) == NN_SVC_OK)
			nn_print_tensor(sh, "in", 0, &t);
		n = nn_svc_output_count();
		for (i = 0; i < n; i++)
			if (nn_svc_output((unsigned)i, &t) == NN_SVC_OK)
				nn_print_tensor(sh, "out", i, &t);
	} else {
#if NN_SVC_HAS_MODEL_LOAD
		cli_warn(sh, "note    : nothing loaded -- `nn model load ...`\r\n");
#else
		cli_warn(sh, "note    : this build has no runtime model loader\r\n");
#endif
	}

	cli_print(sh, "thresh  : %u/1000\r\n", nn_svc_thresh_get());
	return 0;
}

/* ---- nn model load / unload ---------------------------------------------- */

#if NN_SVC_HAS_MODEL_LOAD
static const char nn_model_usage[] =
	"usage: nn model load <--name <name> | --slot <n> | --path <p> | "
	"builtin | --addr <addr> <len>>\r\n";

static int cmd_nn_model_load(struct cli_instance *sh, int argc, char **argv)
{
	struct nn_spec spec;
	struct nn_op_result res;
	enum nn_model_state state = NN_MODEL_EMPTY;
	int rc;

	/* argv[0] is "load"; the source starts after it. */
	rc = nn_spec_parse(argc - 1, &argv[1], &spec);
	if (rc == NN_SVC_ERR_SPEC) {
		cli_error(sh, "nn: '%s' is not a model source.  Say which kind it "
		              "is -- the same word means different things on "
		              "different boards\r\n", argv[1]);
		cli_print(sh, nn_model_usage);
		return 1;
	}
	if (rc != NN_SVC_OK) {
		cli_print(sh, nn_model_usage);
		return 1;
	}

	memset(&res, 0, sizeof res);
#if NN_SVC_HAS_MODEL_PATH
	nn_svc_model_load(&spec, nn_board_read_file, sh, &res, &state);
#else
	nn_svc_model_load(&spec, NULL, NULL, &res, &state);
#endif

	/*
	 * [!] THE RESULTING MODEL STATE IS ITS OWN ANSWER.  A refused load
	 * normally leaves the previous model active, but both STM32 backends
	 * document that if even rebuilding the previous one fails the model is
	 * left CLOSED.  An operator told only "rejected" would go on believing
	 * something is loaded when nothing is.
	 */
	if (res.status != NN_SVC_OK) {
		nn_report(sh, "model load", &res);
		if (state == NN_MODEL_EMPTY)
			cli_warn(sh, "nn: and the previous model could not be "
			             "restored -- nothing is loaded now\r\n");
		else if (state == NN_MODEL_PREVIOUS)
			cli_print(sh, "nn: the previous model is still active\r\n");
		return 1;
	}

	nn_report(sh, "model load", &res);
	cli_print(sh, "nn: loaded\r\n");
	return 0;
}

static int cmd_nn_model_unload(struct cli_instance *sh, int argc, char **argv)
{
	struct nn_op_result res;

	(void)argc; (void)argv;

	memset(&res, 0, sizeof res);
	nn_svc_model_unload(&res);
	if (res.status != NN_SVC_OK) {
		nn_report(sh, "model unload", &res);
		return 1;
	}
	nn_report(sh, "model unload", &res);
	cli_print(sh, "nn: unloaded\r\n");
	return 0;
}
#endif /* NN_SVC_HAS_MODEL_LOAD */

/* ---- nn bench ------------------------------------------------------------ */

#if NN_SVC_HAS_BENCH
static int cmd_nn_bench(struct cli_instance *sh, int argc, char **argv)
{
	struct nn_op_result res;
	struct nn_bench_stats st;
	uint32_t iters = 50u;

	if (argc > 1 && cli_parse_u32(argv[1], &iters) != 0) {
		cli_error(sh, "nn: usage: nn bench [iterations]\r\n");
		return 1;
	}
	if (iters == 0u || iters > 100000u) {
		cli_error(sh, "nn: iterations must be 1 .. 100000\r\n");
		return 1;
	}

	/* [!] The deterministic fill is not a tidy-up: on one board the arena is
	 * NOLOAD, so an unfilled input holds whatever survived the last reset and
	 * two runs are not measuring the same work. */
	memset(&res, 0, sizeof res);
	nn_svc_bench_prepare(&res);
	if (res.status != NN_SVC_OK) {
		nn_report(sh, "bench", &res);
		return 1;
	}

	memset(&res, 0, sizeof res);
	memset(&st, 0, sizeof st);
	nn_svc_bench_run(iters, &st, nn_cancel_shim, sh, &res);
	if (res.status != NN_SVC_OK) {
		nn_report(sh, "bench", &res);
		return 1;
	}
	nn_report(sh, "bench", &res);

	if (st.runs == 0u) {
		cli_warn(sh, "nn: no run completed\r\n");
		return 1;
	}
	/*
	 * [!] THE SPREAD, NOT JUST THE MEAN.  Inference shares a core with a
	 * console and, on two boards, a camera producer -- so the gap between the
	 * fastest and the slowest run is often the thing being looked for, and an
	 * average on its own hides it.
	 */
	cli_print(sh, "runs    : %lu\r\n", (unsigned long)st.runs);
	if (st.clock_mhz)
		cli_print(sh, "latency : min %lu  avg %lu  max %lu us  (@%lu MHz)\r\n",
		          (unsigned long)st.min_us, (unsigned long)st.avg_us,
		          (unsigned long)st.max_us, (unsigned long)st.clock_mhz);
	else
		cli_print(sh, "latency : min %lu  avg %lu  max %lu us\r\n",
		          (unsigned long)st.min_us, (unsigned long)st.avg_us,
		          (unsigned long)st.max_us);
	cli_print(sh, "total   : %lu.%03lu ms\r\n",
	          (unsigned long)(st.total_us / 1000u),
	          (unsigned long)(st.total_us % 1000u));
	/*
	 * [!] NAME BOTH CAUSES, because this cannot tell them apart and one of
	 * them is ordinary.  A backend that performs no inference (the `null`
	 * stub on one board returns immediately) legitimately takes zero
	 * measurable time; a stopped cycle counter looks identical from here.
	 * Blaming the time source alone sends someone debugging hardware that is
	 * fine.
	 */
	if (st.total_us == 0u)
		cli_warn(sh, "note    : zero elapsed -- either this backend performs "
		             "no inference, or the cycle counter is not advancing.  "
		             "Not a measurement either way\r\n");
	return 0;
}
#endif /* NN_SVC_HAS_BENCH */

/* ---- boxes --------------------------------------------------------------- */

/*
 * Print the boxes of one decode.
 *
 * [!] IN FRAME COORDINATES, THROUGH THE BOARD'S OWN MAPPING.  The decoder works
 * in the model input's space, and on one board that is the whole frame while on
 * another it is the centre square -- so scaling by 100 prints a box that is
 * simply somewhere else.  The board maps it, using the SAME transform its
 * overlay draws with, so the console and the panel cannot disagree about where a
 * face is (issue #48).
 *
 * [!] AND A NEGATIVE COUNT IS NOT ZERO FACES (issue #57).  Zero reads as a
 * measurement; -1 means the outputs are not a model this decoder knows, which is
 * a different thing to go and look at.
 */
/*
 * Top classes of an output vector, by insertion -- N is small and the vector is
 * a class count, so nothing cleverer earns its code size.
 *
 * [!] THIS IS WHAT MAKES ONE `run` SERVE BOTH KINDS OF MODEL.  The three boards
 * used to split classification and detection into separate subcommands, so the
 * operator had to know which the loaded model was and pick the matching verb.
 * What is loaded already decides that, and the decoder says so: only
 * BF_ERR_MODEL means "not a detector", and it is the one code that routes here
 * instead of being reported as a failure.
 */
static void nn_print_top(struct cli_instance *sh, unsigned index)
{
	struct tensor_desc td;
	const struct tensor_desc *t = &td;
	enum { TOP_N = 5 };
	int   best_i[TOP_N];
	float best_v[TOP_N];
	unsigned n = 0u, k;
	uint32_t esz, count, i;

	/* [!] PINNED ACROSS THE FETCH AND THE WALK.  The buffer is in the arena, and
	 * an unload on another console would close the interpreter underneath it. */
	if (nn_svc_tensors_pin() != NN_SVC_OK) {
		cli_warn(sh, "top     : the model is busy or gone\r\n");
		return;
	}
	if (nn_svc_output(index, &td) != NN_SVC_OK) {
		nn_svc_tensors_unpin();
		cli_warn(sh, "top     : output %u is unreadable\r\n", index);
		return;
	}
	esz = nn_dtype_size(t->dtype);
	if (esz == 0u || t->data == NULL) {
		nn_svc_tensors_unpin();
		cli_warn(sh, "top     : the output cannot be read at a known "
		             "stride (%s)\r\n", nn_dtype_name(t->dtype));
		return;
	}
	count = (uint32_t)(t->bytes / esz);

	for (k = 0u; k < TOP_N; k++) {
		best_i[k] = -1;
		best_v[k] = -1e30f;
	}
	for (i = 0u; i < count; i++) {
		float v;

		switch (t->dtype) {
		case TENSOR_DTYPE_INT8:
			v = ((float)((const int8_t *)t->data)[i] - (float)t->zero_point) *
			    t->scale;
			break;
		case TENSOR_DTYPE_UINT8:
			v = ((float)((const uint8_t *)t->data)[i] - (float)t->zero_point) *
			    t->scale;
			break;
		case TENSOR_DTYPE_FLOAT32:
			v = ((const float *)t->data)[i];
			break;
		default:
			nn_svc_tensors_unpin();
			return;   /* refused above for anything without a stride */
		}
		for (k = 0u; k < TOP_N; k++) {
			if (v > best_v[k]) {
				unsigned j;

				for (j = TOP_N - 1u; j > k; j--) {
					best_v[j] = best_v[j - 1u];
					best_i[j] = best_i[j - 1u];
				}
				best_v[k] = v;
				best_i[k] = (int)i;
				if (n < TOP_N)
					n++;
				break;
			}
		}
	}

	nn_svc_tensors_unpin();   /* the values are copied out; printing is safe */

	cli_print(sh, "top     : %u of %lu class(es)\r\n", n, (unsigned long)count);
	for (k = 0u; k < n; k++) {
		const char *sign;
		uint32_t ip, frac;

		if (nn_f32_parts(best_v[k], &sign, &ip, &frac) != 0)
			cli_print(sh, "  #%u  class %-4d  nan\r\n", k + 1u, best_i[k]);
		else
			cli_print(sh, "  #%u  class %-4d  score %s%lu.%06lu\r\n",
			          k + 1u, best_i[k], sign, (unsigned long)ip,
			          (unsigned long)frac);
	}
}

static void nn_print_dets(struct cli_instance *sh,
                          const struct nn_det_snapshot *snap,
                          const struct bf_det *dets)
{
	int i;

	/*
	 * [!] THE DECODER'S NEGATIVE CODES ARE NOT INTERCHANGEABLE (issue #97), and
	 * folding them here would undo that.  BF_ERR_MODEL means the loaded model
	 * is not a detector -- an ordinary thing to have loaded, and the cue to
	 * report classes instead.  The others mean this firmware is wired wrong,
	 * and reporting them as "not a detector" would send somebody off to
	 * re-check a model that is fine.
	 */
	if (snap->ndet == BF_ERR_MODEL) {
		nn_print_top(sh, 0u);
		return;
	}
	if (snap->ndet < 0) {
		cli_error(sh, "nn: the decoder is not usable (internal error %d)\r\n",
		          snap->ndet);
		return;
	}

	cli_print(sh, "dets    : %d  (x/y/w/h in %% of frame, score in milli; "
	              "thresh %u)\r\n", snap->ndet, snap->res.thresh_milli);

	for (i = 0; i < snap->ndet; i++) {
		struct bf_det f;

		if (nn_svc_box_to_frame(&dets[i], &f) != NN_SVC_OK) {
			cli_warn(sh, "  face[%d]  outside the frame\r\n", i);
			continue;
		}
		cli_print(sh, "  face[%d]  x %ld%% y %ld%% w %ld%% h %ld%%  "
		              "score %ld\r\n", i,
		          (long)(f.x * 100.0f), (long)(f.y * 100.0f),
		          (long)(f.w * 100.0f), (long)(f.h * 100.0f),
		          (long)(f.score * 1000.0f));
	}
}

#if NN_SVC_HAS_CAMERA
static int cmd_nn_run(struct cli_instance *sh, int argc, char **argv)
{
	struct nn_det_snapshot snap;
	struct bf_det dets[BF_MAX_DET];
	struct nn_op_result res;

	(void)argc; (void)argv;

	memset(&snap, 0, sizeof snap);
	memset(dets, 0, sizeof dets);
	memset(&res, 0, sizeof res);

	nn_svc_run_once(&snap, dets, BF_MAX_DET, nn_cancel_shim, sh, &res);
	if (res.status != NN_SVC_OK) {
		nn_report(sh, "run", &res);
		return 1;
	}
	/* Reported even on success: an incomplete teardown still holds the claim,
	 * and the board that used to run this discarded that answer entirely. */
	nn_report(sh, "run", &res);

	if (!snap.valid) {
		cli_warn(sh, "nn: no decode was published for that frame\r\n");
		return 1;
	}
	nn_print_dets(sh, &snap, dets);
	return 0;
}
#endif /* NN_SVC_HAS_CAMERA */

static int cmd_nn_dets(struct cli_instance *sh, int argc, char **argv)
{
	struct nn_det_snapshot snap;
	struct bf_det dets[BF_MAX_DET];
	struct nn_op_result res;

	(void)argc; (void)argv;

	memset(&snap, 0, sizeof snap);
	memset(dets, 0, sizeof dets);
	memset(&res, 0, sizeof res);

	nn_svc_decode_current(&snap, dets, BF_MAX_DET, &res);
	if (res.status != NN_SVC_OK) {
		nn_report(sh, "dets", &res);
		return 1;
	}
	nn_report(sh, "dets", &res);

	if (!snap.valid) {
		cli_warn(sh, "nn: nothing has been inferred yet\r\n");
		return 1;
	}
	nn_print_dets(sh, &snap, dets);
	return 0;
}

/* ---- nn out -------------------------------------------------------------- */

/*
 * Dequantised values of one output tensor.
 *
 * Reading the buffer needs the element size, and an unsupported dtype has none
 * -- so it is refused rather than walked at some assumed stride.  That is the
 * failure this repo has hit before: a descriptor read as something it is not
 * produces plausible numbers, and plausible numbers are the hard kind of wrong.
 */
static int cmd_nn_out(struct cli_instance *sh, int argc, char **argv)
{
	struct tensor_desc t;
	uint32_t idx = 0u, want = 16u, esz, have, i;
	int n, rc = 1;

	if (argc > 1 && cli_parse_u32(argv[1], &idx) != 0) {
		cli_error(sh, "nn: usage: nn out [tensor] [count]\r\n");
		return 1;
	}
	if (argc > 2 && cli_parse_u32(argv[2], &want) != 0) {
		cli_error(sh, "nn: usage: nn out [tensor] [count]\r\n");
		return 1;
	}

	n = nn_svc_output_count();
	if (n <= 0) {
		cli_error(sh, "nn: no outputs (is a model loaded?)\r\n");
		return 1;
	}
	if (idx >= (uint32_t)n) {
		cli_error(sh, "nn: tensor must be 0 .. %d\r\n", n - 1);
		return 1;
	}

	/*
	 * [!] PINNED FOR THE WHOLE WALK, not just the fetch.  This reads tensor
	 * BODIES -- the arena -- and a `nn model unload` on another console can
	 * close the interpreter, return the flash lease, or rebuild the model
	 * singleton while this loop is part way through it.  Both boards that had
	 * an equivalent command guarded exactly this and left `nn info` unguarded,
	 * because `info` never dereferences the buffer.
	 *
	 * One exit from here down, so the pin is returned exactly once however this
	 * ends -- the failure paths outnumber the success path.
	 */
	if (nn_svc_tensors_pin() != NN_SVC_OK) {
		cli_error(sh, "nn: busy -- another nn command holds the model\r\n");
		return 1;
	}

	if (nn_svc_output(idx, &t) != NN_SVC_OK || t.data == NULL) {
		cli_error(sh, "nn: output %lu has no buffer\r\n", (unsigned long)idx);
		goto out;
	}
	esz = nn_dtype_size(t.dtype);
	if (esz == 0u) {
		cli_error(sh, "nn: output %lu has an element type this build does "
		              "not know (%s) -- refusing to read it at a guessed "
		              "stride\r\n", (unsigned long)idx, nn_dtype_name(t.dtype));
		goto out;
	}
	have = (uint32_t)(t.bytes / esz);
	if (want > have)
		want = have;

	{
		char shape[NN_SHAPE_MAX];

		cli_print(sh, "out[%lu] : %s %s  %lu of %lu value(s)\r\n",
		          (unsigned long)idx, nn_shape_str(&t, shape, sizeof shape),
		          nn_dtype_name(t.dtype), (unsigned long)want,
		          (unsigned long)have);
	}

	for (i = 0u; i < want; i++) {
		const char *sign;
		uint32_t ip, frac;
		float v;

		switch (t.dtype) {
		case TENSOR_DTYPE_INT8:
			v = ((float)((const int8_t *)t.data)[i] - (float)t.zero_point) *
			    t.scale;
			break;
		case TENSOR_DTYPE_UINT8:
			v = ((float)((const uint8_t *)t.data)[i] - (float)t.zero_point) *
			    t.scale;
			break;
		case TENSOR_DTYPE_INT16:
			v = ((float)((const int16_t *)t.data)[i] - (float)t.zero_point) *
			    t.scale;
			break;
		case TENSOR_DTYPE_INT32:
			v = ((float)((const int32_t *)t.data)[i] - (float)t.zero_point) *
			    t.scale;
			break;
		case TENSOR_DTYPE_FLOAT32:
			/* [!] No affine here: scale and zero point are meaningless for a
			 * float tensor, and two boards publish a scale of 0 for one.
			 * Multiplying would print every value as zero. */
			v = ((const float *)t.data)[i];
			break;
		default:
			v = 0.0f;
			break;
		}

		if (nn_f32_parts(v, &sign, &ip, &frac) != 0)
			cli_print(sh, "  [%lu] nan\r\n", (unsigned long)i);
		else
			cli_print(sh, "  [%lu] %s%lu.%06lu\r\n", (unsigned long)i,
			          sign, (unsigned long)ip, (unsigned long)frac);

		if (cli_cancel_requested(sh))
			break;
	}
	rc = 0;
out:
	nn_svc_tensors_unpin();
	return rc;
}

/* ---- nn thresh ----------------------------------------------------------- */

static int cmd_nn_thresh(struct cli_instance *sh, int argc, char **argv)
{
	uint32_t milli;

	if (argc < 2) {
		cli_print(sh, "thresh  : %u/1000\r\n", nn_svc_thresh_get());
		return 0;
	}
	if (cli_parse_u32(argv[1], &milli) != 0 || milli == 0u || milli > 999u) {
		cli_error(sh, "nn: usage: nn thresh [1..999]  "
		              "(milli-probability)\r\n");
		return 1;
	}
	if (nn_svc_thresh_set((unsigned)milli) != NN_SVC_OK) {
		cli_error(sh, "nn: the threshold was not accepted\r\n");
		return 1;
	}
	cli_print(sh, "thresh  : %u/1000\r\n", nn_svc_thresh_get());
	return 0;
}

/* ---- nn norm ------------------------------------------------------------- */

#if NN_SVC_HAS_NORM
static int cmd_nn_norm(struct cli_instance *sh, int argc, char **argv)
{
	uint32_t v;

	if (argc < 2) {
		cli_print(sh, "norm    : %d  (%s)\r\n", nn_svc_norm_get(),
		          nn_svc_norm_get() ? "[-1,1]" : "[0,1]");
		return 0;
	}
	if (cli_parse_u32(argv[1], &v) != 0 || v > 1u) {
		cli_error(sh, "nn: usage: nn norm <0|1>  (0=[0,1], 1=[-1,1])\r\n");
		return 1;
	}
	nn_svc_norm_set((int)v);
	cli_print(sh, "norm    : %d  (%s)\r\n", nn_svc_norm_get(),
	          nn_svc_norm_get() ? "[-1,1]" : "[0,1]");
	return 0;
}
#endif /* NN_SVC_HAS_NORM */

#if NN_SVC_HAS_OVERLAY
static int cmd_nn_overlay(struct cli_instance *sh, int argc, char **argv)
{
	if (argc < 2) {
		cli_print(sh, "overlay : %s\r\n", nn_svc_overlay_get() ? "on" : "off");
		return 0;
	}
	if (strcmp(argv[1], "on") == 0) {
		nn_svc_overlay_set(1);
	} else if (strcmp(argv[1], "off") == 0) {
		nn_svc_overlay_set(0);
	} else {
		cli_error(sh, "nn: usage: nn overlay <on|off>\r\n");
		return 1;
	}
	cli_print(sh, "overlay : %s\r\n", nn_svc_overlay_get() ? "on" : "off");
	return 0;
}
#endif /* NN_SVC_HAS_OVERLAY */

/* ---- registration -------------------------------------------------------- */

#if NN_SVC_HAS_MODEL_LOAD
CLI_SUBCMD_SET_CREATE(nn_model_subcmds,
	CLI_CMD_ARG_USAGE(load, NULL, "point the model at a source",
	                  nn_model_usage, cmd_nn_model_load, 2, 3),
	CLI_CMD(unload, NULL, "release the model and what it holds",
	        cmd_nn_model_unload),
	CLI_SUBCMD_SET_END);
#endif

/*
 * [!] A SUBCOMMAND A BOARD DOES NOT HAVE IS NOT REGISTERED, rather than
 * registered as a stub that answers "not supported".  `help` is the only
 * inventory an operator has, and one that lists what the family can do rather
 * than what this board can do is worse than no list.
 */
CLI_SUBCMD_SET_CREATE(nn_subcmds,
	CLI_CMD(info, NULL, "backend / model / tensor shapes / arena", cmd_nn_info),
#if NN_SVC_HAS_MODEL_LOAD
	CLI_CMD_ARG_USAGE(model, nn_model_subcmds, "runtime model",
	                  nn_model_usage, NULL, 1, 4),
#endif
#if NN_SVC_HAS_BENCH
	CLI_CMD_ARG_USAGE(bench, NULL, "run inference [n] times, report latency",
	                  "usage: nn bench [iterations]\r\n", cmd_nn_bench, 1, 1),
#endif
#if NN_SVC_HAS_CAMERA
	CLI_CMD(run, NULL, "one frame: capture, infer, print the boxes", cmd_nn_run),
#endif
	CLI_CMD_ARG_USAGE(out, NULL, "dequantised values of an output tensor",
	                  "usage: nn out [tensor] [count]\r\n", cmd_nn_out, 1, 2),
	CLI_CMD(dets, NULL, "decode the current outputs into boxes", cmd_nn_dets),
	CLI_CMD_ARG_USAGE(thresh, NULL, "detection score threshold",
	                  "usage: nn thresh [1..999]\r\n", cmd_nn_thresh, 1, 1),
#if NN_SVC_HAS_NORM
	CLI_CMD_ARG_USAGE(norm, NULL, "float input normalisation",
	                  "usage: nn norm <0|1>\r\n", cmd_nn_norm, 1, 1),
#endif
#if NN_SVC_HAS_OVERLAY
	CLI_CMD_ARG_USAGE(overlay, NULL, "draw the boxes on the live preview",
	                  "usage: nn overlay <on|off>\r\n", cmd_nn_overlay, 1, 1),
#endif
#if NN_SVC_HAS_STREAM
	CLI_CMD_ARG_USAGE(stream, nn_board_stream_subcmds, "live inference",
	                  "usage: nn stream <start|stop|stats>\r\n", NULL, 1, 3),
#endif
#if NN_SVC_HAS_PREVIEW
	CLI_CMD_ARG_USAGE(preview, NULL, "live inference on the panel",
	                  "usage: nn preview [frames]\r\n", nn_board_preview, 1, 1),
#endif
	CLI_SUBCMD_SET_END);

CLI_CMD_REGISTER(nn, nn_subcmds, "neural network inference", NULL, 1, 4);
