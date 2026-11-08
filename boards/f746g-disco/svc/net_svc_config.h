/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    net_svc_config.h
 * @brief   What the shared `net` command has on STM32F746G-DISCO (issue #219).
 *
 * The board says what it HAS; the shared command (shell/cmds/cmd_net.c)
 * prints or registers under these and the adapter
 * (port/netxduo/net_svc_f746.c) answers the calls of svc/net/net_svc.h.
 * Properties, never a board identity test.  Every macro is defined, so a
 * reader sees the whole set and a missing one is a build error.
 */
#ifndef NET_SVC_CONFIG_H
#define NET_SVC_CONFIG_H

/** The `net` line of the top-level `help`. */
#define NET_SVC_HELP "NetX Duo IPv4 over the on-board Ethernet (link: see `eth`)"

/** 0: nothing marks a lease the link went down under (issue #220 gives both
 *  boards the mark and removes this property). */
#define NET_SVC_HAS_LEASE_STALE 0

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
 * `mjpeg`: MJPEG-over-HTTP on :80, a subscriber of the JPEG camera stream.
 */
#define NET_SVC_BOARD_DECLS                                                    \
	extern const struct cli_cmd net_board_mjpeg_subcmds[];

#define NET_SVC_BOARD_SUBCMDS                                                  \
	CLI_CMD(mjpeg, net_board_mjpeg_subcmds,                                \
	        "MJPEG-over-HTTP camera stream (start/stop/stats)", NULL),

#endif /* NET_SVC_CONFIG_H */
