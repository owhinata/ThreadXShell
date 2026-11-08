/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * A stand-in for a board's net_svc_config.h (issue #219), so
 * shell/test/test_net_cmd.c can build the shared `net` command in each shape a
 * board may declare.  run_host_tests.sh picks the shape:
 *
 *   NET_TEST_SHAPE_STALE  the lease-stale mark and one board subcommand
 *                         (`extra`, with a leaf table) -- wio-like
 *   NET_TEST_SHAPE_BARE   neither (no board subcommand at all)
 *   NET_TEST_SHAPE_DUP    STALE, but the board also registers `info` -- the
 *                         shadowing the table check must catch
 *
 * The real boards' tables are checked against their REAL config headers by
 * boards/<board>/test/host_tests.sh.
 */
#ifndef NET_SVC_CONFIG_H
#define NET_SVC_CONFIG_H

#define NET_SVC_HELP "test network"

#if defined(NET_TEST_SHAPE_STALE) || defined(NET_TEST_SHAPE_DUP)
#define NET_SVC_HAS_LEASE_STALE 1
#define NET_SVC_BOARD_DECLS                                                  \
	extern const struct cli_cmd test_net_extra_subcmds[];                \
	int test_net_board_cmd(struct cli_instance *sh, int argc, char **argv);
#if defined(NET_TEST_SHAPE_DUP)
#define NET_SVC_BOARD_SUBCMDS                                                \
	CLI_CMD(extra, test_net_extra_subcmds, "board: extra", NULL),        \
	CLI_CMD(info, NULL, "board: a second info", test_net_board_cmd),
#else
#define NET_SVC_BOARD_SUBCMDS                                                \
	CLI_CMD(extra, test_net_extra_subcmds, "board: extra", NULL),
#endif
#elif defined(NET_TEST_SHAPE_BARE)
#define NET_SVC_HAS_LEASE_STALE 0
#define NET_SVC_BOARD_DECLS
#define NET_SVC_BOARD_SUBCMDS
#else
#error "build with -DNET_TEST_SHAPE_STALE, _BARE or _DUP"
#endif

#endif /* NET_SVC_CONFIG_H */
