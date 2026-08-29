/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    cmd_nn_board.c
 * @brief   `nn preview` -- this board's own live inference (issues #48, #50).
 *
 * The rest of `nn` is one shared command (shell/cmds/cmd_nn.c) above a neutral
 * contract.  This subcommand is not, and issue #50 says so deliberately: the
 * other two boards run live inference on a background worker with stop and
 * stats, while this one BLOCKS and draws on the panel, and reconciling those is
 * a rework of the camera-producer and panel-guard ordering that issues #48, #63,
 * #65 and #77 settled -- not a formatting difference.  Putting that inside a
 * rename would bury an untestable-by-hand concurrency change in a diff about
 * output.
 *
 * [!] TEMPORARY, AND THE FOLLOW-UP REMOVES IT.  The destination is `nn stream
 * start/stop/stats` on all three boards over neutral start/poll/request-stop
 * hooks.  This is not a second permanent grammar.
 *
 * It lives in cmds/ rather than in the port because it needs a shell instance --
 * it prints, it sleeps, it polls for Ctrl+C.  cmds/ is shell-layer code and may
 * do that; the port may not, which is why the state it reads is behind
 * nn_svc_grove.h instead of being reached for directly.
 *
 * The work itself happens on the CAMERA PRODUCER THREAD, inside the sink's
 * consume() (port/npu/nn_overlay.c).  This function only starts it, waits, and
 * stops it.
 */
#include "cli.h"

#include <stdint.h>

#include "cam_lcd_sink.h"
#include "camera.h"
#include "nn_overlay.h"
#include "nn_svc_grove.h"
#include "tx_api.h"

int nn_board_preview(struct cli_instance *sh, int argc, char **argv)
{
	struct nn_overlay_stats ns;
	struct camera_stats st;
	char why[128];
	uint32_t frames = 0u;   /* 0 = until Ctrl+C, as `camera preview` */
	uint32_t before;
	ULONG t0, t1, ticks;
	int rc, stop_rc;

	if (argc > 1 && cli_parse_u32(argv[1], &frames) != 0) {
		cli_error(sh, "nn: bad frame count '%s'\r\n", argv[1]);
		return -1;
	}

	if (!nn_svc_grove_acquire()) {
		cli_error(sh, "nn: busy (another nn job holds it)\r\n");
		return -1;
	}
	if (!nn_svc_grove_model_open()) {
		cli_error(sh, "nn: no model is loaded -- `nn model load --name det`\r\n");
		nn_svc_grove_release();
		return -1;
	}

	/*
	 * [!] EVERY CHECK BEFORE THE STREAM STARTS.  A preview that starts and then
	 * fails on every frame is a panel showing a live picture with no boxes and
	 * no explanation -- the exact failure this command exists to make visible.
	 */
	why[0] = '\0';
	if (nn_svc_grove_detector_ready(why, sizeof why) != 0) {
		cli_error(sh, "nn: %s\r\n",
		          why[0] ? why : "this model cannot annotate");
		nn_svc_grove_release();
		return -1;
	}

	/*
	 * [!] ONE CALL, AND THEREFORE ONE FAILURE (issue #63).  This used to attach
	 * the sink and then start the stream, and a start that came back BUSY meant
	 * a stream was already running WITH THIS SINK ATTACHED -- a producer could
	 * be inside consume() at that moment, so the sink could not be detached and
	 * the NPU could not be released.  The camera does both under its API mutex
	 * now, so a failure here means nothing was attached and nothing started.
	 */
	rc = cam_lcd_sink_attach_and_stream(nn_overlay_arm());
	if (rc != CAM_OK) {
		if (rc == CAM_ERR_BUSY)
			cli_error(sh, "nn: a preview is already running, or another "
			              "command owns the camera\r\n");
		else
			cli_error(sh, "nn: preview start failed (%d)\r\n", rc);
		nn_svc_grove_release();
		return -1;
	}

	camera_stream_stats(&st);
	before = st.frames;
	t0 = tx_time_get();

	/* Only waiting happens here: capture, inference and the blit are all on the
	   producer.  One poll per tick notices Ctrl+C and costs nothing against a
	   ~115 ms frame. */
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

	/* [!] BEFORE the stop, always: it is what stops the frame in flight from
	 * starting an inference the join would then have to wait for. */
	nn_overlay_request_stop();
	stop_rc = camera_stream_stop();

	if (stop_rc != CAM_OK) {
		/*
		 * No confirmed stop, so nothing is torn down and the claim is KEPT:
		 * detaching now would unlink a sink the producer may be inside.  That
		 * much is the same for every failure -- but what the operator is told
		 * is NOT, and saying the poisoned version of it for a lock collision
		 * was a plain lie (issue #65).
		 */
		if (stop_rc == CAM_ERR_LOCKED)
			cli_error(sh, "nn: the camera API stayed locked, so the stop was "
			              "never requested;\r\n"
			              "    nothing was torn down, the sink stays attached "
			              "and nn stays held.\r\n"
			              "    A reboot is what clears it\r\n");
		else
			cli_error(sh, "nn: the camera did not stop (%d); it is now "
			              "unusable until reboot, and nn stays held\r\n",
			          stop_rc);
		return -1;
	}

	/*
	 * [!] AND THE DETACH IS THE SECOND HALF OF THE STOP (issue #57).  The blit
	 * runs on the panel thread, so a confirmed producer stop no longer proves
	 * nothing is using this frame: detach unlinks the sink and then drains that
	 * thread.  A drain that does not finish means a thread may still be inside
	 * draw(), reading the detections -- and releasing the NPU there would let a
	 * later unload dismantle an interpreter underneath it.  So the claim is
	 * released only after BOTH halves are confirmed.
	 */
	stop_rc = cam_lcd_sink_detach();
	if (stop_rc != CAM_OK) {
		cli_error(sh, "nn: the panel thread did not finish (%d); the preview "
		              "is unusable until reboot, and nn stays held\r\n",
		          stop_rc);
		return -1;
	}
	nn_svc_grove_release();

	if (cli_cancel_requested(sh)) {
		/* Cancelled: the shared core discards output produced while cancel_req
		 * is set, so a summary would never arrive.  The dispatcher's "^C" is
		 * the feedback, as in `camera preview`. */
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
		uint32_t ms = (uint32_t)((ticks * 1000u) / TX_TIMER_TICKS_PER_SECOND);

		cli_print(sh, "%lu frame(s) in %lu ms", (unsigned long)got,
		          (unsigned long)ms);
		if (ms != 0u)
			cli_print(sh, " = %lu.%lu fps",
			          (unsigned long)(got * 1000u / ms),
			          (unsigned long)((got * 10000u / ms) % 10u));
		cli_print(sh, "\r\n");
		cli_print(sh, "%lu inference(s), %lu face(s) drawn, last %lu ms\r\n",
		          (unsigned long)ns.inferences, (unsigned long)ns.detections,
		          (unsigned long)ns.last_ms);
		/* Both counts matter and neither is an error on its own: frames are
		   skipped by a pending stop, and the panel being busy is a dropped
		   frame rather than a failure. */
		if (ns.skipped || ns.errors)
			cli_print(sh, "%lu frame(s) skipped, %lu refused\r\n",
			          (unsigned long)ns.skipped, (unsigned long)ns.errors);
		/* [!] Say WHICH refusal (issue #97).  The producer has no console, so
		 * this line is the only place the two can be told apart -- and one
		 * means "load a different model" while the other means "this firmware
		 * is wired wrong". */
		if (ns.model_errors || ns.decoder_errors)
			cli_print(sh, "  of those: %lu not BlazeFace-shaped, %lu decoder "
			              "fault(s)\r\n", (unsigned long)ns.model_errors,
			          (unsigned long)ns.decoder_errors);
	}
	return 0;
}
