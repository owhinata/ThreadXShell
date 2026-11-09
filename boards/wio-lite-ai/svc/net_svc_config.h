/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    net_svc_config.h
 * @brief   What the shared `net` command has on Wio Lite AI (issue #219).
 *
 * The board says what it HAS; the shared command (shell/cmds/cmd_net.c)
 * prints or registers under these and the adapter (src/net_svc_wio.c)
 * answers the calls of svc/net/net_svc.h.  Properties, never a board
 * identity test.  Every macro is defined, so a reader sees the whole set and
 * a missing one is a build error.
 */
#ifndef NET_SVC_CONFIG_H
#define NET_SVC_CONFIG_H

/** The `net` line of the top-level `help`. */
#define NET_SVC_HELP "IPv4 (L3) over the onboard RTL8720 (info / ip / dhcp / ping)"

/*
 * The board's own subcommands, appended to the shared `net` table: a run of
 * CLI_CMD / CLI_CMD_ARG / CLI_CMD_ARG_USAGE entries, EACH FOLLOWED BY A COMMA,
 * expanded just before CLI_SUBCMD_SET_END.  Their handlers and tables are the
 * board's (cmds/cmd_net_board.c), declared by NET_SVC_BOARD_DECLS at file
 * scope after cli.h.
 *
 * [!] A NAME HERE THAT IS ALSO A SHARED SUBCOMMAND IS SILENTLY SHADOWED; the
 * board's host test builds the table and refuses a duplicate.
 *
 * None: the telnet console's `net shell start / stop / status` went with issue
 * #221 (the console is resident from boot and its state is a line of `net
 * info`).  Both stay defined, empty, so the shared command's table still
 * expands them.
 */
#define NET_SVC_BOARD_DECLS
#define NET_SVC_BOARD_SUBCMDS

#endif /* NET_SVC_CONFIG_H */
