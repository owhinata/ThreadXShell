/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    cmd_nn_board.c
 * @brief   `nn stream` -- this board's background inference worker (issue #50).
 *
 * The rest of `nn` is one shared command (shell/cmds/cmd_nn.c) above a neutral
 * contract.  This subcommand is not, and issue #50 says so deliberately: a
 * worker with its own lost-stream latch, a re-arm that another console can
 * observe, and a teardown that can come back INCOMPLETE is not a formatting
 * difference.  Reconciling it with the Grove board's blocking panel preview
 * belongs in its own change.
 *
 * [!] TEMPORARY.  The destination is `nn stream start/stop/stats` on all three
 * boards over neutral start/poll/request-stop hooks -- the same grammar, one
 * implementation.  This file is the interim step.
 *
 * It lives in cmds/ rather than in the port because it needs a shell instance;
 * cmds/ is shell-layer code and may hold one, the port may not.
 *
 * The stats report is deliberately longer than a progress display needs: this
 * board deleted the donor's staging machinery on the argument that the
 * inference:ingest ratio made it unnecessary, and these counters are how the
 * board says whether that actually held.  `ingest max` against the band
 * deadline and `stream` are the two that would otherwise fail silently.
 */
#include "cli.h"

#include <stdint.h>
#include <string.h>

#include "cam_band.h"
#include "nn_camera.h"
#include "stm32h7xx_hal.h"   /* SystemCoreClock, for the cycle -> us conversion */

static const char *nn_nncam_strerror(int rc)
{
	switch (rc) {
	case NNCAM_ERR_RUNNING: return "a stream is already running (`nn stream stats`)";
	case NNCAM_ERR_NOTRUN:  return "not running";
	case NNCAM_ERR_MODEL:   return "no model loaded, or it has no usable input "
	                               "tensor (`blob list`, then `nn model load --slot <n>`)";
	case NNCAM_ERR_SESSION: return "the NN session is busy (`nn bench` or "
	                               "`nn model load` is running)";
	case NNCAM_ERR_PSRAM:   return "PSRAM not ready, or OCTOSPI1 is held by a "
	                               "psram/membench/devmem/wifi flash command";
	case NNCAM_ERR_BAND:    return "the camera would not start a band stream -- a "
	                               "frame stream may own the DCMI "
	                               "(`camera stream stop`), the other console may be "
	                               "starting or stopping it, or see `dmesg`";
	case NNCAM_ERR_GEOM:    return "the model input does not tile onto the camera's "
	                               "4 bands, or its dtype is neither int8 nor "
	                               "float32 (`nn info`)";
	case NNCAM_ERR_QUANT:   return "the int8 input carries no per-tensor quantization "
	                               "scale (`nn info` shows q(s=0.000000)) -- a "
	                               "per-axis quantized input is not supported";
	case NNCAM_ERR_INIT:    return "the worker thread or its objects could not be "
	                               "created";
	case NNCAM_ERR_TEARING: return "still tearing down (a callback or an inference "
	                               "has not returned) -- run `nn stream stop` again";
	case NNCAM_ERR_REARM:   return "the stream could not be re-armed (the DCMI may "
	                               "be owned elsewhere) -- run `nn stream stop`, "
	                               "then `nn stream start`";
	default:                return "unknown error";
	}
}

static uint32_t nn_cyc_to_us(uint32_t cyc)
{
	uint32_t mhz = SystemCoreClock / 1000000u;

	return mhz ? (cyc / mhz) : 0u;
}

static void nn_stream_order_note(struct cli_instance *sh)
{
#if BSP_ENABLE_LCD
	if (!cam_band_claimed(CAM_BAND_PREVIEW))
		cli_print(sh, "note    : the OCTOSPI1 guard is held for the stream's "
		          "lifetime, so `camera preview on` is refused until "
		          "`nn stream stop` -- start the preview FIRST if you want to "
		          "see the boxes\r\n");
#else
	(void)sh;
#endif
}

static int cmd_nn_stream_start(struct cli_instance *sh, int argc, char **argv)
{
	int colorbar = 0, rc, rearm;

	if (argc > 1) {
		if (strcmp(argv[1], "test") != 0) {
			cli_error(sh, "nn: usage: nn stream start [test]\r\n");
			return 1;
		}
		colorbar = 1;
	}
	/* Sampled before the call, because a successful re-arm clears the latch. */
	rearm = nn_camera_running() && cam_band_stream_lost();

	rc = nn_camera_start(colorbar);
	if (rc != NNCAM_OK) {
		cli_error(sh, "nn: %s\r\n", nn_nncam_strerror(rc));
		return 1;
	}
	if (rearm) {
		/* Say which of the two happened.  "started" over a stream that was only
		   re-armed would hide that an outage occurred at all -- and the counters
		   deliberately keep running across it, so they would not show it either. */
		cli_print(sh, "nn: stream re-armed after a lost stream "
		          "(counters continue; `nn stream stop` to reset them)\r\n");
		return 0;
	}
	cli_print(sh, "nn: inference stream started (worker prio 18%s)\r\n",
	          colorbar ? ", colorbar" : "");
	nn_stream_order_note(sh);
	return 0;
}

static int cmd_nn_stream_stop(struct cli_instance *sh, int argc, char **argv)
{
	int rc;

	(void)argc; (void)argv;

	rc = nn_camera_stop();
	if (rc == NNCAM_ERR_NOTRUN) {
		cli_warn(sh, "nn: not running\r\n");
		return 0;
	}
	if (rc != NNCAM_OK) {
		cli_error(sh, "nn: %s\r\n", nn_nncam_strerror(rc));
		return 1;
	}
	cli_print(sh, "nn: stopped\r\n");
	return 0;
}

static int cmd_nn_stream_stats(struct cli_instance *sh, int argc, char **argv)
{
	struct nn_camera_stats st;
	struct bf_det dets[BF_MAX_DET];
	uint32_t fps_x100 = 0u;
	int n;

	(void)argc; (void)argv;

	nn_camera_stats_get(&st);
	if (st.elapsed_ms)
		fps_x100 = (uint32_t)((uint64_t)st.infers * 100000u / st.elapsed_ms);

	cli_print(sh, "state   : %s\r\n", st.running ? "running" : "stopped");
	cli_print(sh, "session : %s\r\n",
	          st.holds_guards ? "held (NN + OCTOSPI1)" : "free");
	cli_print(sh, "stream  : %s\r\n",
	          st.stream_lost ? "LOST -- re-issue `nn stream start` to re-arm"
	                         : (st.running ? "ok" : "-"));
	cli_print(sh, "infers  : %lu in %lu ms  (%lu.%02lu inf/s)\r\n",
	          (unsigned long)st.infers, (unsigned long)st.elapsed_ms,
	          (unsigned long)(fps_x100 / 100u), (unsigned long)(fps_x100 % 100u));
	cli_print(sh, "frames  : %lu ingested, %lu skipped (worker busy), %lu error(s)\r\n",
	          (unsigned long)st.frames, (unsigned long)st.skipped,
	          (unsigned long)st.errors);
	/* The ownership invariant, reported rather than assumed (owhinata/wio-lite-ai#54).
    `raced` must be 0;
	 * anything else means part of the tensor the model saw was activations, not
	 * camera.  `stale` counts the frame posts the pre-arm drain discarded -- each one
	 * is a race that would have started and then never stopped. */
	cli_print(sh, "tensor  : %lu raced (must be 0), %lu stale post(s) dropped\r\n",
	          (unsigned long)st.raced, (unsigned long)st.stale_posts);
	cli_print(sh, "ingest  : last %lu us  max %lu us  (band deadline ~18500 us)\r\n",
	          (unsigned long)nn_cyc_to_us(st.ingest_last_cyc),
	          (unsigned long)nn_cyc_to_us(st.ingest_max_cyc));
	cli_print(sh, "latency : %lu us  (%lu cycles)\r\n",
	          (unsigned long)nn_cyc_to_us(st.infer_last_cyc),
	          (unsigned long)st.infer_last_cyc);
	cli_print(sh, "norm    : %s   overlay: %s\r\n",
	          st.norm_signed ? "[-1,1]" : "[0,1]", st.overlay ? "on" : "off");

	return 0;
}

/*
 * Exported, not static: the shared command's table names this set under the
 * board's NN_SVC_HAS_STREAM capability.  It is the one symbol that crosses.
 */
const struct cli_cmd nn_board_stream_subcmds[] = {
	CLI_CMD_ARG_USAGE(start, NULL, "claim the band stream and infer continuously",
	                  "usage: nn stream start [test]\r\n",
	                  cmd_nn_stream_start, 1, 1),
	CLI_CMD(stop,  NULL, "stop inferring and release the stream",
	        cmd_nn_stream_stop),
	CLI_CMD(stats, NULL, "fps / ingest cost / stream health",
	        cmd_nn_stream_stats),
	CLI_SUBCMD_SET_END,
};
