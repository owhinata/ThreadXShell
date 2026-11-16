/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    nor_svc_config.h
 * @brief   What the shared `nor` command has on STM32F746G-DISCO (issue #250;
 *          until then this board's `qspi`).
 *
 * The board says what it HAS; the shared command (shell/cmds/cmd_nor.c)
 * registers under these and cmds/cmd_nor_board.c answers svc/nor/nor_svc.h.
 * Every macro is defined, so a missing one is a build error.
 */
#ifndef NOR_SVC_CONFIG_H
#define NOR_SVC_CONFIG_H

/** The N25Q128A on QUADSPI is always built. */
#define NOR_SVC_HAS_DEVICE 1

/** The `nor` line of the top-level `help`. */
#define NOR_SVC_HELP "QSPI NOR (N25Q128A)"

/** The shared subcommands this board has: no `nor write` (nothing here needs
 *  one); its code is not in this board's image (issue #250). */
#define NOR_SVC_HAS_READ  1
#define NOR_SVC_HAS_WRITE 0
#define NOR_SVC_HAS_TEST  1
#define NOR_SVC_NO_WRITE_SAYS ""

/*
 * The board's own subcommands, appended to the shared `nor` table: a run of
 * CLI_CMD_ARG / CLI_CMD_ARG_USAGE entries, EACH FOLLOWED BY A COMMA, declared
 * by NOR_SVC_BOARD_DECLS at file scope after cli.h.
 *
 * [!] A NAME HERE THAT IS ALSO A SHARED SUBCOMMAND IS SILENTLY SHADOWED; the
 * board's host test builds the table and refuses a duplicate.
 *
 * `probe`: read the JEDEC id and say whether it is the part this board has.
 */
#define NOR_SVC_BOARD_DECLS                                                    \
	int nor_board_probe(struct cli_instance *sh, int argc, char **argv);

#define NOR_SVC_BOARD_SUBCMDS                                                  \
	CLI_CMD_ARG(probe, NULL, "JEDEC id (expect 20 BA 18)",                 \
	            nor_board_probe, 1, 0),

#endif /* NOR_SVC_CONFIG_H */
