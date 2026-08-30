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
 *   nn stream start [--test] [--frames <n>]   begin live inference
 *   nn stream stop | stats                    end it, or see how it is going
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
#ifndef NN_SVC_HAS_STREAM_TEST
#define NN_SVC_HAS_STREAM_TEST 0
#endif
#ifndef NN_SVC_HAS_MODEL_PATH
#define NN_SVC_HAS_MODEL_PATH  0
#endif
#ifndef NN_SVC_HAS_OVERLAY
#define NN_SVC_HAS_OVERLAY     0
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

/* One wording for every withheld section, so an operator learns it once. */
static const char nn_withheld[] =
	"-- held by a running stream (`nn stream stats`, or stop it)";

/*
 * The threshold line, in one place because two subcommands print it (#104).
 *
 * [!] ABSENCE HAS ITS OWN SPELLING.  A board can hold no threshold at all -- no
 * decoder loaded, or a loaded one that takes no parameters -- and rendering that
 * as `0/1000` offers the operator a setting to adjust that nothing would read.
 * The contract names the value (NN_SVC_THRESH_NONE); this is where it is said
 * out loud, once, so `nn info` and `nn thresh` cannot drift apart about it.
 */
static void nn_print_thresh(struct cli_instance *sh)
{
	unsigned milli = nn_svc_thresh_get();

	if (milli == NN_SVC_THRESH_NONE)
		cli_print(sh, "thresh  : none -- the active decoder has no "
		              "threshold\r\n");
	else
		cli_print(sh, "thresh  : %u/1000\r\n", milli);
}

/*
 * Adapt the board's length-bearing writer onto the shell's printer.
 *
 * [!] THE BUFFER IS A LOCAL, NOT A STATIC.  This file owns no mutable storage
 * and cmake/check_no_mutable_storage.py audits that per board; a scratch buffer
 * here would be exactly the static the gate exists to refuse.  A local is the
 * caller's stack, which the board that called in already accounted for.
 *
 * svc/fmt.c implements no precision, so a length-bearing string cannot be handed
 * to cli_print directly -- it is copied, terminated, and emitted in chunks.  A
 * board that writes more than fits in one chunk gets several calls, which is
 * why the writer reports what it TOOK rather than assuming everything landed.
 */
struct nn_info_sink {
	struct cli_instance *sh;
};

static int nn_info_write(void *ctx, const char *s, size_t len)
{
	struct nn_info_sink *sink = (struct nn_info_sink *)ctx;
	char chunk[64];
	size_t done = 0u;

	if (sink == NULL || sink->sh == NULL || (s == NULL && len != 0u))
		return -1;

	while (done < len) {
		size_t n = len - done;

		if (n > sizeof chunk - 1u)
			n = sizeof chunk - 1u;
		memcpy(chunk, s + done, n);
		chunk[n] = '\0';
		cli_print(sink->sh, "%s", chunk);
		done += n;
	}
	return (int)done;
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
	if (info.avail_identity == NN_AVAIL_WITHHELD) {
		cli_print(sh, "model   : %s\r\n", nn_withheld);
	} else {
		cli_print(sh, "model   : %s\r\n",
		          info.model_active ? (info.model[0] ? info.model : "(unnamed)")
		                            : "(none)");
		if (info.source[0])
			cli_print(sh, "source  : %s\r\n", info.source);
	}
	cli_print(sh, "arena   : %lu B reserved\r\n",
	          (unsigned long)info.arena_bytes);
	if (info.avail_runtime == NN_AVAIL_WITHHELD)
		cli_print(sh, "used    : %s\r\n", nn_withheld);
	else if (info.arena_used)
		cli_print(sh, "used    : %lu B (activations)\r\n",
		          (unsigned long)info.arena_used);

	/*
	 * [!] SAY WHY A SECTION IS MISSING (issue #99).  This used to probe the
	 * tensors and print nothing at all when the board refused, so a report
	 * taken while something else held the claim came out silently shorter --
	 * which reads as "a model with no tensors", a different and wrong fact.
	 * The board's answer is checked first, and the probe itself still reports
	 * a refusal, because the claim can be taken between the two.
	 */
	if (info.avail_tensors == NN_AVAIL_WITHHELD) {
		cli_print(sh, "tensors : %s\r\n", nn_withheld);
	} else if (info.model_active) {
		struct tensor_desc t;

		if (nn_svc_input(&t) == NN_SVC_OK)
			nn_print_tensor(sh, "in", 0, &t);
		else
			cli_warn(sh, "  in[0]   unavailable just now\r\n");
		n = nn_svc_output_count();
		if (n < 0)
			cli_warn(sh, "  out[]   unavailable just now\r\n");
		for (i = 0; i < n; i++) {
			if (nn_svc_output((unsigned)i, &t) == NN_SVC_OK)
				nn_print_tensor(sh, "out", i, &t);
			else
				cli_warn(sh, "  out[%d]  unavailable just now\r\n", i);
		}
	} else {
#if NN_SVC_HAS_MODEL_LOAD
		cli_warn(sh, "note    : nothing loaded -- `nn model load ...`\r\n");
#else
		cli_warn(sh, "note    : this build has no runtime model loader\r\n");
#endif
	}

	nn_print_thresh(sh);

	/* Whatever this board wants to add, in its own words.  Last, so the shared
	 * lines always appear in the same place whatever a board says after them. */
	{
		struct nn_info_sink sink;

		sink.sh = sh;
		nn_svc_info_extra(nn_info_write, &sink);
	}
	return 0;
}

/* ---- nn model load / unload ---------------------------------------------- */

#if NN_SVC_HAS_MODEL_LOAD
/* [!] TWO SPELLINGS, BECAUSE THERE ARE TWO CONSUMERS.  A .usage field carries
 * the ARGUMENT SPELLING only -- the dispatcher prints "usage: <command path> "
 * in front of it -- while the parser below prints a whole line of its own.  One
 * string used for both is how every wrong-argument line in this file came to
 * read "usage: nn bench usage: nn bench [iterations]". */
static const char nn_model_args[] =
	"load <--name <name> | --slot <n> | --path <p> | builtin | "
	"--addr <addr> <len>>";
static const char nn_model_usage[] =
	"usage: nn model load <--name <name> | --slot <n> | --path <p> | "
	"builtin | --addr <addr> <len>>\r\n";

static int cmd_nn_model_load(struct cli_instance *sh, int argc, char **argv)
{
	struct nn_spec spec;
	struct nn_op_result res;
	enum nn_model_state state = NN_MODEL_EMPTY;
	int had_model, rc;

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

	/* [!] SAMPLED BEFORE THE LOAD, so a refusal can tell "nothing was loaded in
	 * the first place" from "the previous model could not be rebuilt".  The
	 * state alone cannot: NN_MODEL_EMPTY means both. */
	{
		struct nn_svc_info before;

		memset(&before, 0, sizeof before);
		nn_svc_info(&before);
		had_model = before.model_active ? 1 : 0;
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
		if (state == NN_MODEL_EMPTY && had_model)
			cli_warn(sh, "nn: and the previous model could not be "
			             "restored -- nothing is loaded now\r\n");
		else if (state == NN_MODEL_EMPTY)
			cli_print(sh, "nn: nothing is loaded\r\n");
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
	int32_t best_raw[TOP_N];
	unsigned n = 0u, k;
	uint32_t esz, count, i;
	int integer_scored;

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
	integer_scored = (t->dtype != TENSOR_DTYPE_FLOAT32);
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
		best_raw[k] = 0;
	}
	for (i = 0u; i < count; i++) {
		int32_t raw;
		float v;

		switch (t->dtype) {
		case TENSOR_DTYPE_INT8:
			raw = ((const int8_t *)t->data)[i];
			v = ((float)raw - (float)t->zero_point) * t->scale;
			break;
		case TENSOR_DTYPE_UINT8:
			raw = ((const uint8_t *)t->data)[i];
			v = ((float)raw - (float)t->zero_point) * t->scale;
			break;
		case TENSOR_DTYPE_FLOAT32:
			raw = 0;
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
					best_raw[j] = best_raw[j - 1u];
				}
				best_v[k] = v;
				best_i[k] = (int)i;
				best_raw[k] = raw;
				if (n < TOP_N)
					n++;
				break;
			}
		}
	}

	nn_svc_tensors_unpin();   /* the values are copied out; printing is safe */

	cli_print(sh, "top     : %u of %lu class(es)\r\n", n, (unsigned long)count);
	for (k = 0u; k < n; k++) {
		/*
		 * [!] THE RAW CODE AND THE SCORE IN MILLI, matching what this report
		 * looked like before three commands became one.  The raw int8 is what
		 * gets checked first when a dequantised number looks wrong -- it is
		 * the half that does not depend on the scale being right -- and milli
		 * is the unit `dets` already prints, so one command does not carry
		 * two spellings of "score".
		 */
		long milli = (long)(best_v[k] * 1000.0f);

		if (best_v[k] != best_v[k])          /* NaN */
			cli_print(sh, "  #%u  class %-4d  raw %4ld  score nan\r\n",
			          k + 1u, best_i[k], (long)best_raw[k]);
		else if (integer_scored)
			cli_print(sh, "  #%u  class %-4d  raw %4ld  score %ld/1000\r\n",
			          k + 1u, best_i[k], (long)best_raw[k], milli);
		else
			cli_print(sh, "  #%u  class %-4d  score %ld/1000\r\n",
			          k + 1u, best_i[k], milli);
	}
}

/*
 * The outputs, as they are, for a board that has no decoder for them (#104).
 *
 * [!] THIS DESCRIBES, IT DOES NOT INTERPRET.  Every other report here says what
 * the numbers MEAN -- faces, classes -- and each of those readings belongs to a
 * decoder that agreed to it.  With no decoder there is no such agreement, so
 * what can honestly be printed is the shape of the buffers and where to read
 * them; `nn out` already does the reading, and repeating it here would be a
 * second value-dumping loop that could disagree with the first.
 *
 * Pinned across the whole walk for the reason nn_print_top() is: these are
 * descriptors of arena buffers, and an unload on another console closes the
 * interpreter underneath them.
 */
static void nn_print_raw_outputs(struct cli_instance *sh)
{
	int n, i;

	if (nn_svc_tensors_pin() != NN_SVC_OK) {
		cli_warn(sh, "out     : the model is busy or gone\r\n");
		return;
	}
	n = nn_svc_output_count();
	if (n < 0) {
		nn_svc_tensors_unpin();
		cli_warn(sh, "out     : the outputs are unavailable just now\r\n");
		return;
	}

	cli_print(sh, "decoder : none -- %d output(s), undecoded\r\n", n);
	for (i = 0; i < n; i++) {
		struct tensor_desc t;

		if (nn_svc_output((unsigned)i, &t) == NN_SVC_OK)
			nn_print_tensor(sh, "out", i, &t);
		else
			cli_warn(sh, "  out[%d]  unavailable just now\r\n", i);
	}
	nn_svc_tensors_unpin();

	cli_print(sh, "note    : `nn out <tensor> <count>` reads the values; a "
	              "container carrying a decoder interprets them\r\n");
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
	/*
	 * [!] THE BOXES MAY NOT BE OURS (issue #103).  When a loaded plugin decoded
	 * this, `dets` was never written -- the plugin's result has a shape this
	 * firmware does not know -- so the board describes it instead.  Checked
	 * before everything else, including the BF_ERR_MODEL branch below: those
	 * codes belong to the resident decoder's vocabulary, and a plugin's count
	 * is its own.
	 */
	if (snap->kind == (uint8_t)NN_DET_PLUGIN_REPORT) {
		struct nn_info_sink sink;

		sink.sh = sh;
		if (nn_svc_report(nn_info_write, &sink) < 0)
			cli_warn(sh, "nn: the decoder stopped part way through its own "
			             "report\r\n");
		return;
	}
	/*
	 * [!] NOTHING DECODED THIS, so nothing here interprets it (issue #104).  A
	 * board with no decoder ran the model and has raw outputs; the honest report
	 * is the tensors themselves.  Not the class report: that reads output 0 as a
	 * vector of class scores, which for a detector's regression tensor is a tidy
	 * table of numbers that mean nothing.
	 */
	if (snap->kind == (uint8_t)NN_DET_RAW_TENSORS) {
		nn_print_raw_outputs(sh);
		return;
	}

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
	int rc;

	if (argc < 2) {
		nn_print_thresh(sh);
		return 0;
	}
	if (cli_parse_u32(argv[1], &milli) != 0 || milli == 0u || milli > 999u) {
		cli_error(sh, "nn: usage: nn thresh [1..999]  "
		              "(milli-probability)\r\n");
		return 1;
	}
	rc = nn_svc_thresh_set((unsigned)milli);
	if (rc == NN_SVC_ERR_STATE) {
		/* [!] NOT "the threshold was not accepted" (issue #104).  The value was
		 * fine; there is nothing here that holds one.  Told apart because the
		 * two send an operator to different places -- a number to change, or a
		 * container to load. */
		cli_error(sh, "nn: no decoder is loaded, so there is no threshold to "
		              "set\r\n");
		return 1;
	}
	if (rc != NN_SVC_OK) {
		cli_error(sh, "nn: the threshold was not accepted\r\n");
		return 1;
	}
	nn_print_thresh(sh);
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

#if NN_SVC_HAS_STREAM
/* ---- nn stream (issue #99) -----------------------------------------------
 *
 * One implementation for all three boards.  Until issue #99 each board carried
 * its own handler -- two with a background worker, one that blocked and drew on
 * a panel -- and the three had drifted in grammar and output.  Everything that
 * differs is now either a field of struct nn_stream_stats or a line the board
 * writes for itself; everything else, including the bounded wait, is here.
 */

/*
 * The board's own lines, bounded twice over: by the buffer this owns and by the
 * count the contract fixes.  The cap is not decoration -- without it an adapter
 * that never says "no more" prints until the console is killed.
 */
static void nn_stream_lines(struct cli_instance *sh,
                            enum nn_stream_lines_ctx ctx)
{
	unsigned i;

	for (i = 0u; i < (unsigned)NN_STREAM_LINES_MAX; i++) {
		char line[NN_STREAM_LINE_MAX];
		int rc;

		line[0] = '\0';
		rc = nn_svc_stream_lines(ctx, i, line, sizeof line);
		if (rc == 0)
			return;
		if (rc < 0) {
			cli_warn(sh, "nn: the board stopped part way through its own "
			             "report (%d)\r\n", rc);
			return;
		}
		cli_print(sh, "%s\r\n", line);
	}
	cli_warn(sh, "nn: the board offered more than %u lines; the rest are not "
	             "shown\r\n", (unsigned)NN_STREAM_LINES_MAX);
}

/*
 * Every option is settled before anything is acquired, so a typo cannot leave a
 * stream running that the operator did not ask for.
 */
static int nn_stream_parse(struct cli_instance *sh, int argc, char **argv,
                           struct nn_stream_spec *spec, uint32_t *frames)
{
	int i, have_test = 0, have_frames = 0;

	memset(spec, 0, sizeof *spec);
	*frames = 0u;

	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--test") == 0) {
			if (have_test) {
				cli_error(sh, "nn: --test given twice\r\n");
				return -1;
			}
			have_test = 1;
			spec->test = 1u;
			continue;
		}
		if (strcmp(argv[i], "--frames") == 0) {
			uint32_t v;

			if (have_frames) {
				cli_error(sh, "nn: --frames given twice\r\n");
				return -1;
			}
			if (i + 1 >= argc) {
				cli_error(sh, "nn: --frames needs a count\r\n");
				return -1;
			}
			if (cli_parse_u32(argv[i + 1], &v) != 0) {
				cli_error(sh, "nn: '%s' is not a frame count\r\n",
				          argv[i + 1]);
				return -1;
			}
			/* [!] Zero is refused rather than taken as "for ever".  The
			 * command this replaces read 0 that way, and quietly giving a
			 * new option the old meaning is how one grammar becomes two. */
			if (v == 0u) {
				cli_error(sh, "nn: --frames 0 is not a run; leave it off "
				              "to start and return\r\n");
				return -1;
			}
			*frames = v;
			have_frames = 1;
			i++;
			continue;
		}
		cli_error(sh, "nn: '%s' is not an option here\r\n", argv[i]);
		return -1;
	}
#if !NN_SVC_HAS_STREAM_TEST
	if (spec->test) {
		cli_error(sh, "nn: this board has no test pattern to stream\r\n");
		return -1;
	}
#endif
	return 0;
}

static void nn_stream_report_stats(struct cli_instance *sh,
                                   const struct nn_stream_stats *st)
{
	uint32_t fps_x100 = 0u;

	/* 64-bit intermediate: infers * 100000 overflows 32 bits in well under an
	   hour of streaming, and a wrapped rate reads as a real measurement. */
	if (st->elapsed_ms != 0u)
		fps_x100 = (uint32_t)(((uint64_t)st->infers * 100000u) /
		                      (uint64_t)st->elapsed_ms);

	cli_print(sh, "state   : %s\r\n", st->running ? "running" : "stopped");
	cli_print(sh, "frames  : %lu in, %lu skipped, %lu error(s)\r\n",
	          (unsigned long)st->frames, (unsigned long)st->skipped,
	          (unsigned long)st->errors);
	cli_print(sh, "infers  : %lu in %lu ms  (%lu.%02lu inf/s)\r\n",
	          (unsigned long)st->infers, (unsigned long)st->elapsed_ms,
	          (unsigned long)(fps_x100 / 100u),
	          (unsigned long)(fps_x100 % 100u));
	/* [!] Say WHICH refusal (issue #97): one means "load a different model",
	 * the other means "this firmware is wired wrong", and the worker has no
	 * console of its own to tell them apart on. */
	if (st->model_errors != 0u || st->decoder_errors != 0u)
		cli_print(sh, "  of those: %lu not the shape the decoder wants, "
		              "%lu decoder fault(s)\r\n",
		          (unsigned long)st->model_errors,
		          (unsigned long)st->decoder_errors);
	if (st->infers != 0u)
		cli_print(sh, "latency : %lu us (last)\r\n",
		          (unsigned long)st->last_us);
	/*
	 * [!] THREE ANSWERS, NOT TWO (issue #97).  "Nothing decoded yet" is not
	 * "decoded nobody", and a negative count is not a count at all -- it is the
	 * decoder saying the open model is not one it recognises, which calls for
	 * loading a different model rather than for looking at the picture.
	 * Printing it as "-1 face(s)" hands the reader arithmetic to do on a
	 * sentinel.
	 */
	if (!st->last_valid)
		/* [!] Two reasons for no decode, and they are not the same fact.  One
		 * board drops its boxes when a stream stops -- deliberately, so a
		 * stopped stream does not leave stale detections on view -- and saying
		 * "nothing decoded yet" after a run that inferred hundreds of frames
		 * reads as a broken detector. */
		cli_print(sh, "last    : -- (%s)\r\n",
		          st->infers ? "the boxes are dropped when a stream stops"
		                     : "nothing decoded yet");
	else if (st->last_ndet < 0)
		cli_print(sh, "last    : -- (this model is not one the decoder "
		              "recognises)\r\n");
	else
		cli_print(sh, "last    : %ld face(s)\r\n", (long)st->last_ndet);
}

/*
 * Sample, tolerating the stale answer a two-phase read can legitimately give.
 * A handful of attempts is plenty -- a transition takes microseconds -- and
 * saying "it moved" beats printing two instants as though they were one.
 */
static int nn_stream_sample(uint32_t gen, struct nn_stream_stats *st)
{
	int rc = NN_SVC_ERR_STALE, tries;

	for (tries = 0; tries < 8 && rc == NN_SVC_ERR_STALE; tries++)
		rc = nn_svc_stream_poll(gen, st);
	return rc;
}

/*
 * The bounded run.  This is the whole of what Grove's blocking `preview` used
 * to be, and it is board-neutral because everything it needs is in the poll.
 */
static int nn_stream_wait(struct cli_instance *sh, uint32_t gen, uint32_t frames)
{
	struct nn_stream_stats st;
	struct nn_op_result res;
	int ended_early = 0;

	for (;;) {
		int rc;

		if (cli_cancel_requested(sh))
			break;

		rc = nn_stream_sample(gen, &st);
		if (rc == NN_SVC_ERR_GEN) {
			/*
			 * [!] SOMEBODY ELSE'S STREAM IS RUNNING, SO DO NOT STOP
			 * ANYTHING.  Ours was torn down while this waited and a new one
			 * started in its place; stopping here would release a camera and
			 * an NPU that now belong to whoever started it.
			 */
			cli_warn(sh, "nn: this run's stream was replaced by another one "
			             "-- leaving that one alone\r\n");
			return 1;
		}
		if (rc != NN_SVC_OK && rc != NN_SVC_ERR_STALE) {
			cli_error(sh, "nn: could not read the stream: %s (%d)\r\n",
			          nn_status_name(rc), rc);
			return 1;
		}
		if (rc == NN_SVC_OK) {
			if (!st.running) {
				ended_early = 1;
				break;
			}
			if (st.frames >= frames)
				break;
		}
		/*
		 * [!] SLEEP EVEN AFTER A STALE READ.  Looping straight back would spin
		 * at shell priority, starving the background jobs that run below it
		 * under TX_NO_TIME_SLICE -- and a stale answer is precisely a moment
		 * when another thread is mid-transition and needs the CPU.
		 */
		if (cli_sleep(sh, 1u) != 0)
			break;                    /* Ctrl+C, or the job was killed */
	}

	memset(&res, 0, sizeof res);
	nn_svc_stream_stop(gen, &res);

	/*
	 * [!] WHY THE RUN ENDED IS THE STOP'S ANSWER, NOT THE POLL'S.  The poll only
	 * sees that the stream is no longer producing, and that looks identical
	 * whether the producer gave up or another console stopped it -- which is the
	 * difference between "go and read dmesg" and "nothing is wrong".  Reporting
	 * before the stop sent an operator hunting for a fault that did not exist,
	 * every time they stopped a background run by hand.
	 */
	if (res.status == NN_SVC_ERR_BUSY || res.status == NN_SVC_ERR_GEN) {
		cli_warn(sh, "nn: this run's stream was stopped or replaced from "
		             "elsewhere -- leaving it alone\r\n");
		return 1;
	}
	if (res.status == NN_SVC_ERR_STATE && res.claim == NN_CLAIM_NONE) {
		/* Somebody else already settled it, or it ended and settled itself.
		   Either way there is nothing here to stop and nothing to report. */
		return ended_early ? 0 : 1;
	}
	/* This stop is the one that ended it, so the producer really did give up. */
	if (ended_early)
		cli_warn(sh, "nn: the stream stopped on its own before %lu frame(s)"
		             " -- `nn stream stats` and `dmesg` say why\r\n",
		         (unsigned long)frames);
	nn_report(sh, "stream stop", &res);
	if (res.status != NN_SVC_OK)
		return 1;
	cli_print(sh, "nn: stopped\r\n");
	return 0;
}

static int cmd_nn_stream_start(struct cli_instance *sh, int argc, char **argv)
{
	struct nn_stream_spec spec;
	struct nn_op_result res;
	uint32_t frames = 0u, gen = NN_STREAM_GEN_ANY;

	if (nn_stream_parse(sh, argc, argv, &spec, &frames) != 0)
		return 1;

	memset(&res, 0, sizeof res);
	nn_svc_stream_start(&spec, &res, &gen);
	if (res.status != NN_SVC_OK) {
		nn_report(sh, "stream start", &res);
		return 1;                     /* nothing started: never wait or stop */
	}
	cli_print(sh, "nn: %s\r\n", res.detail[0] ? res.detail : "stream started");
	nn_stream_lines(sh, NN_STREAM_LINES_STARTED);

	if (frames == 0u)
		return 0;
	return nn_stream_wait(sh, gen, frames);
}

static int cmd_nn_stream_stop(struct cli_instance *sh, int argc, char **argv)
{
	struct nn_op_result res;

	(void)argc; (void)argv;

	memset(&res, 0, sizeof res);
	nn_svc_stream_stop(NN_STREAM_GEN_ANY, &res);
	/* Not running is not a failure, and both boards that had this command
	   already answered that way. */
	if (res.status == NN_SVC_ERR_STATE && res.claim == NN_CLAIM_NONE) {
		cli_warn(sh, "nn: not running\r\n");
		return 0;
	}
	nn_report(sh, "stream stop", &res);
	if (res.status != NN_SVC_OK)
		return 1;
	cli_print(sh, "nn: stopped\r\n");
	return 0;
}

static int cmd_nn_stream_stats(struct cli_instance *sh, int argc, char **argv)
{
	struct nn_stream_stats st;
	int rc;

	(void)argc; (void)argv;

	memset(&st, 0, sizeof st);
	rc = nn_stream_sample(NN_STREAM_GEN_ANY, &st);
	if (rc == NN_SVC_ERR_STALE) {
		cli_warn(sh, "nn: the stream kept changing while it was being read "
		             "-- try again\r\n");
		return 1;
	}
	if (rc != NN_SVC_OK) {
		cli_error(sh, "nn: %s (%d)\r\n", nn_status_name(rc), rc);
		return 1;
	}
	nn_stream_report_stats(sh, &st);
	nn_stream_lines(sh, NN_STREAM_LINES_STATS);
	return 0;
}

/* The argument spelling only -- the dispatcher prints the command path. */
static const char nn_stream_start_args[] =
#if NN_SVC_HAS_STREAM_TEST
	"[--test] [--frames <n>]";
#else
	"[--frames <n>]";
#endif

static const struct cli_cmd nn_stream_subcmds[] = {
	/* [!] DELIBERATELY GENEROUS (issue #99).  The count here is only a
	 * backstop: nn_stream_parse() names the option that is wrong, and a tight
	 * count would have the dispatcher refuse "--frames 5 --frames 5" with
	 * "invalid number of arguments" before the parser could say "given
	 * twice". */
	CLI_CMD_ARG_USAGE(start, NULL, "begin live inference",
	                  nn_stream_start_args, cmd_nn_stream_start, 1, 6),
	CLI_CMD(stop,  NULL, "end it", cmd_nn_stream_stop),
	CLI_CMD(stats, NULL, "rate / latency / health", cmd_nn_stream_stats),
	CLI_SUBCMD_SET_END,
};
#endif /* NN_SVC_HAS_STREAM */

/* ---- registration -------------------------------------------------------- */

#if NN_SVC_HAS_MODEL_LOAD
CLI_SUBCMD_SET_CREATE(nn_model_subcmds,
	CLI_CMD_ARG_USAGE(load, NULL, "point the model at a source",
	                  nn_model_args, cmd_nn_model_load, 2, 3),
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
	                  nn_model_args, NULL, 1, 4),
#endif
#if NN_SVC_HAS_BENCH
	CLI_CMD_ARG_USAGE(bench, NULL, "run inference [n] times, report latency",
	                  "[iterations]", cmd_nn_bench, 1, 1),
#endif
#if NN_SVC_HAS_CAMERA
	CLI_CMD(run, NULL, "one frame: capture, infer, print the boxes", cmd_nn_run),
#endif
	CLI_CMD_ARG_USAGE(out, NULL, "dequantised values of an output tensor",
	                  "[tensor] [count]", cmd_nn_out, 1, 2),
	CLI_CMD(dets, NULL, "decode the current outputs into boxes", cmd_nn_dets),
	CLI_CMD_ARG_USAGE(thresh, NULL, "detection score threshold",
	                  "[1..999]", cmd_nn_thresh, 1, 1),
#if NN_SVC_HAS_NORM
	CLI_CMD_ARG_USAGE(norm, NULL, "float input normalisation",
	                  "<0|1>", cmd_nn_norm, 1, 1),
#endif
#if NN_SVC_HAS_OVERLAY
	CLI_CMD_ARG_USAGE(overlay, NULL, "draw the boxes on the live preview",
	                  "<on|off>", cmd_nn_overlay, 1, 1),
#endif
#if NN_SVC_HAS_STREAM
	CLI_CMD_ARG_USAGE(stream, nn_stream_subcmds, "live inference",
	                  "<start|stop|stats>", NULL, 1, 4),
#endif
	CLI_SUBCMD_SET_END);

CLI_CMD_REGISTER(nn, nn_subcmds, "neural network inference", NULL, 1, 4);
