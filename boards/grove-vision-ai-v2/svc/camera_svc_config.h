/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    camera_svc_config.h
 * @brief   What the shared `camera` command has on Grove Vision AI V2 (issue #143).
 *
 * The board says what it HAS; the shared command (shell/cmds/cmd_camera.c)
 * registers a subcommand only under its macro, and the board adapter
 * (port/camera/camera_svc_grove.c) implements only what is 1 here.  These are
 * properties, never a board identity test.  Every macro is defined, to 0 or 1,
 * so a reader of this file sees the whole set and a missing one is not
 * mistaken for a decision.
 */
#ifndef CAMERA_SVC_CONFIG_H
#define CAMERA_SVC_CONFIG_H

/** The `camera` line of the top-level `help`: what the module is. */
#define CAMERA_SVC_HELP "OV5647 camera: bring-up, capture, stream to the panel"

/** No `camera on` / `camera off`: this board has no sensor power state for
 *  them to set, so they are not registered (issue #143 decision 2).  With no
 *  `off` there is no CAMERA_SVC_OFF_STOPS_STREAM to declare. */
#define CAMERA_SVC_HAS_POWER  0

/** `camera info`: identity, state, the last capture and the values the driver
 *  last wrote -- reading no sensor register (issue #143 decision 5). */
#define CAMERA_SVC_HAS_INFO   1

/** No test pattern on this data path: `test` is refused, in the words
 *  `camera stream start test` has always used here. */
#define CAMERA_SVC_HAS_TEST   0

/** No `camera save`: this board has no filesystem. */
#define CAMERA_SVC_HAS_SAVE   0

/** `camera send`: the last `camera capture`'s planar B/G/R as it is (D4);
 *  boards/grove-vision-ai-v2/scripts/planar_bgr_to_png.py makes a PNG. */
#define CAMERA_SVC_HAS_SEND   1

/*
 * The board's own subcommands, appended to the shared `camera` table (see the
 * f746 config for the rules).  The counters are `camera stream stats` (issue
 * #143 decision 4); the producer's stage profile that `camera stats` used to
 * end with is `camera profile` (D3).  Handlers in cmds/cmd_camera_board.c.
 */
#define CAMERA_SVC_BOARD_DECLS                                                 \
	int camera_board_cmd_raw(struct cli_instance *sh, int argc, char **argv);   \
	int camera_board_cmd_profile(struct cli_instance *sh, int argc,           \
	                             char **argv);                               \
	int camera_board_cmd_vts(struct cli_instance *sh, int argc, char **argv);   \
	int camera_board_cmd_bench(struct cli_instance *sh, int argc, char **argv); \
	int camera_board_cmd_exposure(struct cli_instance *sh, int argc,          \
	                              char **argv);                              \
	int camera_board_cmd_gain(struct cli_instance *sh, int argc, char **argv);  \
	int camera_board_cmd_auto(struct cli_instance *sh, int argc, char **argv);  \
	int camera_board_cmd_black(struct cli_instance *sh, int argc, char **argv); \
	int camera_board_cmd_sat(struct cli_instance *sh, int argc, char **argv);   \
	int camera_board_cmd_gamma(struct cli_instance *sh, int argc, char **argv); \
	int camera_board_cmd_bayer(struct cli_instance *sh, int argc, char **argv); \
	int camera_board_cmd_wb(struct cli_instance *sh, int argc, char **argv);

#define CAMERA_SVC_BOARD_SUBCMDS                                               \
	CLI_CMD(raw, NULL, "capture the Bayer mosaic and name the phase",      \
	        camera_board_cmd_raw),                                         \
	CLI_CMD(profile, NULL,                                                 \
	        "the producer's time per stage since the last stream start",   \
	        camera_board_cmd_profile),                                     \
	CLI_CMD_ARG_USAGE(vts, NULL,                                           \
	                  "sensor frame length: frame rate vs exposure ceiling", \
	                  "[lines]", camera_board_cmd_vts, 1, 1),              \
	CLI_CMD_ARG_USAGE(bench, NULL,                                         \
	                  "time the datapath with the panel out of the loop",  \
	                  "[frames]", camera_board_cmd_bench, 1, 1),           \
	CLI_CMD_ARG_USAGE(exposure, NULL,                                      \
	                  "read or set the sensor's exposure",                 \
	                  "[lines]", camera_board_cmd_exposure, 1, 1),         \
	CLI_CMD_ARG_USAGE(gain, NULL,                                          \
	                  "read or set analogue / digital gain",               \
	                  "[again [dgain]]", camera_board_cmd_gain, 1, 2),     \
	CLI_CMD_ARG_USAGE(auto, NULL,                                          \
	                  "the sensor's own AEC + this port's white balance",  \
	                  "[on|off]", camera_board_cmd_auto, 1, 1),            \
	CLI_CMD_ARG_USAGE(black, NULL,                                         \
	                  "black level subtracted before the gain (default 16)", \
	                  "[n]", camera_board_cmd_black, 1, 1),                \
	CLI_CMD_ARG_USAGE(sat, NULL,                                           \
	                  "saturation, standing in for a colour matrix "       \
	                  "(256 = unchanged)",                                 \
	                  "[n]", camera_board_cmd_sat, 1, 1),                  \
	CLI_CMD_ARG_USAGE(gamma, NULL,                                         \
	                  "sRGB encode the preview (on by default)",           \
	                  "[on|off]", camera_board_cmd_gamma, 1, 1),           \
	CLI_CMD_ARG_USAGE(bayer, NULL,                                         \
	                  "demosaic phase: bggr|gbrg|grbg|rggb (try all four)", \
	                  "[mode]", camera_board_cmd_bayer, 1, 1),             \
	CLI_CMD_ARG_USAGE(wb, NULL,                                            \
	                  "software white balance (256 = unity)",              \
	                  "[r g b]", camera_board_cmd_wb, 1, 3),

#endif /* CAMERA_SVC_CONFIG_H */
