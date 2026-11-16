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

/* NOR_TEST_SHAPE_ABSENT: a board with none of the three. */
#ifdef NOR_TEST_SHAPE_ABSENT
#define NOR_SVC_HAS_READ  0
#define NOR_SVC_HAS_WRITE 0
#define NOR_SVC_HAS_TEST  0
#define NOR_SVC_NO_READ_SAYS  "the fake's read hint"
#define NOR_SVC_NO_WRITE_SAYS ""
#define NOR_SVC_NO_TEST_SAYS  "use X"
#else
#define NOR_SVC_HAS_READ  1
#define NOR_SVC_HAS_WRITE 1
#define NOR_SVC_HAS_TEST  1
#endif

#define NOR_SVC_BOARD_DECLS                                                    \
	int nor_test_probe(struct cli_instance *sh, int argc, char **argv);

#define NOR_SVC_BOARD_SUBCMDS                                                  \
	CLI_CMD_ARG(probe, NULL, "the board's own", nor_test_probe, 1, 0),

#endif /* NOR_SVC_CONFIG_H */
