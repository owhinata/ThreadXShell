/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Host stand-in for a board's svc/nor_svc_config.h, for test_nor_cmd.c (issue
 * #250): one board subcommand, `probe`, so the table's names can be checked
 * with a board entry in it.
 */
#ifndef NOR_SVC_CONFIG_H
#define NOR_SVC_CONFIG_H

#define NOR_SVC_HAS_DEVICE 1
#define NOR_SVC_HELP "the test's NOR"

#define NOR_SVC_BOARD_DECLS                                                    \
	int nor_test_probe(struct cli_instance *sh, int argc, char **argv);

#define NOR_SVC_BOARD_SUBCMDS                                                  \
	CLI_CMD_ARG(probe, NULL, "the board's own", nor_test_probe, 1, 0),

#endif /* NOR_SVC_CONFIG_H */
