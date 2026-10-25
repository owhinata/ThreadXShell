/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * A stand-in for a board's camera_svc_config.h (issue #143), so
 * shell/test/test_camera_cmd.c can build the shared `camera` command in each
 * capability shape a board may declare.  run_host_tests.sh picks the shape:
 *
 *   CAM_TEST_SHAPE_FULL  every capability, two save media, one board
 *                        subcommand (f746 / wio-like)
 *   CAM_TEST_SHAPE_BARE  no power, no test pattern, no save (Grove-like),
 *                        two board subcommands
 *   CAM_TEST_SHAPE_MIN   power and a test pattern, but no info, save or
 *                        send, and `off` does not stop a stream (wio-like);
 *                        one board subcommand
 *   CAM_TEST_SHAPE_DUP   FULL, but the board also registers `info` -- the
 *                        shadowing the table check must catch
 *
 * The real boards' tables are checked against their REAL config headers by
 * boards/<board>/test/host_tests.sh.
 */
#ifndef CAMERA_SVC_CONFIG_H
#define CAMERA_SVC_CONFIG_H

#define CAMERA_SVC_HELP "test camera"

#define CAMERA_SVC_BOARD_DECLS                                               \
	int test_board_cmd(struct cli_instance *sh, int argc, char **argv);

#if defined(CAM_TEST_SHAPE_BARE)
#define CAMERA_SVC_HAS_POWER  0
#define CAMERA_SVC_HAS_INFO   1
#define CAMERA_SVC_HAS_TEST   0
#define CAMERA_SVC_HAS_SAVE   0
#define CAMERA_SVC_HAS_SEND   1
#define CAMERA_SVC_BOARD_SUBCMDS                                             \
	CLI_CMD(raw, NULL, "board: raw", test_board_cmd),                    \
	CLI_CMD(bench, NULL, "board: bench", test_board_cmd),
#elif defined(CAM_TEST_SHAPE_MIN)
#define CAMERA_SVC_HAS_POWER  1
#define CAMERA_SVC_OFF_STOPS_STREAM 0   /* the board refuses a running stream */
#define CAMERA_SVC_HAS_INFO   0
#define CAMERA_SVC_HAS_TEST   1
#define CAMERA_SVC_HAS_SAVE   0
#define CAMERA_SVC_HAS_SEND   0
#define CAMERA_SVC_BOARD_SUBCMDS                                             \
	CLI_CMD(scan, NULL, "board: scan", test_board_cmd),
#elif defined(CAM_TEST_SHAPE_FULL) || defined(CAM_TEST_SHAPE_DUP)
#define CAMERA_SVC_HAS_POWER  1
#define CAMERA_SVC_OFF_STOPS_STREAM 1
#define CAMERA_SVC_HAS_INFO   1
#define CAMERA_SVC_HAS_TEST   1
#define CAMERA_SVC_HAS_SAVE   1
#define CAMERA_SVC_SAVE_MEDIA(first, more) first("sd") more("fs")
#define CAMERA_SVC_HAS_SEND   1
#if defined(CAM_TEST_SHAPE_DUP)
#define CAMERA_SVC_BOARD_SUBCMDS                                             \
	CLI_CMD(scan, NULL, "board: scan", test_board_cmd),                  \
	CLI_CMD(info, NULL, "board: a second info", test_board_cmd),
#else
#define CAMERA_SVC_BOARD_SUBCMDS                                             \
	CLI_CMD(scan, NULL, "board: scan", test_board_cmd),
#endif
#else
#error "build with -DCAM_TEST_SHAPE_FULL, _BARE, _MIN or _DUP"
#endif

#endif /* CAMERA_SVC_CONFIG_H */
