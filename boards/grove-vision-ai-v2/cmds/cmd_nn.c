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
 * FIELD OF VIEW.  The largest centred square of the frame -- 240x240 of the
 * camera's 320x240 -- is SCALED into the input (issue #48).  It used to be a
 * 128x128 centre CROP, a field of view so narrow the detector was nearly
 * useless at any normal working distance.  The resize is scalar and lives in
 * port/npu/nn_preproc.c, where it is host-tested; the vendor's is a Helium
 * routine and linking it would put predicated MVE in the image.
 */
#include "cli.h"

#include <stdint.h>
#include <string.h>

#include "WE2_device.h"   /* __get_PRIMASK / __disable_irq / __set_PRIMASK */

#include "npu.h"
#include "npu_hw.h"
#include "nn_overlay.h"
#include "nn_preproc.h"
#include "blazeface.h"
#include "camera.h"
#include "cam_imx219.h"
#include "cam_lcd_sink.h"

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
 * The arithmetic is nn_preproc's (issue #48): crop the largest centred
 * rectangle with the input's aspect ratio, scale it in, convert planar B/G/R to
 * interleaved RGB, and shift uint8 to int8.  What is left here is the part that
 * needs a shell instance to complain to.
 *
 * @param geom  filled in, because the caller needs the same geometry to report
 *              the field of view and to map boxes back to frame pixels -- one
 *              transform, computed once.
 */
static int nn_fill_input(struct cli_instance *sh, const uint8_t *raw,
                         const struct npu_tensor *in,
                         struct nn_preproc_geom *geom)
{
	uint32_t w, h;

	if (in->rank != 4 || in->dims[3] != 3) {
		cli_error(sh, "nn: model input is not HxWx3 (rank %u)\r\n", in->rank);
		return -1;
	}
	h = (uint32_t)in->dims[1];
	w = (uint32_t)in->dims[2];

	/* No "does it fit the frame" test any more: the input is SCALED, so an
	 * input larger than the frame is an ordinary upscale.  The old code
	 * refused it. */
	if (nn_preproc_geom(CAM_FRAME_WIDTH, CAM_FRAME_HEIGHT, w, h, geom) != 0) {
		cli_error(sh, "nn: cannot fit a %lux%lu input to a %ux%u frame\r\n",
		          (unsigned long)w, (unsigned long)h,
		          CAM_FRAME_WIDTH, CAM_FRAME_HEIGHT);
		return -1;
	}
	if (in->bytes < (size_t)w * h * 3u) {
		cli_error(sh, "nn: input tensor is shorter than its own shape\r\n");
		return -1;
	}

	if (nn_preproc_fill(raw, CAM_FRAME_WIDTH, CAM_FRAME_HEIGHT, geom,
	                    (uint8_t *)in->data) != 0) {
		cli_error(sh, "nn: preprocessing refused the frame\r\n");
		return -1;
	}
	return 0;
}

/* The field of view, printed with every result: a score read without knowing
 * what the model was shown is not a result. */
static void nn_print_fov(struct cli_instance *sh,
                         const struct nn_preproc_geom *g)
{
	cli_print(sh, "    %lux%lu centre crop of %ux%u at +%lu+%lu, scaled to "
	              "%lux%lu\r\n",
	          (unsigned long)g->w, (unsigned long)g->h,
	          CAM_FRAME_WIDTH, CAM_FRAME_HEIGHT,
	          (unsigned long)g->x, (unsigned long)g->y,
	          (unsigned long)g->dst_w, (unsigned long)g->dst_h);
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
                               const struct npu_tensor *in,
                               struct nn_preproc_geom *geom)
{
	int rc = camera_capture();

	if (rc != 0) {
		cli_error(sh, "nn: capture failed (%d)\r\n", rc);
		return -1;
	}
	return nn_fill_input(sh, camera_raw_frame(), in, geom);
}

static int cmd_nn_run(struct cli_instance *sh, int argc, char **argv)
{
	struct npu_tensor in, out;
	struct nn_preproc_geom geom;
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

	if (nn_capture_and_fill(sh, &in, &geom) != 0) {
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

	cli_print(sh, "nn: inference %lu ms\r\n", (unsigned long)(t1 - t0));
	nn_print_fov(sh, &geom);

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
	struct nn_preproc_geom geom;
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

	if (nn_capture_and_fill(sh, &in, &geom) != 0)
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
	 * drag float formatting into a detection report. */
	cli_print(sh, "nn: detect %lu ms, %d face(s)\r\n",
	          (unsigned long)(t1 - t0), nd);
	nn_print_fov(sh, &geom);

	for (int k = 0; k < nd; k++) {
		struct nn_preproc_box b;

		/* [!] FRAME pixels, through the same transform the overlay draws
		 * with (issue #48).  They used to be pixels of the crop, which
		 * made the console and the panel two coordinate systems that had
		 * to be reconciled by hand -- and the printed numbers were the
		 * ones that could not be checked against anything. */
		if (nn_preproc_box(&geom, det[k].x, det[k].y, det[k].w,
		                   det[k].h, &b) != 0) {
			cli_print(sh, "  #%d  box off-frame  score %ld/1000\r\n",
			          k + 1, (long)(det[k].score * 1000.0f));
			continue;
		}
		cli_print(sh, "  #%d  box %ld,%ld  %ldx%ld px  score %ld/1000\r\n",
		          k + 1, (long)b.x0, (long)b.y0,
		          (long)(b.x1 - b.x0), (long)(b.y1 - b.y0),
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

/* --- live preview with boxes (issue #48) ---------------------------------- */

/*
 * `nn preview`: the camera on the panel, with the faces drawn on it.
 *
 * The work happens on the CAMERA PRODUCER THREAD, inside the sink's consume()
 * (port/npu/nn_overlay.c).  This function only starts it, waits, and stops it.
 * That is why the whole body is bracketed by the ownership gate: the gate is a
 * lease on the NPU held by this command while another thread uses it, which is
 * what keeps `nn close` from dismantling an interpreter mid-frame.
 *
 * [!] THE TEARDOWN IS NOT SYMMETRIC WITH THE SETUP, on purpose.  A stop that
 * does not return CAM_OK means the producer is still running, so the sink is
 * NOT detached and the gate is NOT released -- see camera.h.  Leaving `nn`
 * leased until reboot is the correct trade against a producer executing inside
 * a sink somebody just unlinked.
 */
static int cmd_nn_preview(struct cli_instance *sh, int argc, char **argv)
{
	struct npu_tensor in;
	struct npu_tensor outs[NN_MAX_OUTPUTS];
	struct nn_overlay_stats ns;
	struct camera_stats st;
	uint32_t frames = 0u;   /* 0 = until Ctrl+C, as `camera preview` */
	uint32_t before;
	unsigned n_out, i;
	ULONG t0, t1, ticks;
	int rc, stop_rc;

	if (argc > 1 && cli_parse_u32(argv[1], &frames) != 0) {
		cli_error(sh, "nn: bad frame count '%s'\r\n", argv[1]);
		return -1;
	}

	if (!nn_try_acquire()) {
		cli_error(sh, "nn: busy\r\n");
		return -1;
	}
	if (!nn_open_done) {
		cli_error(sh, "nn: no model open (nn open det)\r\n");
		nn_release();
		return -1;
	}

	/*
	 * [!] EVERY CHECK BEFORE THE STREAM STARTS.
	 *
	 * A preview that starts and then fails on every frame is a panel
	 * showing a live picture with no boxes and no explanation -- the exact
	 * failure this whole command exists to make visible.  So the input
	 * quantisation and the output shapes are settled here, where refusing
	 * costs nothing and can say why.
	 */
	if (npu_input(&in) != NPU_OK) {
		cli_error(sh, "nn: no input tensor\r\n");
		nn_release();
		return -1;
	}
	if (nn_input_quant_ok(sh, &in) != 0) {
		nn_release();
		return -1;
	}
	n_out = npu_output_count();
	if (n_out > NN_MAX_OUTPUTS) {
		cli_error(sh, "nn: model has %u outputs, this command reads %u\r\n",
		          n_out, (unsigned)NN_MAX_OUTPUTS);
		nn_release();
		return -1;
	}
	for (i = 0; i < n_out; i++)
		if (npu_output(i, &outs[i]) != NPU_OK) {
			cli_error(sh, "nn: output %u is unreadable\r\n", i);
			nn_release();
			return -1;
		}
	if (!blazeface_shapes_ok(outs, n_out)) {
		cli_error(sh, "nn: the open model is not BlazeFace-shaped "
		              "(nn open det)\r\n");
		nn_release();
		return -1;
	}

	rc = cam_lcd_sink_attach(nn_overlay_arm());
	if (rc == CAM_ERR_BUSY) {
		cli_error(sh, "nn: a preview is already running\r\n");
		nn_release();
		return -1;
	}
	if (rc != CAM_OK) {
		cli_error(sh, "nn: panel attach failed (%d)\r\n", rc);
		nn_release();
		return -1;
	}

	rc = camera_stream_start();
	if (rc != CAM_OK) {
		/*
		 * [!] CAM_ERR_BUSY IS NOT "NOTHING HAPPENED".  It is the one
		 * start failure that means a stream is ALREADY RUNNING -- and
		 * this sink is attached to it, so a producer can be inside
		 * consume() right now.  Detaching would unlink a sink mid
		 * delivery and releasing the gate would let `nn close` dismantle
		 * an interpreter the producer is using.  Every other failure
		 * (bring-up, datapath) leaves no producer, so the sink comes
		 * back off normally.
		 *
		 * Today the exclusive attach above makes this unreachable --
		 * whoever owns the stream owns the sink.  It is handled anyway
		 * because "unreachable" here rests on another module's
		 * exclusivity rather than on anything checked at this line.
		 */
		cli_error(sh, "nn: stream start failed (%d)\r\n", rc);
		if (rc != CAM_ERR_BUSY) {
			(void)cam_lcd_sink_detach();
			nn_release();
		} else {
			cli_error(sh, "nn: a stream is already running; nn stays "
			              "held\r\n");
		}
		return -1;
	}

	camera_stream_stats(&st);
	before = st.frames;
	t0 = tx_time_get();

	/* Only waiting happens here: capture, inference and the blit are all on
	 * the producer.  One poll per tick notices Ctrl+C and costs nothing
	 * against a ~115 ms frame. */
	for (;;) {
		if (cli_cancel_requested(sh))
			break;
		camera_stream_stats(&st);
		if (!st.streaming)
			break;                      /* the producer gave up */
		if (frames != 0u && (st.frames - before) >= frames)
			break;
		if (cli_sleep(sh, 1u) != 0)
			break;
	}

	t1 = tx_time_get();
	ticks = t1 - t0;

	/* [!] BEFORE the stop, always: it is what stops the frame in flight
	 * from starting an inference the join would then have to wait for. */
	nn_overlay_request_stop();
	stop_rc = camera_stream_stop();

	if (stop_rc != CAM_OK) {
		/*
		 * The producer never acknowledged.  Detaching now would unlink
		 * a sink it may be inside, so nothing is torn down and the gate
		 * is kept -- the camera has poisoned itself and says so.
		 */
		cli_error(sh, "nn: the camera did not stop (%d); it is now "
		              "unusable until reboot, and nn stays held\r\n",
		          stop_rc);
		return -1;
	}

	(void)cam_lcd_sink_detach();
	nn_release();

	if (cli_cancel_requested(sh)) {
		/* Cancelled: the shared core discards output produced while
		 * cancel_req is set, so a summary would never arrive.  The
		 * dispatcher's "^C" is the feedback, as in `camera preview`. */
		return 0;
	}

	camera_stream_stats(&st);
	nn_overlay_stats(&ns);

	if (st.fault != NULL) {
		cli_error(sh, "nn: preview stopped: %s\r\n", st.fault);
		return -1;
	}

	{
		uint32_t got = st.frames - before;
		uint32_t ms = (uint32_t)((ticks * 1000u) /
		                         TX_TIMER_TICKS_PER_SECOND);

		cli_print(sh, "%lu frame(s) in %lu ms", (unsigned long)got,
		          (unsigned long)ms);
		if (ms != 0u)
			cli_print(sh, " = %lu.%lu fps",
			          (unsigned long)(got * 1000u / ms),
			          (unsigned long)((got * 10000u / ms) % 10u));
		cli_print(sh, "\r\n");
		cli_print(sh, "%lu inference(s), %lu face(s) drawn, last %lu ms\r\n",
		          (unsigned long)ns.inferences,
		          (unsigned long)ns.detections,
		          (unsigned long)ns.last_ms);
		/* Both counts matter and neither is an error on its own: frames
		 * are skipped by a pending stop, and the panel being busy is a
		 * dropped frame rather than a failure. */
		if (ns.skipped || ns.errors)
			cli_print(sh, "%lu frame(s) skipped, %lu refused\r\n",
			          (unsigned long)ns.skipped,
			          (unsigned long)ns.errors);
	}
	return 0;
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
	CLI_CMD_ARG_USAGE(preview, NULL,
	                  "live preview with face boxes, Ctrl+C to stop",
	                  "[frames]", cmd_nn_preview, 1, 1),
	CLI_CMD_ARG_USAGE(thresh, NULL, "detection score threshold",
	                  "[<1..999>]", cmd_nn_thresh, 1, 1),
	CLI_SUBCMD_SET_END);

CLI_CMD_REGISTER(nn, nn_subcmds,
                 "Ethos-U55 inference: classification and face detection",
                 NULL, 1, 0);
