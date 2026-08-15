/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    cmd_nn.c
 * @brief   `nn` command: one-shot Ethos-U55 classification (issues #44, #40).
 *
 *   nn info               what is loaded, and the arena budget
 *   nn open [<addr>]      bring the NPU up and parse the model in flash
 *   nn close              release it
 *   nn run                capture one frame, preprocess, infer, print top-5
 *   nn detect             the same, decoded as BlazeFace face boxes (#45)
 *   nn thresh [<milli>]   the detector's score threshold
 *
 * SINGLE OWNER.  The interpreter, the driver state, the camera buffer and the
 * arena are all static, and the shell runs foreground and background jobs
 * concurrently (`nn run &` while another command types).  So this carries an
 * ownership gate of exactly the shape CoreMark's does -- a PRIMASK-guarded
 * test-and-set, not a ThreadX object, because it also has to protect the
 * open/close transitions that create and destroy those objects.  bench_gate is
 * not this: it checks that the clock a benchmark divides by is trustworthy,
 * which is a different question.
 *
 * CACHE.  Not here any more (issue #46).  Maintenance is the port's, in
 * npu_cache.c, hung off the driver's inference_begin/end callbacks -- the only
 * two instants where the arena's owner actually changes hands, both of them
 * inside Invoke().  From out here every choice is wrong: cleaning the input
 * before the call is too early (the ethos-u kernel writes arena scratch after
 * it) and invalidating outputs after it is too late (TFLM writes the arena
 * before Invoke() returns).
 *
 * FIELD OF VIEW.  The camera delivers 320x240 and the model wants a square
 * input, so the frame is CENTRE-CROPPED, not resized.  The vendor's resize is
 * a Helium routine (hx_lib_image_resize_..._helium) and linking it would put
 * predicated MVE in the image; a scalar resize is a thing to write later if the
 * crop's narrower field of view turns out to matter.  It is stated in the
 * output so nobody reads a score without knowing.
 */
#include "cli.h"

#include <stdint.h>
#include <string.h>

#include "WE2_device.h"   /* __get_PRIMASK / __disable_irq / __set_PRIMASK */

#include "npu.h"
#include "npu_hw.h"
#include "blazeface.h"
#include "camera.h"
#include "cam_imx219.h"

#include "tx_api.h"        /* tx_time_get(): ThreadX ticks, 1 ms here */

/*
 * Where the models live.
 *
 * The OFFSETS come from board.cmake -- the same cache variables the
 * flash-model-* targets write to -- so there is one address per model and not
 * a constant here plus a constant there kept in step by a comment (issue #45).
 * The base is the chip's memory-mapped flash read alias, which is a property of
 * the part rather than of the layout.
 *
 * `nn open` takes either name or a raw address; the raw form stays because the
 * partition table is a convention and `nn open <addr>` is how you check a model
 * somebody put somewhere else.
 */
#define NN_FLASH_READ_BASE    0x3A000000u
#define NN_MODEL_CLS_ADDR     (NN_FLASH_READ_BASE + NN_MODEL_CLS_OFFSET)
#define NN_MODEL_DET_ADDR     (NN_FLASH_READ_BASE + NN_MODEL_DET_OFFSET)
#define NN_MODEL_ADDR_DEFAULT NN_MODEL_CLS_ADDR

/* --- ownership ----------------------------------------------------------- */

static volatile uint8_t nn_busy;

static int nn_try_acquire(void)
{
	uint32_t pm = __get_PRIMASK();
	int ok;

	__disable_irq();
	ok = !nn_busy;
	if (ok)
		nn_busy = 1u;
	__set_PRIMASK(pm);
	return ok;
}

static void nn_release(void)
{
	nn_busy = 0u;
}

/* --- state --------------------------------------------------------------- */

static uint8_t  nn_open_done;
static uint32_t nn_model_addr;

/* --- preprocessing ------------------------------------------------------- */

/*
 * Camera frame -> input tensor.
 *
 * The frame is 320x240 PLANAR B/G/R (that is what WDMA3 writes -- see the
 * camera port); the model wants interleaved RGB.  Both conversions happen in
 * this one pass, along with the centre crop and the uint8->int8 shift the
 * quantisation asks for.
 *
 * No vectorisation here even by accident: the file carries
 * -fno-tree-vectorize, for the reason at the top of board.cmake's tflm_obj
 * block.
 */
static int nn_fill_input(struct cli_instance *sh, const uint8_t *raw,
                         const struct npu_tensor *in)
{
	const uint32_t fw = CAM_FRAME_WIDTH, fh = CAM_FRAME_HEIGHT;
	const uint32_t plane = fw * fh;
	uint32_t w, h, x0, y0;
	uint8_t *dst = (uint8_t *)in->data;

	if (in->rank != 4 || in->dims[3] != 3) {
		cli_error(sh, "nn: model input is not HxWx3 (rank %u)\r\n", in->rank);
		return -1;
	}
	h = (uint32_t)in->dims[1];
	w = (uint32_t)in->dims[2];
	if (w == 0u || h == 0u || w > fw || h > fh) {
		cli_error(sh, "nn: %lux%lu input does not fit a %lux%lu frame\r\n",
		          (unsigned long)w, (unsigned long)h,
		          (unsigned long)fw, (unsigned long)fh);
		return -1;
	}
	if (in->bytes < (size_t)w * h * 3u) {
		cli_error(sh, "nn: input tensor is shorter than its own shape\r\n");
		return -1;
	}

	x0 = (fw - w) / 2u;
	y0 = (fh - h) / 2u;

	for (uint32_t y = 0; y < h; y++) {
		const uint8_t *row = raw + (size_t)(y0 + y) * fw + x0;

		for (uint32_t x = 0; x < w; x++) {
			uint8_t b = row[x];
			uint8_t g = row[x + plane];
			uint8_t r = row[x + 2u * plane];

			/* uint8 -> int8 by shifting the range, which is what an int8 model
			 * quantised around zero expects.  Done as a wrap on purpose: the
			 * bit pattern is the answer, the arithmetic is not. */
			*dst++ = (uint8_t)(r - 128u);
			*dst++ = (uint8_t)(g - 128u);
			*dst++ = (uint8_t)(b - 128u);
		}
	}
	return 0;
}

/* --- reporting ----------------------------------------------------------- */

static void nn_print_tensor(struct cli_instance *sh, const char *what,
                            const struct npu_tensor *t)
{
	cli_print(sh, "%-6s %s", what, npu_type_name(t->type));
	for (unsigned i = 0; i < t->rank; i++)
		cli_print(sh, "%s%ld", i == 0u ? " [" : "x", (long)t->dims[i]);
	cli_print(sh, "]  %lu B  scale %d/1e6  zp %ld\r\n",
	          (unsigned long)t->bytes, (int)(t->scale * 1000000.0f),
	          (long)t->zero_point);
}

/* Top-N over an int8 output vector, by insertion -- N is 5 and the vector is a
 * class count, so nothing cleverer earns its code size. */
static void nn_print_top(struct cli_instance *sh, const struct npu_tensor *out)
{
	enum { TOP_N = 5 };
	int      best_i[TOP_N];
	int8_t   best_v[TOP_N];
	unsigned n = 0;
	const int8_t *v = (const int8_t *)out->data;
	size_t count = out->bytes;

	for (unsigned i = 0; i < TOP_N; i++) {
		best_i[i] = -1;
		best_v[i] = -128;
	}
	for (size_t i = 0; i < count; i++) {
		for (unsigned k = 0; k < TOP_N; k++) {
			if (v[i] > best_v[k]) {
				for (unsigned j = TOP_N - 1u; j > k; j--) {
					best_v[j] = best_v[j - 1u];
					best_i[j] = best_i[j - 1u];
				}
				best_v[k] = v[i];
				best_i[k] = (int)i;
				if (n < TOP_N)
					n++;
				break;
			}
		}
	}
	for (unsigned k = 0; k < n; k++) {
		/* Dequantised score, in thousandths, so no %f is needed on a path that
		 * would otherwise pull float printf into an inference report. */
		long milli = (long)(((float)best_v[k] - (float)out->zero_point)
		                    * out->scale * 1000.0f);

		cli_print(sh, "  #%u  class %-4d  raw %4d  score %ld/1000\r\n",
		          k + 1u, best_i[k], (int)best_v[k], milli);
	}
}

/* --- subcommands --------------------------------------------------------- */

static int cmd_nn_info(struct cli_instance *sh, int argc, char **argv)
{
	struct npu_tensor t;

	(void)argc;
	(void)argv;

	/* [!] BEHIND THE GATE, like every other subcommand (issue #45).  It used
	 * not to be, which was wrong rather than merely untidy: nn_busy also
	 * covers open and close, and the teardown rewrites npu_hw_ready() and the
	 * wrapped-IRQ set that the lines below walk.  Reading hardware state while
	 * another job is dismantling it is a race, and the report it produces is
	 * of a machine that no longer exists.
	 *
	 * When the gate refuses, only facts that CANNOT be in flight are printed:
	 * the arena reservation is a link-time constant. */
	if (!nn_try_acquire()) {
		cli_print(sh, "npu      busy (another nn job holds it)\r\n");
		cli_print(sh, "arena    %lu B reserved @%p\r\n",
		          (unsigned long)npu_arena_bytes(), npu_arena_base());
		return 0;
	}

	cli_print(sh, "npu      %s\r\n",
	          npu_hw_ready() ? "up (secure, privileged)" : "down");
	if (!npu_hw_ready() && npu_hw_fail_reason() != NULL)
		cli_print(sh, "         last refusal: %s\r\n", npu_hw_fail_reason());
	cli_print(sh, "arena    %lu B reserved @%p\r\n",
	          (unsigned long)npu_arena_bytes(), npu_arena_base());

	if (npu_hw_ready()) {
		int lines[8];
		unsigned n = npu_hw_wrapped_irqs(lines, 8u);

		/* The EPK rule is that nothing may be enabled but unaccounted.  The
		 * bring-up enforces it by refusing, so this is the state of that
		 * enforcement rather than a check -- but an invariant with no readout
		 * is one nobody verifies. */
		cli_print(sh, "irq      ");
		for (unsigned i = 0; i < n; i++)
			cli_print(sh, "%s%d", i ? ", " : "", lines[i]);
		cli_print(sh, "%s (wrapped for cpu%% accounting)\r\n",
		          n ? "" : "none");
	}

	cli_print(sh, "detect   score threshold %u/1000 (nn thresh <1..999>)\r\n",
	          blazeface_get_thresh_milli());

	if (!nn_open_done) {
		cli_print(sh, "model    not open (nn open [<addr>])\r\n");
		nn_release();
		return 0;
	}
	cli_print(sh, "model    0x%08lx\r\n", (unsigned long)nn_model_addr);
	cli_print(sh, "arena    %lu B used by this layout\r\n",
	          (unsigned long)npu_arena_used());
	if (npu_input(&t) == NPU_OK)
		nn_print_tensor(sh, "input", &t);
	for (unsigned i = 0; i < npu_output_count(); i++)
		if (npu_output(i, &t) == NPU_OK)
			nn_print_tensor(sh, "output", &t);
	nn_release();
	return 0;
}

static int cmd_nn_open(struct cli_instance *sh, int argc, char **argv)
{
	uint32_t addr = NN_MODEL_ADDR_DEFAULT;
	int rc;

	if (argc > 1) {
		/* The two partition names before the numeric form, so a name can never
		 * be mistaken for a hex address. */
		if (strcmp(argv[1], "cls") == 0) {
			addr = NN_MODEL_CLS_ADDR;
		} else if (strcmp(argv[1], "det") == 0) {
			addr = NN_MODEL_DET_ADDR;
		} else if (cli_parse_u32(argv[1], &addr) != 0) {
			cli_error(sh, "nn: bad address '%s' (want cls, det or an address)\r\n",
			          argv[1]);
			return -1;
		}
	}
	if (!nn_try_acquire()) {
		cli_error(sh, "nn: busy\r\n");
		return -1;
	}
	if (nn_open_done) {
		cli_error(sh, "nn: already open (nn close first)\r\n");
		nn_release();
		return -1;
	}

	if (npu_hw_init() != 0) {
		cli_error(sh, "nn: %s\r\n",
		          npu_hw_fail_reason() ? npu_hw_fail_reason() : "bring-up failed");
		nn_release();
		return -1;
	}

	rc = npu_open(addr, npu_arena_base(), npu_arena_bytes());
	if (rc != NPU_OK) {
		cli_error(sh, "nn: %s (0x%08lx)\r\n", npu_status_name(rc),
		          (unsigned long)addr);
		/* Leave the hardware down too: an NPU that is up with no model is a
		 * state nothing below here would ever use. */
		npu_hw_deinit();
		nn_release();
		return -1;
	}

	nn_open_done  = 1u;
	nn_model_addr = addr;
	cli_print(sh, "nn: model at 0x%08lx open, arena %lu/%lu B\r\n",
	          (unsigned long)addr, (unsigned long)npu_arena_used(),
	          (unsigned long)npu_arena_bytes());
	nn_release();
	return 0;
}

static int cmd_nn_close(struct cli_instance *sh, int argc, char **argv)
{
	(void)argc;
	(void)argv;

	if (!nn_try_acquire()) {
		cli_error(sh, "nn: busy\r\n");
		return -1;
	}
	npu_close();
	npu_hw_deinit();
	nn_open_done = 0u;
	nn_release();
	cli_print(sh, "nn: closed\r\n");
	return 0;
}

/*
 * One frame from the camera into the model's input tensor.
 *
 * Shared by `nn run` and `nn detect`, which differ only in how the OUTPUT is
 * read.  camera_capture() quiesces the datapath on both the success and the
 * failure path, so the camera is never left streaming into a buffer the NPU is
 * about to be pointed at; it also refuses while a preview is running, which is
 * the camera/NPU concurrency question answered by the layer that owns it.
 */
static int nn_capture_and_fill(struct cli_instance *sh,
                               const struct npu_tensor *in)
{
	int rc = camera_capture();

	if (rc != 0) {
		cli_error(sh, "nn: capture failed (%d)\r\n", rc);
		return -1;
	}
	return nn_fill_input(sh, camera_raw_frame(), in);
}

static int cmd_nn_run(struct cli_instance *sh, int argc, char **argv)
{
	struct npu_tensor in, out;
	uint32_t t0, t1;
	int rc;

	(void)argc;
	(void)argv;

	if (!nn_try_acquire()) {
		cli_error(sh, "nn: busy\r\n");
		return -1;
	}
	if (!nn_open_done) {
		cli_error(sh, "nn: no model open\r\n");
		nn_release();
		return -1;
	}
	if (npu_input(&in) != NPU_OK) {
		cli_error(sh, "nn: no input tensor\r\n");
		nn_release();
		return -1;
	}

	if (nn_capture_and_fill(sh, &in) != 0) {
		nn_release();
		return -1;
	}

	/* No cache maintenance here (issue #46).  It moved into the port, where the
	 * driver's own lifecycle callbacks put it at the only two instants that are
	 * correct: the whole arena is cleaned immediately before the command stream
	 * is launched, and invalidated once completion is confirmed -- both before
	 * TFLM resumes.  Anything done from out here is either too early (the
	 * ethos-u kernel writes arena scratch after it) or too late (TFLM writes the
	 * arena before Invoke() returns). */
	t0 = (uint32_t)tx_time_get();
	rc = npu_invoke();
	t1 = (uint32_t)tx_time_get();

	if (rc != NPU_OK) {
		cli_error(sh, "nn: %s\r\n", npu_status_name(rc));
		nn_release();
		return -1;
	}

	cli_print(sh, "nn: inference %lu ms (centre crop %ldx%ld of %ux%u; "
	              "field of view is narrower than the preview)\r\n",
	          (unsigned long)(t1 - t0), (long)in.dims[2], (long)in.dims[1],
	          CAM_FRAME_WIDTH, CAM_FRAME_HEIGHT);

	/* The outputs are already visible: the port invalidated the whole arena
	 * once completion was confirmed, before TFLM resumed. */
	if (npu_output(0u, &out) == NPU_OK)
		nn_print_top(sh, &out);

	nn_release();
	return 0;
}

/* --- face detection (issue #45) ------------------------------------------- */

/* Enough descriptors for any model this port will hand to a decoder; BlazeFace
 * needs four.  A model with more outputs than this is reported rather than
 * silently truncated, because a decoder that locates tensors by shape would
 * otherwise fail with "not BlazeFace-shaped" and never say why. */
#define NN_MAX_OUTPUTS 8

/*
 * Does the model's input quantisation match what nn_fill_input() produces?
 *
 * [!] THIS IS NOT A FORMALITY.  nn_fill_input() writes `pixel - 128` -- a fixed
 * shift, not a quantisation using the tensor's parameters -- which is exactly
 * right for scale 1/255 with zero point -128 and progressively wrong for
 * anything else.  BlazeFace-front 128 is quantised that way, and being wrong
 * here does not look like an error: the boxes are still boxes, just in the
 * wrong places, on an image nobody can see.  So it is checked, and a mismatch
 * refuses rather than warns.
 *
 * The comparison is on the scale in millionths so that no float formatting is
 * needed on this path; 1/255 is 3921.6e-6 and the window is about one percent,
 * which distinguishes "this model" from "a differently quantised model" without
 * depending on how the converter rounded.
 */
static int nn_input_quant_ok(struct cli_instance *sh, const struct npu_tensor *in)
{
	long micro = (long)(in->scale * 1000000.0f + 0.5f);

	if (!npu_tensor_is_int8(in->type)) {
		cli_error(sh, "nn: detect needs an int8 input, this model has %s\r\n",
		          npu_type_name(in->type));
		return -1;
	}
	if (in->zero_point != -128 || micro < 3882 || micro > 3961) {
		cli_error(sh, "nn: this model wants scale %ld/1e6 zp %ld, but the frame "
		              "is filled as (pixel - 128), which is scale 3922/1e6 "
		              "zp -128\r\n",
		          micro, (long)in->zero_point);
		return -1;
	}
	return 0;
}

static int cmd_nn_detect(struct cli_instance *sh, int argc, char **argv)
{
	struct npu_tensor in;
	struct npu_tensor outs[NN_MAX_OUTPUTS];
	struct bf_det det[BF_MAX_DET];
	unsigned n_out, i;
	uint32_t t0, t1;
	long peak;
	int rc = -1, nd;

	(void)argc;
	(void)argv;

	if (!nn_try_acquire()) {
		cli_error(sh, "nn: busy\r\n");
		return -1;
	}

	/* One exit from here down, so the gate is released exactly once however
	 * this ends -- the failure paths outnumber the success path. */
	if (!nn_open_done) {
		cli_error(sh, "nn: no model open\r\n");
		goto out;
	}
	if (npu_input(&in) != NPU_OK) {
		cli_error(sh, "nn: no input tensor\r\n");
		goto out;
	}
	if (nn_input_quant_ok(sh, &in) != 0)
		goto out;

	n_out = npu_output_count();
	if (n_out > NN_MAX_OUTPUTS) {
		cli_error(sh, "nn: model has %u outputs, this command reads %u\r\n",
		          n_out, (unsigned)NN_MAX_OUTPUTS);
		goto out;
	}
	for (i = 0; i < n_out; i++)
		if (npu_output(i, &outs[i]) != NPU_OK) {
			cli_error(sh, "nn: output %u is unreadable\r\n", i);
			goto out;
		}

	if (nn_capture_and_fill(sh, &in) != 0)
		goto out;

	/* No cache maintenance here either -- see the note in cmd_nn_run(). */
	t0 = (uint32_t)tx_time_get();
	if (npu_invoke() != NPU_OK) {
		cli_error(sh, "nn: inference failed\r\n");
		goto out;
	}
	t1 = (uint32_t)tx_time_get();

	/* The descriptors were taken before the run; the POINTERS in them are
	 * arena addresses that do not move, and the port made the contents visible
	 * before Invoke() returned. */
	nd = blazeface_decode(outs, n_out, det, BF_MAX_DET);
	if (nd < 0) {
		cli_error(sh, "nn: the open model is not BlazeFace-shaped (wants 1x512x16, "
		              "1x512x1, 1x384x16 and 1x384x1 int8 outputs)\r\n");
		goto out;
	}

	/* Integers only, all the way down: no %f on a path that would otherwise
	 * drag float formatting into a detection report.  Boxes are in pixels of
	 * the CROP, and the crop's origin in the frame is printed with them so the
	 * two coordinate systems are never confused. */
	cli_print(sh, "nn: detect %lu ms, %d face(s)\r\n",
	          (unsigned long)(t1 - t0), nd);
	cli_print(sh, "    %ldx%ld centre crop of %ux%u at +%lu+%lu; field of view "
	              "is narrower than the preview\r\n",
	          (long)in.dims[2], (long)in.dims[1],
	          CAM_FRAME_WIDTH, CAM_FRAME_HEIGHT,
	          (unsigned long)((CAM_FRAME_WIDTH - (uint32_t)in.dims[2]) / 2u),
	          (unsigned long)((CAM_FRAME_HEIGHT - (uint32_t)in.dims[1]) / 2u));

	for (int k = 0; k < nd; k++) {
		long w = (long)(det[k].w * (float)in.dims[2]);
		long h = (long)(det[k].h * (float)in.dims[1]);

		cli_print(sh, "  #%d  box %ld,%ld  %ldx%ld px  score %ld/1000\r\n",
		          k + 1,
		          (long)(det[k].x * (float)in.dims[2]),
		          (long)(det[k].y * (float)in.dims[1]), w, h,
		          (long)(det[k].score * 1000.0f));
	}

	/* [!] ALWAYS, even at zero detections -- especially at zero detections.
	 * "no faces" and "the threshold is above everything the model produced"
	 * are different states and the count alone cannot tell them apart.  The
	 * peak is over ALL 896 anchors (blazeface.c never cuts the scan short), so
	 * it is the real maximum and not the maximum of a prefix. */
	peak = (long)(blazeface_last_max_score() * 1000.0f);
	cli_print(sh, "    peak raw %ld/1000, threshold %ld/1000 raw "
	              "(= %u/1000 after sigmoid)\r\n",
	          peak, (long)(blazeface_get_thresh_logit() * 1000.0f),
	          blazeface_get_thresh_milli());
	cli_print(sh, "    %d anchor(s) over threshold, %d kept of %d, %d after NMS\r\n",
	          blazeface_last_npass(), blazeface_last_nkept(),
	          (int)BF_MAX_CAND, nd);
	rc = 0;
out:
	nn_release();
	return rc;
}

static int cmd_nn_thresh(struct cli_instance *sh, int argc, char **argv)
{
	uint32_t milli;
	int rc = 0;

	/* Behind the gate because the threshold is static state that a running
	 * decode reads; setting it mid-decode would apply to part of a frame. */
	if (!nn_try_acquire()) {
		cli_error(sh, "nn: busy\r\n");
		return -1;
	}
	if (argc > 1) {
		if (cli_parse_u32(argv[1], &milli) != 0) {
			cli_error(sh, "nn: bad threshold '%s'\r\n", argv[1]);
			rc = -1;
			goto out;
		}
		/* 0 and 1000 are the poles of the inverse sigmoid: the logit the
		 * decoder compares against is undefined there, so they are refused
		 * rather than clamped -- a silently clamped threshold is a setting
		 * that does not do what it says. */
		if (blazeface_set_thresh_milli(milli) != 0) {
			cli_error(sh, "nn: threshold must be 1..999 (milli-probability)\r\n");
			rc = -1;
			goto out;
		}
	}
	cli_print(sh, "nn: detect threshold %u/1000 (raw %ld/1000)\r\n",
	          blazeface_get_thresh_milli(),
	          (long)(blazeface_get_thresh_logit() * 1000.0f));
out:
	nn_release();
	return rc;
}

CLI_SUBCMD_SET_CREATE(nn_subcmds,
	CLI_CMD(info,  NULL, "what is loaded and what the arena costs", cmd_nn_info),
	CLI_CMD_ARG_USAGE(open, NULL, "bring the NPU up and parse the model",
	                  "[cls|det|<flash-addr>]", cmd_nn_open, 1, 1),
	CLI_CMD(close, NULL, "release the model and the NPU",           cmd_nn_close),
	CLI_CMD(run,   NULL, "capture one frame and classify it",       cmd_nn_run),
	CLI_CMD(detect, NULL, "capture one frame and find faces",       cmd_nn_detect),
	CLI_CMD_ARG_USAGE(thresh, NULL, "detection score threshold",
	                  "[<1..999>]", cmd_nn_thresh, 1, 1),
	CLI_SUBCMD_SET_END);

CLI_CMD_REGISTER(nn, nn_subcmds,
                 "Ethos-U55 inference: classification and face detection",
                 NULL, 1, 0);
