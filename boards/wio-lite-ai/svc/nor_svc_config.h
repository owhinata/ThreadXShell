/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    nor_svc_config.h
 * @brief   What the shared `nor` command has on Wio Lite AI (issue #250).
 *
 * The board says what it HAS; the shared command (shell/cmds/cmd_nor.c)
 * registers under these and cmds/cmd_nor_board.c answers svc/nor/nor_svc.h.
 * Every macro is defined, so a missing one is a build error.
 */
#ifndef NOR_SVC_CONFIG_H
#define NOR_SVC_CONFIG_H

/** The W25Q128JV on OCTOSPI2 is built with the KV store (BSP_ENABLE_KV);
 *  without it there is no `nor` (the same switch as `blob`). */
#if BSP_ENABLE_KV
#define NOR_SVC_HAS_DEVICE 1
#else
#define NOR_SVC_HAS_DEVICE 0
#endif

/** The `nor` line of the top-level `help`. */
#define NOR_SVC_HELP "external NOR flash (OCTOSPI2 / W25Q128JV)"

/** No subcommands of the board's own.  A name here that is also a shared
 *  subcommand would be silently shadowed; the board's host test refuses one. */
#define NOR_SVC_BOARD_DECLS
#define NOR_SVC_BOARD_SUBCMDS

#endif /* NOR_SVC_CONFIG_H */
