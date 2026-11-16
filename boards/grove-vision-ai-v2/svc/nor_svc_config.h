/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    nor_svc_config.h
 * @brief   What the shared `nor` command has on Grove Vision AI V2 (issue
 *          #250).
 *
 * The board says what it HAS; the shared command (shell/cmds/cmd_nor.c)
 * registers under these and cmds/cmd_nor_board.c answers svc/nor/nor_svc.h.
 * Every macro is defined, so a missing one is a build error.
 */
#ifndef NOR_SVC_CONFIG_H
#define NOR_SVC_CONFIG_H

/** The external QSPI NOR the bootloader lives on is always there. */
#define NOR_SVC_HAS_DEVICE 1

/** The `nor` line of the top-level `help`. */
#define NOR_SVC_HELP "the external QSPI NOR"

/** The shared subcommands this board has: no `nor read` (the flash is
 *  memory-mapped, `devmem dump` reads it under a lease) and no `nor test`
 *  (every `nor write` reads back what it wrote); their code is not in this
 *  board's image (issue #250). */
#define NOR_SVC_HAS_READ  0
#define NOR_SVC_HAS_WRITE 1
#define NOR_SVC_HAS_TEST  0
#define NOR_SVC_NO_READ_SAYS "`devmem dump 0x3A000000` reads it"
#define NOR_SVC_NO_TEST_SAYS "`nor write` verifies itself"

/*
 * The board's own subcommands, appended to the shared `nor` table: a run of
 * CLI_CMD_ARG_USAGE entries, EACH FOLLOWED BY A COMMA, declared by
 * NOR_SVC_BOARD_DECLS at file scope after cli.h.
 *
 * [!] A NAME HERE THAT IS ALSO A SHARED SUBCOMMAND IS SILENTLY SHADOWED; the
 * board's host test builds the table and refuses a duplicate.
 *
 * `scan`: walk the window and report what is not erased (reads only).
 * `cycle`: take the window down and bring it back, touching no data -- two
 * status-register writes, so it is a dangerous command like erase and write.
 */
#define NOR_SVC_BOARD_DECLS                                                    \
	int nor_board_scan(struct cli_instance *sh, int argc, char **argv);   \
	int nor_board_cycle(struct cli_instance *sh, int argc, char **argv);

#if CLI_ENABLE_DANGEROUS_CMDS
#define NOR_SVC_BOARD_CYCLE                                                    \
	CLI_CMD_ARG_USAGE(cycle, NULL, "window down and up, no data",             \
	                  NULL, nor_board_cycle, 1, 0),
#else
#define NOR_SVC_BOARD_CYCLE
#endif

#define NOR_SVC_BOARD_SUBCMDS                                                  \
	CLI_CMD_ARG_USAGE(scan, NULL, "report non-erased extents",                \
	                  NULL, nor_board_scan, 1, 0),                         \
	NOR_SVC_BOARD_CYCLE

#endif /* NOR_SVC_CONFIG_H */
