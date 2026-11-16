/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    ext_ram_config.h
 * @brief   The shared external RAM command on Wio Lite AI: `psram` (issue #251;
 *          until then cmds/cmd_psram.c was all of it).
 *
 * shell/cmds/cmd_ext_ram.c registers under these, and cmds/cmd_psram.c is the
 * board's table (shell/cmds/ext_ram_cmd_board.h) and its tuning subcommands.
 * Built only with BSP_ENABLE_PSRAM (BOARD_HAS_EXT_RAM).  Every macro is
 * defined, so a missing one is a build error.
 */
#ifndef EXT_RAM_CONFIG_H
#define EXT_RAM_CONFIG_H

/** The command's name (a bare word: it is also the registration's). */
#define EXT_RAM_CMD  psram

/** The `psram` line of the top-level `help`. */
#define EXT_RAM_HELP "OCTOSPI1 APS6408 PSRAM info/test"

/*
 * The six tuning subcommands (cmds/cmd_psram.c), appended after the shared
 * `info` and `test` as they were (issue #251 moves them; #252 is where their
 * words change).  [!] A NAME HERE THAT IS ALSO A SHARED SUBCOMMAND IS SILENTLY
 * SHADOWED; the board's host test builds the table and refuses a duplicate.
 */
#define EXT_RAM_BOARD_DECLS                                                    \
	int cmd_psram_set(struct cli_instance *sh, int argc, char **argv);     \
	int cmd_psram_mr0(struct cli_instance *sh, int argc, char **argv);     \
	int cmd_psram_phase(struct cli_instance *sh, int argc, char **argv);   \
	int cmd_psram_mmapscan(struct cli_instance *sh, int argc, char **argv);\
	int cmd_psram_wtune(struct cli_instance *sh, int argc, char **argv);   \
	int cmd_psram_clk(struct cli_instance *sh, int argc, char **argv);

#define EXT_RAM_BOARD_SUBCMDS                                                  \
	CLI_CMD_ARG_USAGE(set,  NULL, "set mmap latency <rd> <wr> cycles",     \
	                  "<rd> <wr>", cmd_psram_set, 3, 0),                    \
	CLI_CMD_ARG_USAGE(mr0,  NULL, "write device MR0 latency reg <hex>",    \
	                  "<hex>", cmd_psram_mr0, 2, 0),                        \
	CLI_CMD_ARG_USAGE(phase, NULL, "set dlyb read phase <sel> [units]",    \
	                  "<sel> [units]", cmd_psram_phase, 2, 1),              \
	CLI_CMD_ARG_USAGE(mmapscan, NULL,                                      \
	                  "reset-persistent mmap DLYB sweep <start|show|stop>", \
	                  "start [phase] | show | stop", cmd_psram_mmapscan, 2, 1), \
	CLI_CMD_ARG_USAGE(wtune, NULL, "sweep mmap write dummy 0..15 [addr]",  \
	                  "[addr]", cmd_psram_wtune, 1, 1),                     \
	CLI_CMD_ARG_USAGE(clk,  NULL, "set device clock prescaler <0..255>",   \
	                  "<0..255>", cmd_psram_clk, 2, 0),

#endif /* EXT_RAM_CONFIG_H */
