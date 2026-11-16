/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    ext_ram_config.h
 * @brief   The shared external RAM command on STM32F746G-DISCO: `sdram`
 *          (issue #251; until then cmds/cmd_sdram.c was all of it).
 *
 * shell/cmds/cmd_ext_ram.c registers under these, and cmds/cmd_sdram.c is the
 * board's table (shell/cmds/ext_ram_cmd_board.h).  Every macro is defined, so
 * a missing one is a build error.
 */
#ifndef EXT_RAM_CONFIG_H
#define EXT_RAM_CONFIG_H

/** The command's name (a bare word: it is also the registration's). */
#define EXT_RAM_CMD  sdram

/** The `sdram` line of the top-level `help`. */
#define EXT_RAM_HELP "external FMC SDRAM (8 MB @0xC0000000)"

/** No subcommands of the board's own.  A name here that is also a shared
 *  subcommand would be silently shadowed; the board's host test refuses one. */
#define EXT_RAM_BOARD_DECLS
#define EXT_RAM_BOARD_SUBCMDS

#endif /* EXT_RAM_CONFIG_H */
