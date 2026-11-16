/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Host stand-in for a board's ext_ram_config.h, for test_ext_ram_cmd.c (issue
 * #251): the command is `xram`, with one board subcommand, `tune`, so the
 * table's names can be checked with a board entry in it.  -DEXT_RAM_TEST_DUP
 * adds a board `info` too, which the test must refuse.
 */
#ifndef EXT_RAM_CONFIG_H
#define EXT_RAM_CONFIG_H

#define EXT_RAM_CMD  xram
#define EXT_RAM_HELP "the test's external RAM"

#define EXT_RAM_BOARD_DECLS                                                    \
	int xram_test_tune(struct cli_instance *sh, int argc, char **argv);

#ifdef EXT_RAM_TEST_DUP
#define EXT_RAM_BOARD_SUBCMDS                                                  \
	CLI_CMD_ARG(tune, NULL, "the board's own", xram_test_tune, 1, 0),      \
	CLI_CMD_ARG(info, NULL, "a board's info", xram_test_tune, 1, 0),
#else
#define EXT_RAM_BOARD_SUBCMDS                                                  \
	CLI_CMD_ARG(tune, NULL, "the board's own", xram_test_tune, 1, 0),
#endif

#endif /* EXT_RAM_CONFIG_H */
